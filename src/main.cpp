/*
 * ESP-TamaCore
 * Native TamaEmu frontend for Waveshare ESP32-S3-LCD-1.3
 *
 * Target:
 *   ESP32-S3R8
 *   240 MHz Xtensa LX7 dual core
 *   8 MB OPI PSRAM
 *   16 MB Flash
 *
 * Display:
 *   ST7789V2 240x240
 *   MOSI = GPIO41
 *   SCLK = GPIO40
 *   CS   = GPIO39
 *   DC   = GPIO38
 *   RST  = GPIO42
 *   BL   = GPIO20
 *
 * Emulator core:
 *   maragotchi/tamaemu
 *
 * P's memory map:
 *   ROM       0x02000000 - 8 MB
 *   A0RAM     0x00000000 - 32 KB
 *   IVRAM     0x00080000 - 12 KB
 *   DSTRAM    0x00084000 -  4 KB
 *   IO        0x00300000 -  8 KB
 *   LCD CMD   0x00600000
 *   LCD DATA  0x00600001
 *
 * The complete Emu structure is allocated with ps_malloc().
 * This places TamaEmu's internal RAM, LCD GRAM and most emulator
 * state in external PSRAM.
 *
 * The original 8 MB ROM remains memory-mapped in ESP32 Flash.
 */

#include <Arduino.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


// ============================================================================
// P's ROM raw Flash partition
// ============================================================================
//
// The 8 MiB ROM is flashed separately into the "tamarom" partition at
// 0x310000. Keeping it outside firmware.bin avoids an 8+ MiB application
// image during boot and lets the ESP32 map the ROM directly from Flash.
// ============================================================================

static const esp_partition_t *rom_partition = nullptr;


// ============================================================================
// TFT_eSPI configuration validation
// ============================================================================
//
// These values should already come from platformio.ini.
// Keeping the checks here avoids silently compiling for the wrong wiring.
// ============================================================================

#ifndef TFT_MOSI
#error "TFT_MOSI is not defined"
#endif

#ifndef TFT_SCLK
#error "TFT_SCLK is not defined"
#endif

#ifndef TFT_CS
#error "TFT_CS is not defined"
#endif

#ifndef TFT_DC
#error "TFT_DC is not defined"
#endif

#ifndef TFT_RST
#error "TFT_RST is not defined"
#endif


#if TFT_MOSI != 41
#error "Waveshare ESP32-S3-LCD-1.3 requires TFT_MOSI = GPIO41"
#endif

#if TFT_SCLK != 40
#error "Waveshare ESP32-S3-LCD-1.3 requires TFT_SCLK = GPIO40"
#endif

#if TFT_CS != 39
#error "Waveshare ESP32-S3-LCD-1.3 requires TFT_CS = GPIO39"
#endif

#if TFT_DC != 38
#error "Waveshare ESP32-S3-LCD-1.3 requires TFT_DC = GPIO38"
#endif

#if TFT_RST != 42
#error "Waveshare ESP32-S3-LCD-1.3 requires TFT_RST = GPIO42"
#endif


#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY 40000000
#endif

#include <TFT_eSPI.h>


// ============================================================================
// TamaEmu core
// ============================================================================
//
// Recommended project layout:
//
// src/
// ├── main.cpp
// └── tamaemu/
//     ├── emu.h
//     ├── cpu.c
//     ├── mem.c
//     ├── lcd.c
//     ├── device.c
//     ├── periph.c
//     └── ...
//
// The original core is C, therefore declarations must use C linkage.
// ============================================================================

extern "C"
{
#if __has_include("tamaemu/emu.h")
#include "tamaemu/emu.h"

#elif __has_include("emu.h")
#include "emu.h"

#else
#error \
"TamaEmu core not found. Add maragotchi/tamaemu core files under src/tamaemu/"
#endif
}


// ============================================================================
// Board constants
// ============================================================================

static constexpr int BOARD_LCD_WIDTH  = 240;
static constexpr int BOARD_LCD_HEIGHT = 240;

static constexpr int BOARD_LCD_BL = 20;


// TamaEmu P's visible LCD is 128x128.
// Internal LCD controller GRAM is 132x162.

static constexpr int EMU_SCREEN_WIDTH  = 128;
static constexpr int EMU_SCREEN_HEIGHT = 128;

static constexpr int EMU_GRAM_WIDTH  = 132;
static constexpr int EMU_GRAM_HEIGHT = 162;


// Center Tamagotchi's native 128x128 image on the physical 240x240 LCD.

static constexpr int DISPLAY_X =
    (BOARD_LCD_WIDTH - EMU_SCREEN_WIDTH) / 2;

static constexpr int DISPLAY_Y =
    (BOARD_LCD_HEIGHT - EMU_SCREEN_HEIGHT) / 2;


// Tamagotchi P's NOR size.

static constexpr size_t EXPECTED_ROM_SIZE =
    0x00800000UL; // 8 MiB


// ============================================================================
// Emulator execution configuration
// ============================================================================

// Original P's OSC3 frequency from TamaEmu.

static constexpr double FALLBACK_CPU_HZ =
    18432000.0;


// Update TFT at a maximum of 60 frames/s.

static constexpr uint32_t VIDEO_FPS = 60;

static constexpr uint32_t VIDEO_PERIOD_US =
    1000000UL / VIDEO_FPS;


// TamaEmu desktop code advances peripherals after roughly 256 emulated cycles.

static constexpr uint64_t PERIPHERAL_TICK_INTERVAL =
    256;


// Prevent one Arduino loop() call from monopolising the CPU forever.

static constexpr uint32_t MAX_CPU_STEPS_PER_PASS =
    20000;


// Maximum real-time debt retained by scheduler.
//
// If emulation momentarily stalls because of Serial/TFT/etc., we don't want
// the ESP32 spending seconds trying to catch up.

static constexpr double MAX_SCHEDULER_LAG_SECONDS =
    0.050;


// ============================================================================
// Globals
// ============================================================================

static TFT_eSPI tft = TFT_eSPI();


// Entire TamaEmu object lives in PSRAM.

static Emu *emu = nullptr;


// Temporary RGB565 scanline.
//
// 128 pixels * 2 bytes = only 256 bytes, so keeping this tiny hot transfer
// buffer in internal SRAM is faster than putting it in PSRAM.

static uint16_t scanline[EMU_SCREEN_WIDTH];


// Scheduler state.

static uint64_t scheduler_last_us = 0;

static uint64_t scheduler_target_cycles = 0;

static double scheduler_fractional_cycles = 0.0;


// Video state.

static uint64_t next_video_us = 0;

static uint64_t last_ramwr_bytes = UINT64_MAX;

static bool last_display_on = false;

static bool last_sleep_out = false;

static bool last_inverted = false;

static uint8_t last_madctl = 0;


// Performance telemetry.

static uint64_t perf_last_us = 0;

static uint64_t perf_last_cycles = 0;


// Fatal state.

static bool emulator_failed = false;


// ============================================================================
// Fatal error helper
// ============================================================================

static void fatal_error(const char *message)
{
    emulator_failed = true;

    Serial.println();
    Serial.println("========================================");
    Serial.println("[FATAL] ESP-TamaCore");
    Serial.println(message);
    Serial.println("========================================");

    tft.fillScreen(TFT_RED);

    tft.setTextColor(TFT_WHITE, TFT_RED);

    tft.setTextDatum(MC_DATUM);

    tft.drawString(
        "ESP-TamaCore",
        BOARD_LCD_WIDTH / 2,
        BOARD_LCD_HEIGHT / 2 - 24,
        2
    );

    tft.drawString(
        "FATAL ERROR",
        BOARD_LCD_WIDTH / 2,
        BOARD_LCD_HEIGHT / 2,
        2
    );
}


// ============================================================================
// PSRAM
// ============================================================================

static bool initialise_psram()
{
    Serial.println("[PSRAM] checking external RAM...");

    if (!psramFound())
    {
        fatal_error(
            "8 MB PSRAM was not detected."
        );

        return false;
    }

    const size_t total =
        ESP.getPsramSize();

    const size_t free_now =
        ESP.getFreePsram();

    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    Serial.printf(
        "[PSRAM] total   : %u bytes\n",
        static_cast<unsigned>(total)
    );

    Serial.printf(
        "[PSRAM] free    : %u bytes\n",
        static_cast<unsigned>(free_now)
    );

    Serial.printf(
        "[PSRAM] largest : %u bytes\n",
        static_cast<unsigned>(largest)
    );

    if (total == 0)
    {
        fatal_error(
            "PSRAM reported zero bytes."
        );

        return false;
    }

    return true;
}


// ============================================================================
// Embedded ROM validation
// ============================================================================

static bool initialise_rom()
{
    Serial.println("[ROM] locating raw tamarom partition...");

    rom_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        static_cast<esp_partition_subtype_t>(0x40),
        "tamarom"
    );

    if (rom_partition == nullptr)
    {
        fatal_error(
            "Raw tamarom partition was not found."
        );

        return false;
    }

    Serial.printf(
        "[ROM] partition offset : 0x%08X\n",
        static_cast<unsigned>(rom_partition->address)
    );

    Serial.printf(
        "[ROM] partition size   : %u bytes\n",
        static_cast<unsigned>(rom_partition->size)
    );

    if (rom_partition->size < EXPECTED_ROM_SIZE)
    {
        fatal_error(
            "tamarom partition is smaller than 8 MB."
        );

        return false;
    }

    uint8_t header[32] = {};

    const esp_err_t err = esp_partition_read(
        rom_partition,
        0,
        header,
        sizeof(header)
    );

    if (err != ESP_OK)
    {
        Serial.printf(
            "[ROM] partition read failed: %d\n",
            static_cast<int>(err)
        );

        fatal_error(
            "Could not read P's ROM partition."
        );

        return false;
    }

    bool looks_erased = true;

    for (size_t i = 0; i < sizeof(header); ++i)
    {
        if (header[i] != 0xFF)
        {
            looks_erased = false;
            break;
        }
    }

    if (looks_erased)
    {
        fatal_error(
            "tamarom partition appears erased; ps.bin was not flashed."
        );

        return false;
    }

    Serial.println(
        "[ROM] raw Flash partition ready; PSRAM cache will load pages on demand"
    );

    return true;
}


// ============================================================================
// Display
// ============================================================================

static void initialise_display_hardware()
{
    // Bring the LCD up before Serial, PSRAM, ROM validation or emulator state.
    // This gives us a visible diagnostic surface even if later init fails.

    pinMode(
        BOARD_LCD_BL,
        OUTPUT
    );

    digitalWrite(
        BOARD_LCD_BL,
        HIGH
    );

    tft.init();
    tft.setRotation(0);

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.drawString(
        "LCD OK",
        BOARD_LCD_WIDTH / 2,
        BOARD_LCD_HEIGHT / 2,
        2
    );
}


static void show_startup_screen()
{
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(MC_DATUM);

    tft.setTextColor(
        TFT_WHITE,
        TFT_BLACK
    );

    tft.drawString(
        "ESP-TamaCore",
        BOARD_LCD_WIDTH / 2,
        BOARD_LCD_HEIGHT / 2 - 16,
        2
    );

    tft.drawString(
        "Starting...",
        BOARD_LCD_WIDTH / 2,
        BOARD_LCD_HEIGHT / 2 + 16,
        2
    );

    Serial.println(
        "[LCD] ST7789 initialised"
    );

    Serial.printf(
        "[LCD] Tama viewport %dx%d at (%d,%d)\n",
        EMU_SCREEN_WIDTH,
        EMU_SCREEN_HEIGHT,
        DISPLAY_X,
        DISPLAY_Y
    );
}


// ============================================================================
// RGB565 helpers
// ============================================================================

static inline uint16_t rgb565_swap_red_blue(
    uint16_t value
)
{
    const uint16_t red =
        static_cast<uint16_t>(
            (value >> 11) & 0x1F
        );

    const uint16_t green =
        static_cast<uint16_t>(
            (value >> 5) & 0x3F
        );

    const uint16_t blue =
        static_cast<uint16_t>(
            value & 0x1F
        );


    return static_cast<uint16_t>(
        (blue << 11) |
        (green << 5) |
        red
    );
}


// ============================================================================
// TamaEmu -> ST7789
// ============================================================================

static void push_tama_frame()
{
    if (emu == nullptr)
    {
        return;
    }


    Lcd &lcd =
        emu->lcd;


    // TamaEmu renders black while the emulated LCD is asleep/off.

    if (
        (!lcd.sleep_out || !lcd.disp_on) &&
        !emu->stay_awake
    )
    {
        if (
            last_display_on ||
            last_sleep_out
        )
        {
            tft.fillRect(
                DISPLAY_X,
                DISPLAY_Y,
                EMU_SCREEN_WIDTH,
                EMU_SCREEN_HEIGHT,
                TFT_BLACK
            );
        }

        return;
    }


    const bool invert =
        lcd.inverted;


    // TamaEmu's lcd_render() interprets bit 3 as BGR.

    const bool bgr =
        (lcd.madctl & 0x08) != 0;


    tft.startWrite();

    tft.setAddrWindow(
        DISPLAY_X,
        DISPLAY_Y,
        EMU_SCREEN_WIDTH,
        EMU_SCREEN_HEIGHT
    );


    // Fast path:
    //
    // TamaEmu already stores its GRAM as RGB565.
    //
    // If no colour transformation is needed we can transfer each 128-pixel
    // source row directly from emulated LCD GRAM to the ST7789.

    if (!invert && !bgr)
    {
        for (
            int y = 0;
            y < EMU_SCREEN_HEIGHT;
            ++y
        )
        {
            uint16_t *src =
                &lcd.gram[y][0];


            tft.pushColors(
                src,
                EMU_SCREEN_WIDTH,
                true
            );
        }
    }

    else
    {
        // Slow path only when TamaEmu requests inversion/BGR conversion.

        for (
            int y = 0;
            y < EMU_SCREEN_HEIGHT;
            ++y
        )
        {
            const uint16_t *src =
                &lcd.gram[y][0];


            for (
                int x = 0;
                x < EMU_SCREEN_WIDTH;
                ++x
            )
            {
                uint16_t pixel =
                    src[x];


                if (invert)
                {
                    pixel =
                        static_cast<uint16_t>(
                            ~pixel
                        );
                }


                if (bgr)
                {
                    pixel =
                        rgb565_swap_red_blue(
                            pixel
                        );
                }


                scanline[x] =
                    pixel;
            }


            tft.pushColors(
                scanline,
                EMU_SCREEN_WIDTH,
                true
            );
        }
    }


    tft.endWrite();
}


// ============================================================================
// Video scheduler
// ============================================================================

static void update_video()
{
    if (emu == nullptr)
    {
        return;
    }


    const uint64_t now =
        esp_timer_get_time();


    if (now < next_video_us)
    {
        return;
    }


    next_video_us =
        now + VIDEO_PERIOD_US;


    const Lcd &lcd =
        emu->lcd;


    const bool visual_state_changed =
        lcd.disp_on    != last_display_on ||
        lcd.sleep_out  != last_sleep_out ||
        lcd.inverted   != last_inverted ||
        lcd.madctl     != last_madctl;


    const bool pixel_data_changed =
        lcd.ramwr_bytes != last_ramwr_bytes;


    // Skip SPI transfer if the emulated LCD hasn't changed.

    if (
        !visual_state_changed &&
        !pixel_data_changed
    )
    {
        return;
    }


    push_tama_frame();


    last_ramwr_bytes =
        lcd.ramwr_bytes;

    last_display_on =
        lcd.disp_on;

    last_sleep_out =
        lcd.sleep_out;

    last_inverted =
        lcd.inverted;

    last_madctl =
        lcd.madctl;
}


// ============================================================================
// Emulator initialisation
// ============================================================================

static bool initialise_emulator()
{
    Serial.println(
        "[EMU] allocating TamaEmu state in PSRAM..."
    );


    // This is deliberate.
    //
    // Emu contains:
    //
    //   CPU registers/state
    //   A0RAM
    //   IVRAM
    //   DSTRAM
    //   IORAM
    //   LCD GRAM
    //   timer state
    //   trace buffers
    //   peripheral state
    //
    // Allocating the complete structure using ps_malloc() moves those
    // inline arrays into external RAM without rewriting the original
    // TamaEmu data layout.

    emu =
        static_cast<Emu *>(
            ps_malloc(
                sizeof(Emu)
            )
        );


    if (emu == nullptr)
    {
        fatal_error(
            "ps_malloc(sizeof(Emu)) failed."
        );

        return false;
    }


    memset(
        emu,
        0,
        sizeof(Emu)
    );


    Serial.printf(
        "[EMU] Emu size : %u bytes\n",
        static_cast<unsigned>(
            sizeof(Emu)
        )
    );

    Serial.printf(
        "[EMU] Emu ptr  : %p\n",
        emu
    );

    Serial.printf(
        "[PSRAM] free after Emu allocation: %u bytes\n",
        static_cast<unsigned>(
            ESP.getFreePsram()
        )
    );


    // ------------------------------------------------------------------------
    // Select TamaEmu's real P's device profile.
    // ------------------------------------------------------------------------

    const DeviceProfile *device =
        device_find("ps");


    if (device == nullptr)
    {
        fatal_error(
            "TamaEmu device profile 'ps' was not found."
        );

        return false;
    }


    emu->dev =
        *device;


    // ------------------------------------------------------------------------
    // ROM
    // ------------------------------------------------------------------------
    //
    // Original TamaEmu declares:
    //
    //     uint8_t *rom;
    //
    // The 8 MiB ROM lives in the dedicated tamarom Flash partition.
    // mem.c loads 64 KiB pages into a 2 MiB PSRAM cache on demand.
    //
    // IMPORTANT:
    // Do not allow original TamaEmu flash_write() to directly modify this
    // pointer. ESP32 program Flash is read-only through its mapped address.
    //
    // A sparse sector overlay will be added to mem.c for persistent saves.
    // ------------------------------------------------------------------------

    // ROM reads are served by tamaemu/mem.c from the dedicated tamarom
    // partition through a 2 MiB PSRAM page cache.
    emu->rom =
        nullptr;


    emu->cmu.osc3_hz =
        device->osc3_hz;


    emu->rtc_mult =
        1;


    emu->core_id =
        0;


    // ------------------------------------------------------------------------
    // Native S1C33 reset.
    //
    // cpu_reset() obtains the reset vector through TamaEmu's own memory
    // decoder, therefore this immediately validates that rom_start is being
    // used by the Epson CPU core.
    // ------------------------------------------------------------------------

    cpu_reset(
        emu
    );


    emu->last_tick =
        emu->cycles;


    Serial.printf(
        "[EMU] device   : %s\n",
        emu->dev.title
    );

    Serial.printf(
        "[EMU] ROM base : 0x%08X\n",
        static_cast<unsigned>(
            emu->dev.rom_base
        )
    );

    Serial.printf(
        "[EMU] ROM size : %u\n",
        static_cast<unsigned>(
            emu->dev.rom_size
        )
    );

    Serial.printf(
        "[EMU] OSC3     : %.0f Hz\n",
        emu->dev.osc3_hz
    );

    Serial.printf(
        "[CPU] reset PC : 0x%08X\n",
        static_cast<unsigned>(
            emu->pc
        )
    );


    return true;
}


// ============================================================================
// Emulator clock
// ============================================================================

static inline double emulator_clock_hz()
{
    if (emu == nullptr)
    {
        return FALLBACK_CPU_HZ;
    }


    // TamaEmu's CMU updates mclk_hz when firmware changes PLL/clock settings.

    if (emu->cmu.mclk_hz > 1.0)
    {
        return emu->cmu.mclk_hz;
    }


    if (emu->dev.osc3_hz > 1.0)
    {
        return emu->dev.osc3_hz;
    }


    return FALLBACK_CPU_HZ;
}


// ============================================================================
// Peripheral update
// ============================================================================

static inline void update_emulated_peripherals()
{
    const uint64_t elapsed =
        emu->cycles -
        emu->last_tick;


    if (
        elapsed <
        PERIPHERAL_TICK_INTERVAL
    )
    {
        return;
    }


    periph_tick(
        emu,
        static_cast<uint32_t>(
            elapsed
        )
    );


    emu->last_tick =
        emu->cycles;


    // Buttons not wired yet.
    //
    // TamaEmu:
    //
    // bit 0 = A
    // bit 1 = B
    // bit 2 = C
    //
    // Later this will be replaced by the physical button input layer.

    periph_buttons(
        emu,
        0
    );
}


// ============================================================================
// 1x real-time scheduler
// ============================================================================

static void reset_scheduler()
{
    scheduler_last_us =
        esp_timer_get_time();


    scheduler_target_cycles =
        emu != nullptr
            ? emu->cycles
            : 0;


    scheduler_fractional_cycles =
        0.0;


    next_video_us =
        scheduler_last_us;


    perf_last_us =
        scheduler_last_us;


    perf_last_cycles =
        emu != nullptr
            ? emu->cycles
            : 0;
}


static void run_emulator_1x()
{
    if (
        emu == nullptr ||
        emu->stopped ||
        emulator_failed
    )
    {
        return;
    }


    const uint64_t now =
        esp_timer_get_time();


    uint64_t elapsed_us =
        now - scheduler_last_us;


    scheduler_last_us =
        now;


    // If debugger/Serial halted execution for a long time, do not accumulate
    // an enormous amount of emulation work.

    if (elapsed_us > 100000)
    {
        elapsed_us =
            100000;
    }


    const double clock_hz =
        emulator_clock_hz();


    const double generated_cycles =
        (
            static_cast<double>(
                elapsed_us
            ) *
            clock_hz /
            1000000.0
        ) +
        scheduler_fractional_cycles;


    const uint64_t whole_cycles =
        static_cast<uint64_t>(
            generated_cycles
        );


    scheduler_fractional_cycles =
        generated_cycles -
        static_cast<double>(
            whole_cycles
        );


    scheduler_target_cycles +=
        whole_cycles;


    // Bound catch-up debt to approximately 50 ms.

    const uint64_t max_lag_cycles =
        static_cast<uint64_t>(
            clock_hz *
            MAX_SCHEDULER_LAG_SECONDS
        );


    const uint64_t max_target =
        emu->cycles +
        max_lag_cycles;


    if (
        scheduler_target_cycles >
        max_target
    )
    {
        scheduler_target_cycles =
            max_target;
    }


    uint32_t executed_steps =
        0;


    while (
        emu->cycles <
            scheduler_target_cycles &&

        executed_steps <
            MAX_CPU_STEPS_PER_PASS &&

        !emu->stopped
    )
    {
        // Native TamaEmu S1C33 interpreter.

        cpu_step(
            emu
        );


        update_emulated_peripherals();


        ++executed_steps;


        // Diagnostic watchdog-safe build:
        // yield one FreeRTOS tick after each simulated CPU step so the
        // Arduino loop task cannot starve the RTOS/watchdog.
        //
        // This intentionally sacrifices emulation speed for stability.
        vTaskDelay(1);
    }


    if (emu->stopped)
    {
        Serial.println();
        Serial.println(
            "[CPU] TamaEmu stopped."
        );

        Serial.printf(
            "[CPU] PC     : 0x%08X\n",
            static_cast<unsigned>(
                emu->pc
            )
        );

        Serial.printf(
            "[CPU] cycles : %llu\n",
            static_cast<unsigned long long>(
                emu->cycles
            )
        );

        Serial.printf(
            "[CPU] reason : %s\n",
            emu->stop_reason
        );


        fatal_error(
            "TamaEmu CPU stopped."
        );
    }
}


// ============================================================================
// Performance monitor
// ============================================================================

static void update_performance_monitor()
{
    if (
        emu == nullptr ||
        emulator_failed
    )
    {
        return;
    }


    const uint64_t now =
        esp_timer_get_time();


    const uint64_t elapsed_us =
        now - perf_last_us;


    if (elapsed_us < 1000000)
    {
        return;
    }


    const uint64_t cycle_delta =
        emu->cycles -
        perf_last_cycles;


    const double seconds =
        static_cast<double>(
            elapsed_us
        ) /
        1000000.0;


    const double emulated_hz =
        static_cast<double>(
            cycle_delta
        ) /
        seconds;


    const double target_hz =
        emulator_clock_hz();


    const double speed =
        target_hz > 0.0
            ? emulated_hz /
                target_hz
            : 0.0;


    Serial.printf(
        "[PERF] %.3fx | "
        "%.2f Mcy/s | "
        "target %.2f MHz | "
        "PC=%08X | "
        "PSRAM=%u\n",

        speed,

        emulated_hz /
            1000000.0,

        target_hz /
            1000000.0,

        static_cast<unsigned>(
            emu->pc
        ),

        static_cast<unsigned>(
            ESP.getFreePsram()
        )
    );


    perf_last_us =
        now;


    perf_last_cycles =
        emu->cycles;
}


// ============================================================================
// setup()
// ============================================================================

void setup()
{
    // Serial first for crash diagnostics. The previous build proved that
    // TFT_eSPI::init() itself can fault before any visible LCD output.
    Serial.begin(
        115200
    );

    delay(
        50
    );

    Serial.println();
    Serial.println("[BOOT] Arduino setup entered");
    Serial.println("[BOOT] Initialising ST7789 on HSPI/SPI3...");

    initialise_display_hardware();

    Serial.println("[BOOT] TFT initialisation returned successfully");


    Serial.println();
    Serial.println(
        "========================================"
    );

    Serial.println(
        " ESP-TamaCore"
    );

    Serial.println(
        " Waveshare ESP32-S3-LCD-1.3"
    );

    Serial.println(
        " TamaEmu S1C33 native frontend"
    );

    Serial.println(
        " ROM source: raw Flash partition"
    );

    Serial.println(
        "========================================"
    );


    show_startup_screen();


    if (!initialise_psram())
    {
        return;
    }


    if (!initialise_rom())
    {
        return;
    }


    if (!initialise_emulator())
    {
        return;
    }


    // Clear boot splash before the emulated LCD becomes active.

    tft.fillScreen(
        TFT_BLACK
    );


    reset_scheduler();


    Serial.println();
    Serial.println(
        "[BOOT] ESP-TamaCore running."
    );

    Serial.println(
        "[BOOT] Scheduler target: 1.000x."
    );

    Serial.println(
        "[BOOT] Waiting for P's firmware..."
    );
}


// ============================================================================
// loop()
// ============================================================================

void loop()
{
    if (emulator_failed)
    {
        delay(
            1000
        );

        return;
    }


    // ------------------------------------------------------------------------
    // S1C33 CPU
    // ------------------------------------------------------------------------
    //
    // Runs as one tight native batch until emulated time catches real time.
    //
    // No SD.
    // No filesystem.
    // No delay() in the CPU hot path.
    // No per-instruction TFT operation.
    // ------------------------------------------------------------------------

    run_emulator_1x();


    // ------------------------------------------------------------------------
    // ST7789
    // ------------------------------------------------------------------------
    //
    // Only transfers the emulated display when TamaEmu changed LCD RAM/state.
    //
    // RGB565 goes directly to TFT_eSPI pushColors().
    // ------------------------------------------------------------------------

    update_video();


    // ------------------------------------------------------------------------
    // Telemetry
    // ------------------------------------------------------------------------

    update_performance_monitor();


    // Second watchdog safety net. The CPU interpreter already yields inside
    // run_emulator_1x(), but this guarantees FreeRTOS also gets time between
    // complete Arduino loop passes.
    vTaskDelay(1);
}
