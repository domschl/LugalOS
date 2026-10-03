# Host harnesses: kernel code under ASan, UBSan and valgrind

Phase 40's review of the usual C failure modes — out-of-bounds access,
use of uninitialised memory, unbounded recursion and loops, arithmetic
overflow. The modules that parse input someone else controls are built for
the development machine and driven with that input, valid and corrupted,
under the host's sanitizers.

## Why not valgrind on QEMU

Valgrind runs Linux user-space programs: it replaces the process's loader
and instruments its machine code, and it needs the system calls and the
`malloc` of a hosted C library to know what memory is valid. A LugalOS
guest has none of that — it *is* the kernel, with its own allocator, its own
page tables and PMP regions, running on bare (emulated) hardware. There is
no valgrind for that, under QEMU or anywhere.

What the QEMU targets do have, and keep:

* **UBSan** (`ENABLE_SANITIZERS`, on for every QEMU build): signed overflow,
  bad shifts, misaligned or null access, out-of-bounds indexing of arrays of
  known size — each halts the guest, and `tests/runner.py` reports it.
* **The runner's stuck-guest report** (40.1): a guest that stops answering is
  captured from the QEMU monitor — every hart's pc, ra and sp, named against
  the build — so a hang or a UBSan halt leaves evidence.
* Hardware-enforced isolation of U-mode programs and driver tasks (PMP/Sv39),
  and the scheduler's stack checks.

What only a hosted build can add is **AddressSanitizer** (every heap and
stack access checked against what was allocated) and **valgrind**
(uninitialised reads), and the speed to run tens of thousands of inputs.
The harnesses here buy that for the code where it matters most.

## Running

    make -C tests/host asan        # ASan + UBSan, 2000 inputs per harness
    make -C tests/host valgrind    # valgrind, 200 inputs per harness
    make -C tests/host check       # both
    make -C tests/host asan ITER=50000 SEED=7

Each harness prints its seed; a failure is replayed exactly by passing the
same `ITER` and `SEED`. Binaries go to `build/host/{asan,plain}/`.
`HOST_VERBOSE=1` shows the module's own messages, which are discarded by
default. `tests/runner.py` runs a short `asan` pass when gcc and make are
present.

| Harness | Module | Input |
|---|---|---|
| `fat32_host` | `fs/fat32.c` | A functional pass against an in-memory model (sizes 0..70 KB, writes past EOF, appends, truncation, removal, a directory grown past one cluster, a remount, a free count that must come back exactly), then the populated image corrupted — bytes in the boot sector, FATs and directories, and the geometry fields — and mounted, walked, read, written, deleted. |
| `chibicc_host` | `user/chibicc/*` | The SD image's sample programs (each must compile), then mutated copies — bytes changed, tokens inserted, spans deleted or repeated, the text cut short — and limit probes: deep nesting, a small source with a lot of code. |
| `p9_host` | `fs/9p.c` | A scripted client session (version, auth, attach, walk, open, read, write, create, stat, remove, flush) against the real server over a fake namespace, under both auth policies; then the session's frames mutated, and frames of random bytes. |

Every harness also arms an alarm per input: a call that does not return is
a failure (a cyclic cluster chain, say), not a slow test.

## What it found

The first runs found, and phase 40 fixed: a FAT32 directory scan that
looped forever on a cyclic chain; a boot sector accepted whatever its
geometry; a C parser that dereferenced NULL after a syntax error, recursed
without bound on the kernel stack, and wrote binaries for programs it had
not understood; and a code generator that wrote past its 4 KB output buffer
into the kernel heap. See the commits that mention `tests/host`.

## Adding a harness

1. `foo_host.c` with a `main(int argc, char **argv)` taking `[iterations
   [seed]]`, using `host_rand()` from `shim.h` so runs replay.
2. Link what the module needs: `nm -u` on its object lists the kernel
   symbols it calls. Shared shims (`printk`, `cprintf`, `ksnprintf`, ylock,
   palloc, a flat in-memory `vfs_read`/`vfs_write`) are in `shim.c`; stub
   anything else in the harness itself.
3. Add it to `HARNESSES` and a `foo_host_SRC` line in the `Makefile`.

## Limits

* The host is LP64, like the RV64 targets; `long` and pointers are 32 bits
  on RV32 and the RP2350, which these builds do not cover.
* Single-threaded, with locks that only count: no concurrency bugs are
  found here.
* The shims are not the kernel: `ksnprintf` is the C library's, the VFS is a
  table. A bug in the shimmed code itself is out of scope.
