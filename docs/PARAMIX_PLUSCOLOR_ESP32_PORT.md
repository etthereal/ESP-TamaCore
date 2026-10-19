# ESP-TamaCore: experimental Plus Color / Paramix-inspired port

This branch adds an experimental Tamagotchi Plus Color target to ESP-TamaCore for the Waveshare ESP32-S3-LCD-1.3 (ESP32-S3R8, 240 MHz, 8 MiB PSRAM, 16 MiB flash).

It reuses the TamaEmu S1C33 interpreter and the plus-color device profile. It does not run Paramix Studio's Paradise-specific emu.bin: that binary targets the Paradise's ARM Thumb-2 CPU and hardware, not the ESP32-S3 Xtensa LX7.

## Flash-backed ROM design

- Build target: pio run -e pluscolor_flash
- Firmware input: the user's own legal dump, flashed at offset 0x310000 into the tamarom partition.
- Partition capacity: 8 MiB, enough for raw 4 MiB or 8 MiB images when the selected device profile expects that size.
- Runtime: the ROM remains in the ESP32's 16 MiB flash. The existing memory layer reads 64 KiB pages into a small PSRAM cache on demand. It does not copy the whole ROM to PSRAM and does not use the SD card.
- The guest firmware sees an emulated NOR chip. Writes/erases currently use a PSRAM copy-on-write overlay. Those changes are volatile and are lost on power-off until a persistent overlay/save backend is added.
- The existing P's environment remains the default and uses the same flash-backed page-cache approach.

This is conceptually similar to Paramix's flash-resident guest firmware, but it is not a byte-for-byte port of Paramix's packing format. Paramix skips unused 64 KiB blocks and keeps spare pages for guest writes; ESP-TamaCore currently stores a raw ROM image and caches reads. Packing sparse ROM blocks and persisting write overlays are future steps.

## Flashing the ROM

The tamarom partition begins at 0x310000 and is 0x800000 bytes long in partitions.csv. Flash a raw ROM image to this partition using the appropriate ESP32 flash tool, preserving the application and partition table. Do not flash an entire 16 MiB image over the device unless it was deliberately built for this exact partition layout.

The Plus Color profile currently expects a 4 MiB image. The 8 MiB partition capacity does not automatically make an 8 MiB firmware compatible with a profile configured for 4 MiB; the device profile and ROM size must match. P's uses an 8 MiB profile.

## What is not done yet

1. Not compiled or hardware-tested in this commit.
2. Guest flash writes are not persistent across power cycles.
3. Buttons and GPIO-IR are not wired to physical hardware yet.
4. Audio output is not implemented by the current frontend.
5. Real-time performance is unknown. The Paradise port's JIT cannot be reused directly because it emits ARM Thumb-2 code; this target uses TamaEmu's native interpreter.

## Next validation steps

1. Build with PlatformIO: pio run -e pluscolor_flash.
2. Flash the application, then write the user's own 4 MiB Plus Color dump to the tamarom partition at offset 0x310000.
3. Confirm the serial log detects the raw flash partition and the Plus Color device profile.
4. Verify the reset vector, startup and LCD activity.
5. Measure emulation speed; then add persistent save/overlay storage.

## Upstream references

- Paramix Studio: https://github.com/mameise/Paramix-Studio
- TamaEmu: https://github.com/maragotchi/tamaemu
- Waveshare ESP32-S3-LCD-1.3: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3
