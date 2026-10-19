# ESP-TamaCore: experimental Plus Color / Paramix-inspired port

This branch adds an experimental Tamagotchi Plus Color target to ESP-TamaCore for the Waveshare ESP32-S3-LCD-1.3 (ESP32-S3R8, 240 MHz, 8 MiB PSRAM, 16 MiB flash).

It reuses the existing TamaEmu S1C33 interpreter and the `plus-color` device profile. It does **not** run Paramix Studio's Paradise-specific `emu.bin`: that binary is compiled for the Paradise's ARM Thumb-2 CPU and hardware, not the ESP32-S3 Xtensa LX7.

## Current design

- Build target: `pio run -e pluscolor_sd`
- Firmware input: user-supplied legal dump `/pluscolor.bin` on the TF/microSD card.
- Exact ROM size: 4 MiB (4,194,304 bytes).
- Boot: initialise SD, allocate a 4 MiB PSRAM buffer, copy the ROM in 32 KiB sequential reads, close the file and call `SD.end()`.
- Runtime: `mem.c` reads guest ROM bytes directly from `Emu.rom` in PSRAM. It does not read from SD or the ESP partition on the CPU instruction-fetch/memory-read path.
- Writes to the guest's emulated NOR use a PSRAM copy-on-write sector overlay. In this prototype, persistence of those overlays across power cycles is **not implemented yet**; do not treat this build as a save-safe daily driver.
- The existing P's build remains the default environment and keeps its raw-flash/2 MiB cache path.

## SD pin configuration

The initial defaults are MOSI=GPIO11, MISO=GPIO13, SCK=GPIO12, CS=GPIO10. These are compile-time macros in `platformio.ini` and can be changed. Confirm the pinout against the exact ESP32-S3-LCD-1.3 board revision before powering the board; the TFT frontend uses HSPI while this target uses FSPI for SD.

Format the card as FAT32 and put your own 4 MiB dump at the root as `pluscolor.bin`. Do not download or redistribute copyrighted firmware dumps.

## What is not done yet

1. **Not hardware-tested in this commit.** The build must be compiled and run on the actual board.
2. The Plus Color ROM's guest flash writes are volatile until a persistent overlay/save backend is added.
3. Buttons and GPIO-IR are not wired to physical hardware yet.
4. Audio output is not implemented by the current frontend.
5. Performance at real-time speed is unknown; the Paradise port's JIT cannot be reused directly because it emits ARM Thumb-2 code. This target uses TamaEmu's native interpreter.

## Next validation steps

1. Build with PlatformIO.
2. Confirm SD initialises and reports exactly 4,194,304 bytes.
3. Confirm the log shows PSRAM remaining after ROM load and `device: Tamagotchi Plus Color`.
4. Verify reset vector / early boot and LCD activity.
5. Measure emulation speed before implementing the save overlay.

## Upstream references

- Paramix Studio: https://github.com/mameise/Paramix-Studio
- TamaEmu: https://github.com/maragotchi/tamaemu
- Waveshare ESP32-S3-LCD-1.3: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3
