# Updating Nodes over 9P

[Documentation](index.md) > [Administrator Manual](admin.md) > **Updating Nodes over 9P**

---

A node that supports updates takes a new image through two files in its own
namespace, keeps the old image until the new one has proven itself, and goes
back to the old one by itself if it does not. Because it is only 9P, any path
to the node works: a host on the LAN, a P4 reaching its C6 through `/c6`, or
the console's SLIP link.

Supported so far: **the ESP32-C6** (`esp32c6`, `esp32c6-mini`) and **the
ESP32-P4** (`esp32p4`, `esp32p4-lcd7b`, `esp32p4-tab5`), and **the RP2350** (so far
the Pico 2 W, `rp2350-wifi`; the other presets build the same backend, the
RP2350-LCD-7's larger flash is not used yet).

## The files

| File | Read | Write |
| :--- | :--- | :--- |
| `/dev/ota` | `board`, `chip`, `layout` (`loader`, `idf`, `partition`, or `none`), `version` (boards whose bootrom orders images by one), `running` (slot `a`/`b`), `trial`, `fell_back`, `slot_a`/`slot_b` (build id or `empty`), `at_a`/`at_b`/`at_flashfs`/`at_records`/`at_identity` and `at_loader` or `at_table` (`0xSTART+0xSIZE` in flash), `active`, `pending`, `tried`, `failed` (the boot record), `release_key` (key id or `none`), `signatures` (`required`, `optional`), `image_signature` (once an image's header is in), `state` (`idle`, `receiving`, `received`, `committed`, `error`), `received`, `error` | `begin SIZE`, `commit`, `reboot`, `confirm`, `abort`, `switch` |
| `/dev/otaimage` | -- | the update image, in order from offset 0, after `begin` |

1. `begin SIZE`: the image's length in bytes.
2. The image into `/dev/otaimage`. Its 256-byte header names the board and the
   chip, and carries the signature (below): an image for another board, or
   one the node's signature policy refuses, is refused when its header
   arrives, before anything is erased. Then the slot this boot is *not* running from is
   emptied and the image goes into it, one 4 KB sector at a time.
3. `commit`: the node reads the slot back and checks its SHA-256 against the
   header; only then does it write the slot's header (a slot cut off half-way
   has none, and is never booted) and ask the next boot to try the slot once.
4. `reboot`: the new image boots **on trial**. It confirms itself once the
   board calls itself healthy (on a C6 beside a P4: the link to the P4 is up;
   on its own: joined to its network) after 10 s. A trial that hangs, crashes
   or does not confirm within 60 s is reset by a watchdog the loader armed
   (a whole-chip reset: `last_reset=watchdog (RTC, ...)`), and the loader
   boots the old slot again (`fell_back=yes`).

`confirm` confirms by hand; `abort` drops an update in progress. `switch`
makes the image already in the other slot the one the next boot starts (no
trial: it is booted as it is), then `reboot`.

Reading `/dev/otaimage` while no update is in progress returns the other
slot's raw bytes (`lota.py dump-other`, for backing up a factory image).

## `tools/lota.py`

```bash
# The image from a build directory (also written by c6flash.py):
uv run tools/lota.py image build/esp32c6-mini

# The C6 beside an LCD-7B, through the P4's console and its /c6:
uv run tools/lota.py push build/esp32c6-mini --p4 lcd7b --prefix /c6 --reboot --wait

# The same over Wi-Fi, through the P4's own 9P server (authenticated; for a
# session, a key set at the P4's console with `p9key HEX` will do):
uv run tools/lota.py push build/esp32c6-mini --tcp 192.168.178.160 --keyfile KEYFILE \
        --prefix /c6 --reboot --wait

# A C6 on its own (the C6-Zero): its own 9P server. Its reboot ends the
# connection; --wait connects again once it is back, which needs a key that
# survives the reboot (a grant: `peers add lugal HEX / rw` on its console --
# a `p9key` lasts one boot):
uv run tools/lota.py push build/esp32c6 --tcp 192.168.178.162 --keyfile KEYFILE --reboot --wait

uv run tools/lota.py status --p4 lcd7b --prefix /c6

# Boot the other slot's image next (e.g. back to the factory esp-hosted-mcu):
uv run tools/lota.py switch --p4 tab5 --prefix /c6 --reboot

# Save the other slot's image (an ESP-IDF image is cut to its length and its
# SHA-256 checked):
uv run tools/lota.py dump-other --p4 tab5 --prefix /c6 -o ota0-factory.bin
```

`--wait` follows `/dev/ota` until the new image has confirmed (exit 0) or the
node has fallen back (exit 2). `--break` sends an image with 64 KB of the
kernel's code zeroed -- it passes every check, boots and dies, which is how the
fall-back is tested. `--no-board-check` sends an image for another board, to
test the node's own refusal. `--rate KB/S` slows the upload down, for cutting
the power in the middle of one by hand.

## Signed images

An update image can carry an ECDSA P-256 signature in its header, over the
header's first 132 bytes. Those bytes include the SHA-256 of the image's
parts, which `commit` checks against what landed in flash, so the signature
covers the whole image. The node checks it when the header arrives, before
anything is erased.

| The node | Unsigned image | Signed with its key | Signed with another key, or damaged |
| :--- | :--- | :--- | :--- |
| no release key (`otakey`: `no release key`) | taken | taken (not checked) | refused if damaged |
| release key installed | refused | taken | refused |
| build with `CONFIG_OTA_REQUIRE_SIGNED=1` | refused | taken | refused |

```bash
uv run tools/lota.py keygen      # once: ~/.config/lugalos/keys/ota-release.pem (keep it safe)
uv run tools/lota.py pubkey      # prints `otakey set <hex>`: type it on each node's console
```

From then on `image` and `push` sign with that key (`--sign-key` names
another, `--no-sign` sends the image unsigned). Installing the key is a
console-only, factory-style step. The network can never change it, and an
`ea` tool call cannot either. To install an unsigned image on a node that
requires signatures (an esp-hosted-mcu image, a test build), type
`otakey allow-unsigned` at its console first: it lets exactly one image begun
within 10 minutes through.

`push --tamper sig|header|part` damages a signed image on purpose: the node
must refuse the first two when the header arrives, and the third at `commit`.

The key id `/dev/ota` reports is the first 8 bytes of the SHA-256 of the
64-byte public key, the same id `lota.py` prints.

## The ESP32-C6's flash

| Offset (C6-MINI) | What |
| :--- | :--- |
| `0x000000` | the loader (`arch/riscv/esp32c6/loader.c`), 2 KB: the image the boot ROM starts. Written only through the header pins (`c6flash.py`). |
| `0x010000` | the boot records: two 4 KB sectors |
| `0x020000` | slot A, 1.5 MB: the image header, the kernel's RAM half at +4 KB, its XIP half at +128 KB |
| `0x1A0000` | slot B, the same |
| `0x3F0000` | the identity sector |

The C6-Zero's map is the same with 2 MB slots (`cmake/flash_layout_esp32c6.cmake`).
The loader prints one line on the C6's UART0 at every boot, e.g.
`[loader] slot B, 881.dbc5629e+ (trial)`.

A kernel loaded into RAM by `c6run.py` was not started by the loader: it
cannot update (`running=-`), and says so if asked.

## The RP2350's flash

The shell's `ota` command (and `(ota-info)` in Lisp) lays out the slots, their versions and the running one.

| Storage offset | What |
| :--- | :--- |
| `0x000000` | the partition table (`tools/rp2350_ptable.py`, via picotool) |
| `0x001000` | slot A, up to `0x17C000` bytes; its update header in the last sector at `0x17D000` |
| `0x180000` | `/flash0`, 1 MB (read at `0x10400000`) |
| `0x280000` | slot B, the same |
| `0x3FD000` | the boot records: two 4 KB sectors |
| `0x3FF000` | the identity sector (read at `0x10800000`) |

The map is the Pico 2's (4 MB). The **RP2350-LCD-7** has 16 MB and `cmake/flash_layout_rp2350_terminal.cmake`: the table, `/flash0`, the boot records and the identity sector stay where they are, and the slots become 4 MB each at `0x400000` and `0x800000` (a slot is exactly what one QMI translation window maps, which leaves the other windows for `/flash0` and the identity). A board flashed with one map needs the first-time install again to take the other.

The slots are a bootrom A/B pair: it boots the higher-versioned valid image, and
maps only that partition at `0x10000000` (datasheet 5.1.19), so the kernel points
spare QMI translation windows at `/flash0` and the identity sector before it reads
them (`rp2350_map_data_windows()`). `_PHYS` names in `cmake/flash_layout.cmake` are
storage offsets, `_BASE` the windows.

How an update goes:

1. `tools/lota.py push build/rp2350-wifi --serial IF02_PORT --reboot`. The image is
   the build's `lugalos.bin`, flagged *try before you buy* and with its VERSION raised
   above the running image's (`version=` in `/dev/ota`): the bootrom's `explicit_buy`
   erases the old image unless the new one is strictly newer, so an update that is not
   would destroy the fallback.
2. `reboot` is a *flash update* boot of the new slot, the only boot that will run a
   TBYB image. The kernel calls `explicit_buy` as soon as it is up (the bootrom gives
   it 16.7 s), then judges the trial itself: confirmed when `/flash0` is mounted and,
   with a Wi-Fi network configured, `wlan0` has joined.
3. An image that dies before it buys is dropped by the bootrom's next reset. One that
   buys and then fails -- it gives up after 50 s, a restart is asked for during the
   trial, or the deadman (re-armed each second, 15 s) fires -- finds its record marked
   `tried` at the next boot, erases its own first sector and restarts, so the old image
   is the only one the bootrom can find. `fell_back=yes`, `failed=yes`.

`switch` lasts until the next reset on this board: the bootrom boots the higher
version again. A BOOTSEL-loaded `lugalos.uf2` copy lands in the slot the bootrom would
not boot.

## The ESP32-P4's flash

| Offset | What |
| :--- | :--- |
| `0x002000` | the loader (`arch/riscv/esp32p4/loader.c`), 2.7 KB: the image the boot ROM starts. Written only by `p4flash.py --only boot`, never over the network. |
| `0x150000` | `/flash0`, 1 MB |
| `0x250000` | slot A, 2 MB: the image header, the kernel's RAM half at +4 KB, its XIP half at +128 KB |
| `0x450000` | slot B, the same |
| `0x650000` | the boot records: two 4 KB sectors |
| `0xFFF000` | the identity sector |

The map is `cmake/flash_layout_esp32p4.cmake`. `0x010000-0x150000`, where the
OS image used to be, is unused. `p4flash.py --only boot,os` writes the loader
and slot A and erases the boot records, so slot A boots; `/flash0` and the
identity are left alone. Push over any 9P path to the P4:
`uv run tools/lota.py push build/esp32p4 --p4 nano --reboot` (the console), or
`--tcp HOST --keyfile KEYFILE`. Opening the console resets the board, so
`--wait` over `--p4` cannot watch a trial: check with `status` afterwards.

The loader prints `[loader] slot B, BUILDID (trial)` on UART0 at 115200. A trial
is healthy when `/flash0` or `/sd0` is mounted and, if a Wi-Fi network is
configured, the C6's `wlan0` has joined. The trial watchdog is the RTC
watchdog, set for 60 s (the P4's slow clock runs near 73 kHz, measured).
`begin` also refuses an image built for another silicon revision. A kernel
loaded by `p4run.py` (esptool `load-ram`) was not started by the loader and
cannot update.

### ESP-IDF partition layout (`layout=idf`)

On boards where the C6's pins cannot be reached (the M5Stack Tab5), LugalOS
runs behind the C6's factory ESP-IDF second-stage bootloader, in the factory
partition table's OTA slots. esp-hosted-mcu's table for the C6:

| Offset | Partition |
| :--- | :--- |
| `0x000000` | the factory ESP-IDF bootloader (never written) |
| `0x008000` | the partition table |
| `0x009000` | `nvs`, 16 KB |
| `0x00D000` | `otadata`, two 4 KB sectors of `esp_ota_select_entry_t` |
| `0x00F000` | `phy_init` |
| `0x010000` | `ota_0` (slot `a`), 1920 KB: esp-hosted-mcu from the factory |
| `0x1F0000` | `ota_1` (slot `b`), 1920 KB |
| `0x3F0000` | the LugalOS identity sector |

* **The image** is a single ESP-IDF `0xE9` application image
  (`build/esp32c6-mini/lugalos-c6-app.bin`, built with the kernel), with its
  `esp_app_desc_t` at offset 0x20. `lota.py push` sees `layout=idf` and sends
  it wrapped in the usual update header.
* **The table is read at boot** and must match what the kernel's early boot
  code assumes (`otadata` at 0xD000, `ota_0` at 0x10000). If it cannot be read
  or differs, updates, `switch` and `confirm` are refused for that boot
  (`running=-`) rather than guessed at.
* **The trial** does not depend on the bootloader's rollback option. `commit`
  writes a new `otadata` entry for the slot, marked as LugalOS's trial. On the
  trial boot the kernel marks that entry ABORTED before anything else runs, and
  `confirm` rewrites it as VALID. A trial that never confirms (hang, crash,
  power loss, watchdog after 60 s) leaves the entry ABORTED, which every IDF
  bootloader skips: the next boot starts the previous slot (`fell_back=yes`).
* **An image the bootloader rejects** (bad hash, wrong chip revision, a RAM
  segment over the bootloader's own memory) is never started: the bootloader
  starts the other slot. The kernel notices that the newest entry names a slot
  it is not running, records the running slot as the newest, and reports
  `fell_back=yes`.
* **An image that is not LugalOS** (esp-hosted-mcu's `network_adapter.bin`)
  can be pushed too; it is marked VALID directly, with no trial.
* **After the first LugalOS self-update** both slots hold LugalOS and the
  factory esp-hosted-mcu is gone; back it up first (`lota.py dump-other`).
  The Tab5's is in `~/Source/gith/esp/tab5-factory-flash/tab5-c6-ota0-factory.bin`,
  the NANO's in `~/Source/gith/esp/p4nano-c6-factory-flash/nano-c6-ota0-hosted.bin`.

### First install from esp-hosted-mcu (`sdio ota`)

A C6 still running the factory esp-hosted-mcu is given LugalOS from the P4, over
SDIO, with esp-hosted-mcu's own OTA RPCs (OTABegin, OTAWrite in 1 KB chunks,
OTAEnd, and OTAActivate on esp-hosted-mcu 2.6 and later):

```
sdio ota [path]       # default /sd0/lugalos-c6-app.bin, then /sd0/firmware/lugalos-c6-mini.bin
```

esp-hosted-mcu writes the image into its passive slot, checks it and activates
it; the P4 resets the C6, which comes up with LugalOS and LLP. A C6 that is
already on LLP is refused (use `/c6/dev/ota`). This first boot is **not** a
LugalOS trial: the kernel marks it VALID when it starts. Its safety net is the
bootloader's own image check, and esp-hosted-mcu staying in the other slot until
the first LugalOS self-update.

---

**Navigation:** [<- Storage Guide](storage.md) | [Administrator Manual](admin.md) | [Board Reference Guides ->](boards.md) | [Documentation Index](index.md)
