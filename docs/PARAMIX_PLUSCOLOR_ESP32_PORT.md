# Paramix-inspired colour Tamagotchi emulation on ESP32-S3

This is an experimental ESP-TamaCore adaptation for Waveshare ESP32-S3-LCD-1.3 (16 MiB Flash, 8 MiB PSRAM). It uses the portable TamaEmu S1C33 emulator core that underlies Paramix Studio, rather than trying to execute Paramix's Paradise-specific `emu.bin` on the ESP32.

## Why this is not a direct Paramix binary port

Paramix's `emu.bin` is compiled for the Tamagotchi Paradise's ARM Thumb-2 CPU and hardware. The ESP32-S3 uses Xtensa LX7, so that binary cannot be executed as-is. The reusable part is the shared S1C33 guest emulator, device profiles and ROM-handling concepts. The Paradise-specific JIT would need a separate Xtensa implementation to be ported.

## SD-backed builds

PlatformIO environments added in this branch:

- `pio run -e ps_sd`: P's; place the user's own raw 8 MiB firmware dump at `/ps.bin` on a FAT-formatted TF card.
- `pio run -e 4uplus_sd`: 4U+; place the user's own raw 8 MiB firmware dump at `/4uplus.bin`.
- `pio run -e 4u_sd`: original 4U; place the user's own raw 8 MiB firmware dump at `/4u.bin`.
- `pio run -e pluscolor_flash`: Plus Color; 4 MiB ROM remains in the `tamarom` flash partition.

The SD-backed builds read the ROM in 64 KiB pages into a 32-slot PSRAM cache (up to 2 MiB), rather than copying the entire 8 MiB image into PSRAM. This keeps the ROM on the TF card and leaves PSRAM available for emulator state and cache. The SD interface is initialized with GPIO MOSI=11, MISO=13, SCK=12, CS=10; confirm these pins against the exact board revision if card initialization fails.

## Important limitations

- Experimental and not yet compiled or hardware-tested in this branch.
- SD page-cache misses perform a seek and 64 KiB read during emulation. Performance and timing stability must be measured on hardware; the cache is a first functional design, not a claim of real-time performance.
- Guest flash writes currently use a volatile copy-on-write overlay in PSRAM. Saves and firmware changes are lost on power-off. A persistent save/overlay file on SD is a required next step.
- The ROM dump is not included. Use a firmware dump from a device you own; the project does not distribute copyrighted firmware.
- Physical buttons, audio, IR and NFC frontends are not yet fully integrated into this ESP32 build.
- The Plus Color target and P's/4U targets use the existing TamaEmu device profiles. This is not the full Paramix Studio UI, Paradise menu or its packing format.

## Memory layout

- ESP32 application and project data: internal 16 MiB flash, partitioned by `partitions.csv`.
- P's/4U firmware source: TF card, raw 8 MiB image.
- Guest ROM read cache: up to 2 MiB PSRAM (32 × 64 KiB pages).
- Emulator state and working memory: PSRAM.
- Persistent save backend: not implemented yet.

## Upstream projects

- Paramix Studio: https://github.com/mameise/Paramix-Studio
- TamaEmu core: https://github.com/maragotchi/tamaemu
- Waveshare board pinout: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3
