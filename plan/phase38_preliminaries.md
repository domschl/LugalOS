# Phase 38 preliminaries: PSRAM, measured before planned

Owner's request (2026-10-02): before writing plan 38 (PSRAM on the RP2350-LCD-7
first, the ESP32-P4 second), measure. This file is the evidence; the plan
cites it. Everything below was measured on the RP2350-LCD-7 on 2026-10-02
with `drivers/psram_rp2350.c` (branch `phase38-psram-spike`, a measurement
spike, not the phase's driver). Nothing here has been measured on the P4 yet.

Decisions taken in the discussion that led here:

* PSRAM stays optional per board; a persona with a large GUI may require it.
* No process or segment **swapping**: PSRAM is memory-mapped on both chips.
* No **FAT cache in PSRAM**: the slow card was algorithmic, and is fixed
  without PSRAM (commit `38.0`, FSInfo + cursors + one cached FAT sector:
  `df` 10.3 s -> 0.1 s, `sdbench w 1024` 6/11 -> 62/349 KB/s).
* A second P4 board is ordered: ESP32-P4-WIFI6-Touch-LCD-7B, 1024x600
  MIPI-DSI (EK79007), samples in `~/Source/gith/esp/ESP32-P4-WIFI6-Touch-LCD-7B`.
  One RGB565 frame is 1.2 MB -- more than all of L2MEM -- so on that board
  PSRAM is a hard prerequisite for the screen, not an optimisation.

---

## 1. The chip

| | |
|---|---|
| `psram init` | GP0 -> XIP_CS1, ID read in SPI mode, RSTEN/RST, enter QPI (0x35) |
| ID | KGD **0x5D** (pass), EID **0x53** |
| Size | **8 MB** by address aliasing (and EID size field 2 = 8 MB) -- **not the 2 MB** the board file and plan 36 assumed |
| Clock | clk_sys 144 MHz / 2 = 72 MHz SCK (clkdiv 1 would be 144 MHz, beyond the part) |
| M1_TIMING | 0x60242202 (pagebreak 1024, cooldown 1, rxdelay 2, max_select 18, min_deselect 2) |
| Pattern test | 8 MB x 4 passes (uncached/uncached, cached->clean->uncached, uncached->cached, cached/cached): **0 errors** |

## 2. Rates (64 KB per pass, LCD scanout DMA running throughout)

| Sequential | KB/s |
|---|---|
| SRAM read | 201 257 |
| PSRAM read, cached, cold | 26 111 |
| PSRAM read, uncached | 24 492 |
| PSRAM read, cached, 8 KB hot set | 63 936 |
| **flash** read, cached (kernel image) | **19 541** |
| flash read, uncached | 18 212 |
| SRAM write | 316 831 |
| PSRAM write, cached (+ clean_all) | **9 123** |
| PSRAM write, uncached | 31 113 |

| Copy | KB/s |
|---|---|
| PSRAM uncached -> SRAM, word loop | 21 850 |
| SRAM -> PSRAM uncached, word loop | 23 315 |
| PSRAM uncached -> SRAM, libc `memcpy` | **9 583** |
| SRAM -> SRAM, libc `memcpy` | **20 062** |
| DMA PSRAM uncached -> SRAM | 25 009 |
| DMA PSRAM cached -> SRAM | 28 469 |
| DMA SRAM -> PSRAM uncached | 31 527 |
| DMA SRAM -> SRAM | 551 724 |

| Pointer chase (dependent loads, 64-byte nodes, one random cycle) | ns/load |
|---|---|
| SRAM, 4-64 KB | 35 |
| PSRAM cached, working set <= 16 KB (fits the XIP cache) | 35-38 |
| PSRAM cached, 64 KB - 1 MB (misses) | **~505** |
| PSRAM uncached, any size | **~392** |

Readings:

* **PSRAM is as fast as flash for sequential reads** (25 vs 19.5 MB/s): both
  sit behind the same QMI, and the PSRAM runs QPI at 72 MHz.
* **A missed load costs ~14x SRAM** (~70 cycles); a cached miss is *slower*
  than an uncached load (505 vs 392 ns): the 8-byte line fill costs more
  than it saves for random access.
* **Cached writes are poor** (9 MB/s): write-allocate fills the line first,
  and the clean costs extra. Bulk writes belong on the uncached alias or DMA.
* **libc's `memcpy` is a byte loop** and costs a factor 2.3 (PSRAM) to 10
  (SRAM) against a word loop -- `plan/open_issues.md` already lists it; with
  PSRAM it becomes a phase-38 item, since everything that moves data in and
  out of PSRAM goes through it.

## 3. The Lisp heap in PSRAM

Same image apart from where `node_pool` (and the mark stack) live;
`(fib 18)` and 300 x `(mk 200 '())` (allocation and collection heavy).

| node_pool | `(fib 18)` | alloc loop |
|---|---|---|
| 2 048 nodes, SRAM (today) | 308 ms | 3 427 ms |
| 2 048 nodes, PSRAM | 515 ms (1.67x) | 6 056 ms (1.77x) |
| **32 768 nodes (16x, 512 KB), PSRAM** | 490 ms (1.59x) | 5 880 ms (1.72x) |

* The cost is the move to PSRAM itself, ~1.6-1.75x. **A 16x larger heap
  costs nothing more** -- it is marginally faster, collecting less often.
* The mark bits are already a separate SRAM bitmap. **The mark stack is
  not**: `gc_work_stack[NODE_POOL_SIZE]` is SRAM proportional to the heap
  (128 KB at 32 K nodes; the spike moved it to PSRAM to link). A large heap
  needs a bounded mark stack (or pointer reversal) whatever else happens.
* A nursery in SRAM (generational) is the known way back towards SRAM speed;
  whether 1.7x is worth that complexity is the owner's call.

## 4. Flash writes and PSRAM: the hazard is real, and the cure works

`psram xip [clean]` runs the ROM sequence the identity-store write runs
(connect_internal_flash, flash_exit_xip, flash_flush_cache,
flash_enter_cmd_xip), core 1 parked, interrupts off, after writing 4 KB
through the cache:

| | not cleaned | cleaned first |
|---|---|---|
| dirty words that reached PSRAM | **16 of 1024** (1008 lost) | 1024 of 1024 |
| M1 RFMT / RCMD after | 0x612aa -> **0x1000 / 0x03** (serial read: garbage from a QPI chip) | same |
| M1 WFMT / WCMD, XIP_CTRL | unchanged | unchanged |
| PSRAM after restoring M1 | answers | answers |

So `drivers/flash_rp2350.c` must, as pico-sdk does (`flash.c`
`flash_save_hardware_state` / `flash_rp2350_restore_qmi_cs1`): clean the XIP
cache before, and restore M1 TIMING/RFMT/RCMD after. Nothing in PSRAM may be
touched while the sequence runs (interrupt handlers, core 1, DMA).

**Found on the way, independent of PSRAM:** after that sequence M0 (flash)
is left in the ROM's serial 03h mode at CLKDIV 12 until reboot -- every
identity-store write already does this today; `psram test` ran 46 % slower
afterwards (7.6 s vs 5.2 s). Restoring a fast M0 needs the boot2 /
continuous-read re-entry, not a register copy.

## 5. Colour on the LCD-7: what the bus allows

The panel runs PCLK 24 MHz, ~56 Hz, ~21.5 Mpixel/s averaged (24 M during a
line). Scan-out straight from PSRAM would need:

| format | average | vs measured PSRAM DMA (25-28 MB/s, shared with all XIP code fetches) |
|---|---|---|
| RGB565 | ~43 MB/s | impossible |
| 8 bpp | ~21.5 MB/s | the whole bus; flash code would starve |
| 4 bpp | ~10.8 MB/s | ~40 % of the bus -- possible on paper, measurably costly to every XIP fetch |
| 2 bpp | ~5.4 MB/s | |
| 1 bpp (today, from SRAM) | ~2.7 MB/s | |

So on the RP2350 PSRAM is **storage for the GUI, not scan-out**: back
buffers, off-screen tiles, saved screens, fonts, compositing. The scan-out
frame stays in SRAM; PSRAM is what lets SRAM afford a 2 bpp (96 KB) frame,
by taking the large, latency-tolerant things out of it. Scanning 4 bpp from
PSRAM would need its own measurement of what it does to XIP-bound code.

## 6. What this means for plan 38 (proposals, for the owner to decide)

1. **Bring-up, proper**: boot-time probe (ID + aliasing) instead of a shell
   command; the 8 MB stated in the board file; M1 timing derived from
   `CONFIG_CLK_SYS_HZ`; the flash path cleaned and M1 restored (§4).
2. **A second palloc zone** (bulk) over PSRAM: 8 MB / 4 KB = 2 048 pages,
   a 256-byte bitmap. Callers state the latency class; bulk falls back to
   SRAM where there is no PSRAM. `/proc/meminfo` per zone. QEMU gets a fake
   bulk zone so the suite runs the code.
3. **libc `memcpy`/`memset`/`memmove` word-wide** (open issue), measured
   with the same bench.
4. **Consumers, in measured order**: RAM disk (sequential, the best case);
   U-mode program images (code from PSRAM is XIP-like: measure); the Lisp
   heap (1.7x, and 16x the size; bounded mark stack); off-screen GUI
   storage; then freed SRAM for hot code (chess `.ramfunc`, open issue).
5. **P4** (priority 2, with a consumer now: the 7B's framebuffer): MPLL,
   MSPI2/3 PSRAM controller, hex-PSRAM mode registers, timing tuning, MMU
   mapping -- ~1 500 lines of IDF to re-derive (`esp_psram_impl_ap_hex.c`,
   `psram_ctrlr_ll.h`, `mspi_timing_tuning`); then the same bench.

## 7. How to repeat

On branch `phase38-psram-spike`, `rp2350-terminal` build:

    psram init          # once per boot
    psram test          # 8 MB, four passes, ~5 s
    psram bench
    psram dma
    psram xip           # resets flash to 03h mode until reboot
    psram xip clean

The Lisp numbers need a build with the pool moved:
`cmake --preset rp2350-terminal -B build/spike-lisp-N -DCMAKE_C_FLAGS=-DSPIKE_LISP_PSRAM=N`.
