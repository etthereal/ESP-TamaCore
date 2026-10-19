# Paramix-inspired colour Tamagotchi emulation on ESP32-S3

This is an experimental ESP-TamaCore adaptation for Waveshare ESP32-S3-LCD-1.3 (16 MiB Flash, 8 MiB PSRAM). It uses the portable TamaEmu S1C33 emulator core that underlies Paramix Studio, rather than trying to execute Paramix's Paradise-specific `emu.bin` on the ESP32.

## Why this is not a direct Paramix binary port

Paramix's `emu.bin` is compiled for the Tamagotchi Paradise's ARM Thumb-2 CPU and hardware. The ESP32-S3 uses Xtensa LX7, so that binary cannot be executed as-is. The reusable part is the shared S1C33 guest emulator, device profiles and ROM-handling concepts. The Paradise-specific JIT emits Thumb-2 instructions; it cannot be reused on Xtensa without a new code-generation backend.

Paramix's architecture is not identical for every guest. P's/4U use TamaEmu's S1C33 core plus a JIT translator; Meets/On use T18Emu, an ARM7TDMI interpreter/JIT and a separate GPEL3101A peripheral model. The 4U/P's path is the relevant one for this ESP-TamaCore branch. The first milestone is to make the existing portable TamaEmu interpreter stable with SD-backed ROM; an Xtensa JIT is a later, separate optimization project.

## SD-backed builds

PlatformIO environments added in this branch:

- `pio run -e ps_sd`: P's; place the user's own raw 8 MiB firmware dump at `/ps.bin` on a FAT-formatted TF card.
- `pio run -e 4uplus_sd`: 4U+; place the user's own raw 8 MiB firmware dump at `/4uplus.bin`.
- `pio run -e 4u_sd`: original 4U; place the user's own raw 8 MiB firmware dump at `/4u.bin`.
- `pio run -e pluscolor_flash`: Plus Color; 4 MiB ROM remains in the `tamarom` flash partition.

The SD-backed builds read the ROM in 64 KiB pages into a 32-slot PSRAM cache (up to 2 MiB), rather than copying the entire 8 MiB image into PSRAM. The cache now uses fully associative LRU replacement instead of direct mapping: this avoids repeated evictions when hot code/data pages happen to collide in the same cache slot. This keeps the ROM on the TF card and leaves PSRAM available for emulator state and cache. The SD interface is initialized with GPIO MOSI=11, MISO=13, SCK=12, CS=10; confirm these pins against the exact board revision if card initialization fails.

Paramix packs used 64 KiB firmware blocks because the Paradise has limited free flash and its guest firmware is embedded in the host ROM. On the ESP32-S3, the SD card already has room for a raw 8 MiB dump, so reproducing that packing format would add complexity without solving a current storage limit. The useful idea to carry over is its block-aware cache and, especially, its persistent handling of guest NOR writes.

## Important limitations

- Experimental. The four PlatformIO targets compiled successfully in GitHub Actions on the previous SD-cache revision; the newest LRU-cache change still needs its own CI run. No hardware test has been performed.
- SD page-cache misses perform a seek and 64 KiB read during emulation. Performance and timing stability must be measured on hardware; the cache is a first functional design, not a claim of real-time performance.
- Guest flash writes currently use a volatile copy-on-write overlay in PSRAM. Saves and firmware changes are lost on power-off. A persistent save/overlay file on SD, inspired by Paramix's spare-block table, is a required next step.
- The ROM dump is not included. Use a firmware dump from a device you own; the project does not distribute copyrighted firmware.
- Physical buttons, audio, IR and NFC frontends are not yet fully integrated into this ESP32 build.
- The Plus Color target and P's/4U targets use the existing TamaEmu device profiles. This is not the full Paramix Studio UI, Paradise menu or its packing format.

## Memory layout

- ESP32 application and project data: internal 16 MiB flash, partitioned by `partitions.csv`.
- P's/4U firmware source: TF card, raw 8 MiB image.
- Guest ROM read cache: up to 2 MiB PSRAM (32 × 64 KiB pages, LRU replacement).
- Emulator state and working memory: PSRAM.
- Persistent save backend: not implemented yet.

## Upstream projects

- Paramix Studio: https://github.com/mameise/Paramix-Studio
- TamaEmu core: https://github.com/maragotchi/tamaemu
- Waveshare board pinout: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.3
