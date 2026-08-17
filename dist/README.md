# LugalOS v0.9.0 — RP2350 release images

Pre-built UF2 firmware images for the Raspberry Pi Pico 2 (RP2350), one per board persona.
Flash by holding **BOOTSEL** while plugging in USB, then copying the UF2 onto the `RP2350`
mass-storage volume that appears (see the main [README](../README.md#flash-to-pico-2) for
the "1200-baud touch" no-button-press flow once a LugalOS build is already on the board).

| File | Persona | What it is |
|---|---|---|
| `lugalos-0.9.0-rp2350-chess.uf2` | `rp2350-chess` | Default persona: SD card (SPI1), ST7735 TFT + TM1638 keypad, onboard chess engine. |
| `lugalos-0.9.0-rp2350-clock.uf2` | `rp2350-clock` | Waveshare Pico-Clock-Green persona: 7-segment display + LDR auto-brightness, appliance-style auto-start. |

`SHA256SUMS` covers both files (`shasum -a 256 -c SHA256SUMS` from this directory).

Built from a clean `build/` via the documented presets (`cmake --preset <name> && cmake --build --preset <name>`);
not hand-edited. Regenerate by rebuilding both `rp2350-chess` and `rp2350-clock` and copying their
`lugalos.uf2` here under these names.
