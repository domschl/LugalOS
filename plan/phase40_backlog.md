# Phase 40 — The actionable backlog, and a review for C's failure modes

**Status: in progress (2026-10-03).** Two objectives:

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
| 2 | Two hand-rolled yielding locks outside the wait-for graph | **Code done, bench pending** — both `ylock_t`, three personas build; needs a Pico 2 W and the ENC28J60 gateway | `83f0507` |
| 3 | Pulling the SD card out of a running board hangs it | **Open — needs the bench.** One candidate fixed on the way (`3ca5b4e`, a cyclic chain looped forever); the real pull is still to be done on the LCD-7 | — |
| 4 | A trailing slash breaks path resolution below a mount root | **Done** | `fd77381` |
| 5 | `cc` searches only /ram0 for a relative `#include` | **Done**, and the shared header buffer it exposed | `1b6a1d6` |
| 6 | `K3` checks pin values from a table, not from the build | **Done**, verified on the LCD-7 (match, mismatch, unknown build) | `64eb2f4` |
| 7 | C6/C7's exact heap comparison disturbed by background allocation | **Done** — settled readings, two compiles; 3/3 on the LCD-7 | `601160e` |
| 8 | The clock display flickers while the radio comes up | **Closed** — already solved, per the owner (2026-10-03) | — |
| 9 | No clean way to leave a BSS before re-joining | **Open — needs a Pico 2 W** | — |
| 10 | `mqttd` has no file-backed source | **Done** (`mqttd file`, `(mqttd-file ...)`) | `bf44055` |
| 11 | Pressure is published as station pressure | **Open — needs the owner's decision** (publish both?) | — |
| 12 | An identity write reboots the board | **Open — needs a write on a board with a stored record** | — |

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

## 3. Observed, not explained

* Once in seven rv64 runs of the architecture suite on 2026-10-03, `taskdemo`
  failed its interleaving check and the C2 page count read 4 high right after
  it. Not reproduced in six further runs; kernel tasks only, not the syscall
  path changed that day. Belongs in `open_issues.md` part B if it recurs.

## 4. What is left, and what it needs

* **Item 2** — run `wifi probe`/`wifi join` on a Pico 2 W and the gateway
  suite on the ENC28J60; any `[Lock BUG]` names a caller under a spinlock.
* **Item 3** — pull the card from the LCD-7 with the console attached, on the
  current build.
* **Item 9** — a Pico 2 W.
* **Item 11** — the owner's decision on point 3 (publish `pressure` and
  `pressure_msl`, or replace).
* **Item 12** — one identity write on a board whose record is stored, console
  attached; drop the reboot if the console survives.
* The current build has not yet been run through `tests/hw/` on a board.
