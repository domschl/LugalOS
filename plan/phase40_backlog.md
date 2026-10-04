# Phase 40 — The actionable backlog, and a review for C's failure modes

**Status: CONCLUDED, 2026-10-04**, shipped as 0.16.0. Two objectives:

1. **Bring the actionable bugs to zero** — the list phase 38's 38.0b sorted
   out of `plan/open_issues.md` part A, below in its original order.
2. **Review the system for C's usual instabilities** — stack overflow,
   unchecked pointer and buffer access, arithmetic overflow, behaviour that
   depends on luck — with tools, not only by reading.

Every fix is its own commit; `git log b0f6c01..` is the record. Each item's
original description stays in git history of `plan/open_issues.md`.

## 1. The backlog

| # | Item | Status | Commit |
|---|---|---|---|
| 1 | The runner cannot see inside a stuck guest | **Done.** Monitor socket, per-hart registers symbolised, bounded writes, `[Attempt]` lines | `902d6dc` |
| 2 | Two hand-rolled yielding locks outside the wait-for graph | **Half verified** -- on a Pico 2 W, `wifi probe` under live traffic, rejoins, 120 pings: no `[Lock BUG]` (2026-10-04); the ENC28J60 gateway half still needs that board | `83f0507` |
| 3 | Pulling the SD card out of a running board hangs it | **Done**, on the LCD-7 -- the allocator spun once per cluster on failed reads; a pulled card is now *lost* until reboot, and the SD icon follows it | `1531895`, `6b8339c` |
| 4 | A trailing slash breaks path resolution below a mount root | **Done** | `fd77381` |
| 5 | `cc` searches only /ram0 for a relative `#include` | **Done**, and the shared header buffer it exposed | `1b6a1d6` |
| 6 | `K3` checks pin values from a table, not from the build | **Done**, verified on the LCD-7 (match, mismatch, unknown build, same-id decoy); its `/proc/node` read exposed an 8 KB identity stack cost, fixed | `64eb2f4` + persona fix, `7844345` |
| 7 | C6/C7's exact heap comparison disturbed by background allocation | **Done** — settled readings, two compiles; 3/3 on the LCD-7 | `601160e` |
| 8 | The clock display flickers while the radio comes up | **Closed** — already solved, per the owner (2026-10-03) | — |
| 9 | No clean way to leave a BSS before re-joining | **Done**, on a Pico 2 W -- the plain disassociate works on today's driver; a join now leaves first | `e7c1ce0` |
| 10 | `mqttd` has no file-backed source | **Done** (`mqttd file`, `(mqttd-file ...)`) | `bf44055` |
| 11 | Pressure is published as station pressure | **Done**, run on a Pico 2 W sensor node; the altitude now takes effect without a reboot. Station pressure agrees with a reference BME280 within 0.3 hPa; the sea-level value is 3.5 hPa below the reference's, accepted by the owner | `aff00f5`, `f888c0a` |
| 12 | An identity write reboots the board | **Done** -- the console, the 9P link, QSPI and PSRAM survive the write on the LCD-7; the reboot is gone | `337cf3c` |

## 2. The review

### Method

**There is no valgrind for a LugalOS guest**, under QEMU or anywhere:
valgrind instruments a Linux process through its loader, its system calls and
its `malloc`, and a guest is the kernel itself. What each layer can do:

* **QEMU builds** keep UBSan (every one halts the guest on undefined
  behaviour) and gain the runner's stuck-guest report (item 1): a hang or a
  halt now leaves every hart's pc, ra and sp, named against the build.
* **`tests/host/`** (`ea5abd9`, `ce38bf3`) builds the modules that parse
  input someone else controls for the development machine, under ASan+UBSan
  and valgrind, and drives them with valid and corrupted input: FAT32 images,
  C programs, 9P sessions, Ethernet frames. `make -C tests/host check` (8 s);
  the QEMU runner runs a short pass (4 s). Its README has the reasoning.
* `gcc -fanalyzer` over the whole tree (one real finding, `7dc72b3`) and
  `-fstack-usage` where a recursion bound had to be chosen (`6539dce`).

### Found and fixed

Each with how it showed itself. "Kernel" means any user could trigger it
from a file or a program.

| Commit | What | How found |
|---|---|---|
| `3ca5b4e` | FAT32: a cyclic cluster chain hung a directory scan forever; a failed sector read handed the callback stack garbage | fat32_host, image 2 519 |
| `db220c2` | FAT32: any boot-sector geometry mounted — 4 KB sectors, FAT16, zero cluster size | fat32_host (every mutated image mounted) |
| `8d93f62` | FAT32: `rm` on a non-empty directory orphaned its children's clusters | reading the remove path |
| `fd77381` | FAT32: a path of 256+ bytes was truncated and the cut-off name resolved | reading (item 4) |
| `f49daf8` | **Kernel heap overflow:** cc's codegen ignored its buffer size — 535 bytes of C wrote a 4 628-byte ELF from a 4 096-byte buffer | chibicc_host |
| `6539dce` | cc: NULL dereference after a syntax error; unbounded recursion on the kernel stack (234 parentheses halted QEMU); binaries written for programs that did not parse | chibicc_host; the stuck-guest report |
| `f49daf8`, `b3ab532` | cc wrong code: constants cut to 12 bits (`return 5000` → 904), unary minus emitted nothing, 17th function and 17th `break` dropped, arguments past 8, calls to nothing became endless loops | chibicc_host, then QEMU |
| `bb7b454`, `532c894` | cc: sources over 4 KB, lines over 255 characters, output over 16 KB, unknown characters and directives — all silently truncated or skipped | reading, chibicc_host |
| `db203af`, `1b6a1d6` | cc preprocessor: nested `#ifdef` in a skipped block ended the skip; every include level shared one buffer | reading (item 5) |
| `df71b42` | cc: `read_file()`/`write_file()` compiled to an endless loop | chibicc_host's fixtures |
| `08c712f` | printk: `%x`/`%u` of an int sign-extended on RV64; `%ld` of LONG_MIN a UBSan halt; `%05d` of a negative | reading; `fmtselftest` |
| `aba123e` | Syscalls staged data through static buffers shared by all callers (two harts; a task resuming on the other hart; a blocked file read); `putnum(LONG_MIN)` from any program a UBSan halt | reading |
| `4ea15f4` | rv64: every exec and spawn allocated page tables under the scheduler lock — `[Lock BUG]` on every program start, unnoticed because the runner does not treat it as a fault | a probe that happened to print the console |
| `b0db3df` | rv64: `usertest` and two other probes leaked four pages of page tables per run | reading `mem_domain_init()` |
| `bb2349f` | 9P client: an unterminated walk name; paths over 127 bytes or 16 components walked somewhere else | reading |
| `4ec7fe3` | mqttd: a reading below zero was `x << k` on a negative int — a UBSan halt | reading, then reproduced |
| `7dc72b3` | idstore: a failed read left a record with garbage length | `-fanalyzer` |
| `7b5cd85` | The chess persona built with three unused-function warnings | the zero-warning build |

### Reviewed and clean

The 9P server (p9_host: 20 000 mutated sessions under both auth policies) and
the IP stack's receive path (net_host: 90 000 bursts, 36 000 TCP connections
established and fuzzed, valgrind clean). Recorded so that "not looked at" and
"looked at, nothing found" stay distinguishable.

### Not covered, and why

* **Lisp** (`user/lisp/lisp.c`, 6 000 lines): its reader and bignums take user
  input, but it reaches into most of the kernel; a host build needs a larger
  shim than this phase built. The obvious next harness.
* **Concurrency**: the host builds are single-threaded. The SMP findings
  above came from reading.
* **RV32 type sizes**: the host is LP64 like RV64.

### Second pass (2026-10-04): phase 44's window system, and the bench

Phase 44 (terminals, the ribbon, per-terminal canvases) landed between the
two passes and made the kernel concurrent in a way it had not been: a shell
in every terminal. Most of what it turned up is that -- state that had one
user and now has several. Found at the start: the QEMU suite at 465/467
(`vtselftest` failing six of 36 since phase 44, on both architectures).

| Commit | What | How found |
|---|---|---|
| `3bf9fff` | lock: a ylock's wait-for edge outlived its acquisition by one interrupt window -- "[Lock BUG]" every few commands with two shells | recorded where each edge was set |
| `9c45070` | vterm: a closed terminal's task called `task_exit()` from `console_putc()` with the console lock held -- `vterm close 1` in terminal 1 hung every shell; its shadow freed under a writer; a spurious `task_unblock()`; a reused slot's old shell; Ctrl-C latched the root console too; a byte lost on a full queue | reproduced in QEMU, then reading |
| `9c45070`, `58f8ed3` | a terminal's shell ran on 8 KB (Lisp recursion stopped at a third of the root shell's depth); on rv64 the Lisp guard left 536 bytes of 16 KB | QEMU, stack high-water |
| `9c499b0` | cc: two terminals compiling shared, then freed, one arena | reading |
| `c678b7e` | lisp: a collection in one terminal freed another terminal's half-evaluated form -- one engine lock now, the prompt-side hooks only try it | reading (needs a person to reproduce) |
| `e47dd0b` | screen: replies carried a stale canvas size; a split focused the canvas (title bar drawn over it); a focused canvas sent keys to the terminal with its slot number; 'N' read its title past the request; `ribbon` listed no windows; Super+W closed the root terminal's window; `exit` left windows behind; layout stores overwrote a second canvas; any canvas came back blank after scrolling; two repaint pixels; Super+Shift+3 | `vtselftest`, then QEMU sessions |
| `7844345` | idstore: 8 KB of stack per identity read; `cat /proc/node` took the RP2350 boot stack to 15 096 of 16 384 | hardware suite's margin check, tests one at a time |
| `337cf3c` | item 12 (the identity-write reboot) | the bench |
| `1531895`, `6b8339c` | item 3: the FAT allocator spun once per cluster on a pulled card; the SD icon and the mount now follow the card | the bench, then fat32_host |
| `9ff29bb` | the first character after every reboot was lost: a break NUL at boot read as Ctrl-Space | owner report, reproduced |

`vtselftest`'s 37.5a divider and swap checks tested the layout model phase
44 replaced; they now test the ribbon's (canvas right of its terminal,
`'w'` stepping the focused window through 38/48/64/full). That is the one
place a test was changed to match the code rather than the other way round.

**End state.** QEMU suite 471/471 (467 tests, 465 passing, at the start);
`make -C tests/host check` clean, fat32_host now with a pulled card; the
RP2350-LCD-7 hardware suite 32/32 on `7844345`, the SD pull and the identity
write verified on it afterwards.

**Not changed, written down.** Shared by every terminal and not worth a
lock yet: the line editor's history ring, the chess engine's game state,
`console_capture()`'s single slot (now per task, still one at a time), and
`run_hotkey()`'s read-and-clear of `g_hotkey` (two harts can both take
one keypress). The kernel-side vterm keeps a 98x27 shadow per terminal
that nothing displays -- the screen's own terms are what is drawn.

## 3. Observed, not explained

* Once in seven rv64 runs of the architecture suite on 2026-10-03, `taskdemo`
  failed its interleaving check and the C2 page count read 4 high right after
  it. Not reproduced since (four full suites on 2026-10-04).
* PSRAM 38.2 "read cached cold" on the LCD-7: 21.1-23.8 MB/s against a 26.1
  reference, inside the 15 % band most runs and outside it about one in
  three -- on both the phase's first and last firmware. The reference is
  stale for this board; re-baselining it is in `plan/open_issues.md`.

## 4. Conclusion

Phase 40 closed on 2026-10-04 as 0.16.0. Of the twelve backlog items, nine
were done then, one closed by the owner, and three needed boards that were
not on the bench. **Later the same day** a Pico 2 W sensor node arrived:
item 9 is done (`e7c1ce0`), item 11 ran on it, and item 2 is half verified.
What it showed beyond the items:

* The identity-write reboot (gone since 40.12) had been applying the
  altitude for us: `pressure_msl` was registered at boot only. Fixed
  (`f888c0a`); the other identity settings were checked and are live or
  documented as boot-time.
* **The sea-level formula's temperature is an open decision.** Station
  pressure agrees with the owner's reference BME280 to 0.3 hPa (966.6 vs
  966.9), but the reduced values differ by 3.5 hPa (1025.0 vs 1028.5): the
  reference uses the standard atmosphere, p0 = p / (1 - h/44330)^5.255,
  and `bme280_sea_level_pa()` the barometric formula with the sensor's own
  temperature -- 28 C for this indoor node, where the formula wants the
  outdoor air column. **Decided by the owner (2026-10-04): kept as is** --
  the difference is within what the sensors can resolve, and the results
  are fine.

What remained at the close, in `plan/open_issues.md`:

* **Item 2** -- the CYW43 and ENC28J60 bus locks are ylocks; the Pico 2 W
  half passed later the same day; the ENC28J60 gateway
  (`tests/hw/test_gateway.py`) remains, watching for `[Lock BUG]`.
* ~~Item 9~~ -- done later the same day.
* ~~Item 11 on hardware~~ -- run later the same day; the formula kept.

The review's obvious next harness is still Lisp (reader, bignums) on the
host, and the second pass adds one: the screen protocol (`screen_canvas_vterm()`)
driven by mutated requests, as `vtselftest` already does by hand.

**Closed by the owner, 2026-10-04.** The bench then ran 0.16.0 on every
board it had: the RP2350-LCD-7 (`rp2350-terminal`), the Pico 2 W sensor
node (`rp2350-sensor`), the chess computer (`rp2350`: SD, ST7735 and TM1638
tasks up) and the Pico-Clock-Green (`rp2350-clock`: WiFi joined, RTC time,
NTP serving, no `[Lock BUG]` -- the CYW43 bus lock's second board). Each
now reports its silicon id as its USB serial instead of `0001`. Open from
this phase: item 2's ENC28J60 half (`plan/open_issues.md`).
