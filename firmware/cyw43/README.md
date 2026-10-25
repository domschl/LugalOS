# CYW43439 firmware blobs

**These are the only non-source artifacts in a LugalOS image.** Everything
else in this repository is source that is compiled here. These three files
are not: they are pre-built binaries that run on the wireless chip's own
processor, and nobody outside Infineon can read what they do.

That is a deliberate, documented exception, not an oversight. The argument
for taking it is in `plan/phase19_ip_stack_and_ethernet.md` §1: the CYW43439
is a *fullmac* part, so the blob does 802.11 association and hands us
Ethernet frames. The IP stack above it (`net/`) is ours; the radio below it
is not, and no amount of work on our side changes that.

## What is here

| File | Size | What it is |
|------|------|------------|
| `43439A0.bin` | 231077 B | Main firmware. Uploaded into the chip's RAM at every boot; the chip has no flash of its own. |
| `43439A0_clm.bin` | 984 B | CLM regulatory data -- the per-region transmit limits. Loaded after the firmware. |
| `nvram_rp2040.bin` | 742 B | Board-specific calibration and module parameters. Named for the RP2040 Pico W, and correct for the Pico 2 W as well: both carry the same wireless module, and embassy's own RP235x example loads this same file. One value differs from embassy's copy: `pa2ga0` (see below). |

Total: **232803 bytes**, about 227 KB, all of it in flash and copied to the
chip at init.

## Where it came from

Fetched from [embassy-rs/embassy](https://github.com/embassy-rs/embassy)'s
`cyw43-firmware/` directory, which in turn takes them from
[georgerobotics/cyw43-driver](https://github.com/georgerobotics/cyw43-driver/tree/main/firmware)
-- the driver the Raspberry Pi Pico SDK itself vendors. Same binaries the
Pico SDK ships, just in loose `.bin` form rather than baked into a C header,
which is what let them be embedded here without a vendored driver.

SHA-256, so a future update is visibly an update:

```
5555e0261da2610a500d68c18d895cace0152bbefbf76f4aa683ebce77e3d7eb  43439A0.bin
e712b3d218e8b1e2747b092e03b8b0afcb8c8c8e355d2a4a0d47b493800f3f89  43439A0_clm.bin
24381ed1c0e8a5569281410ec2885504d54c5499a2d2ae32489980bc2d0a2849  nvram_rp2040.bin
```

**`pa2ga0`, the 2.4 GHz power-amplifier calibration** (2026-10-25): changed
from `-168,6649,-778` to `-168,7161,-820`, the value georgerobotics/cyw43-driver
set in commit cf924bb ("firmware: Update nvram settings for 43439", 2024-11-07)
and the Pico SDK and MicroPython ship. embassy's copy still carries the old
one. The two strings are the same length, so the file's layout is unchanged.
Measured on one Pico 2 W, same place and supply, 200-ping runs: 3.3% lost
with the old value (20 of 600), 0.9% with the new (12 of 1400); MicroPython
on the same board lost none of 600.

## Licence, and whether we may ship them

Infineon **Permissive Binary License 1.0**, reproduced in full in
`LICENSE-permissive-binary-license-1.0.txt`.

Redistribution in binary form **is permitted**, without modification,
provided the copyright notice and disclaimer travel with it -- which is what
this directory does. The licence also forbids reverse engineering,
decompilation or disassembly, so these files are used exactly as shipped and
are never inspected or patched here.

This is why the blobs are committed rather than downloaded during the build:
a build that reaches the network is not reproducible, and a licence that
permits redistribution removes the only reason to avoid committing them.
