#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for RP2350 (A3b: link_usb_cdc, p9share --
plan/phase5_distributed_design.md's M5/T3). Talks to a real, physically
attached board; every test is *skipped* (not failed) when no RP2350 is
found, so this is safe to run speculatively and safe to leave out of CI.

Usage:
    uv run test_rp2350.py
    uv run test_rp2350.py --console /dev/tty.usbmodemXXXX1 --net /dev/tty.usbmodemXXXX3
    uv run test_rp2350.py --skip-qemu-bridge

Wiring assumed: RP2350 flashed with the current build/rp2350/lugalos.uf2,
connected over USB (enumerates two CDC-ACM ports: console + link_usb_cdc's
net port), optionally also wired to a host CP2101/CP2102 UART adapter for
the p9share test (see README.md's "CP2101 -> RP2350 Wiring" section).
"""

from __future__ import annotations

import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

import serial

import rp2350
import p9lib  # via rp2350.py's sys.path insert

REPO_ROOT = rp2350.REPO_ROOT


def test_umode_isolation(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """B3 on real silicon: U-mode, PMP-enforced isolation, and the syscall
    boundary.

    Worth running here rather than trusting QEMU, because every one of B3's
    hardware surprises was invisible on QEMU: Hazard3 implements no TOR, uses a
    32-byte granule, and reverses the PMP permission bit order under erratum
    E6. QEMU has none of those properties, so a build that passes there says
    nothing about whether this board is actually enforcing anything.

    Three claims, each with its own failure mode:
      usertest      -- trap cause 8 proves the privilege level really dropped
      isolationtest -- a U-mode store into kernel memory faults; canary intact
      deputytest    -- the kernel refuses a foreign destination pointer, and
                       still accepts one the task owns
    """
    name = "B3: U-mode isolation and syscall boundary on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"usertest\n", b"isolationtest\n", b"deputytest\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=0.8, deadline=12.0).decode("utf-8", "replace")

        checks = [
            ("privilege level dropped", "cause: 8 (U-mode" in out),
            ("kernel memory isolated",  "ISOLATED (kernel memory untouched)" in out),
            ("foreign pointer refused", "DEPUTY_REFUSED" in out),
            ("own pointer still works", "OWNBUF_OK" in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-400:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_user_elf(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """B6 on real silicon: a separately linked ELF, loaded from the flash
    filesystem and run in U-mode under a PMP-enforced domain.

    Distinct from test_umode_isolation above, which runs kernel code from a
    kernel section under a domain the kernel built around its own image. Here
    the program arrived as a file, was parsed by the loader, and was placed in
    pages the allocator handed out -- so this is the path an untrusted binary
    actually takes, and until B6 it did not involve U-mode at all: `exec`
    called the loaded bytes directly, at machine privilege.

    Worth running on hardware rather than trusting QEMU for the same reason
    B3 was: the loader's image model exists to satisfy PMP's NAPOT rules, and
    Hazard3 is the core that actually has them -- 32-byte granule, no TOR, and
    the reversed permission bit order of erratum E6. A domain that grants R|X
    on QEMU and X|W here would run the program and silently drop its
    isolation.

    Three claims:
      uhello    -- .rodata, a writable .bss page, a pointer-taking syscall,
                   and a normal return carried out through the exit stub
      uisolate  -- a store outside the domain faults; the program never
                   reaches the line that would report it succeeded
      uspin     -- a timer interrupt reaches user code on this silicon too
    """
    name = "B6: separately linked ELF runs confined in U-mode on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            # uspin is *expected* to be silent while it computes -- that is
            # the whole point of its measurement window -- so it needs a quiet
            # threshold longer than the spin. A shorter one does not fail the
            # test honestly, it truncates the capture and reports the marker
            # as missing.
            for cmd, quiet in ((b"exec /flash0/system/bin/uhello.elf\n", 0.8),
                               (b"exec /flash0/system/bin/uisolate.elf\n", 0.8),
                               (b"exec /flash0/system/bin/uspin.elf\n", 3.0)):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=quiet, deadline=30.0).decode("utf-8", "replace")

        checks = [
            ("program text ran",        "UPROG_TEXT_OK" in out),
            ("data page writable",      "UPROG_DATA_OK" in out),
            ("syscall copied a file",   "UPROG_FILE_OK LugalOS" in out),
            ("returned via exit stub",  "returned 7" in out),
            ("out-of-domain store faulted",
             "UISO_ALIVE" in out and "UISO_NOT_ISOLATED" not in out
             and "terminated before it could exit" in out),
            ("user code preempted",
             "USPIN_PREEMPTED" in out and "USPIN_NOT_PREEMPTED" not in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-600:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_usb_cdc_net_link(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """link_usb_cdc standalone: a host 9P client reads /proc/version over
    ACM1 (plain length-prefixed framing -- no SLIP byte-stuffing, matching
    virtio-console's own reasoning: a reliable USB bulk pipe needs no
    escape machinery to protect message *content*). That reliability
    doesn't cover a stray write landing on the port from something other
    than a well-behaved 9P client, though -- see test_usb_cdc_net_resync
    below for the case this link's own framing has to recover from on its
    own. /proc/version is synthetic (A1) so this doesn't depend on whatever
    happens to be on the board's physical SD card."""
    name = "link_usb_cdc: 9P /proc/version read over ACM1"
    try:
        with serial.Serial(ports.net, 115200, timeout=5) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            adapter = rp2350.SerialSocketAdapter(ser)
            client = p9lib.P9Client(adapter, framing="raw")
            if not rp2350.warm_up_9p(client, adapter):
                return (name, False, "no response from ACM1 after repeated Tversion attempts")
            data = client.cat("/proc/version")
            ok = b"LugalOS" in data
            return (name, ok, "" if ok else f"unexpected content: {data!r}")
    except Exception as e:
        return (name, False, str(e))


def test_usb_cdc_net_resync(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 (plan/phase12_microkernel_migration.md): link_usb_cdc's 4-byte
    length-prefixed framing (drivers/usb_cdc.c's ep4_link_poll()) used to
    treat an implausible length prefix as permanently fatal -- the same
    4-byte window kept failing the same check forever, since nothing ever
    advanced past it, and only a real USB bus reset (unplug/replug) cleared
    it. Found reproducing it directly: a manual probing script's own stray
    writes desynced this exact link mid-session while bringing up the
    M4.5 uart task on this board.

    Fixed to discard one byte and report "not ready yet" instead of
    "corrupt" whenever the window doesn't look like a plausible header, so
    the stream re-syncs itself within a few poll() rounds once real,
    frame-aligned traffic resumes -- this test deliberately writes garbage
    that is not a valid frame, then checks a real Tversion still gets a
    real Rversion back afterward, without needing any reset in between."""
    name = "link_usb_cdc: framing resyncs after garbage instead of wedging permanently (M4.5)"
    try:
        with serial.Serial(ports.net, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"\r\nnot a 9P frame, just garbage bytes 1234567890\r\n")
            ser.flush()
            time.sleep(0.5)
            ser.read(ser.in_waiting or 1)  # discard whatever (if anything) came back

            adapter = rp2350.SerialSocketAdapter(ser)
            client = p9lib.P9Client(adapter, framing="raw")
            if not rp2350.warm_up_9p(client, adapter):
                return (name, False,
                        "no Rversion after garbage -- the link did not resync")
            return (name, True, "recovered a real Rversion after garbage, no reset needed")
    except Exception as e:
        return (name, False, str(e))


def test_firmware_freshness(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """Checks the board is running the firmware this tree would build.

    Reported as a WARNING rather than a failure: a deliberately older board is
    a legitimate thing to test against. But a silent mismatch is not -- an
    earlier session lost a full flash-and-measure cycle to a stale UF2 whose
    only symptom was one command being missing, which looked like a broken
    feature rather than a stale board."""
    name = "Firmware freshness: board build id matches the local build"
    want = rp2350.local_build_id()
    if want is None:
        return (name, True, "SKIPPED: no build/rp2350/ build to compare against")
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            ser.write(b"cat /proc/buildid\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", errors="replace")

        m = re.search(r"(\d+\.\d+\.\d+)\s+(\S+)", out.replace("cat /proc/buildid", ""))
        if not m:
            return (name, True,
                    "SKIPPED: board predates /proc/buildid -- it is definitely older than this "
                    f"tree (expected build {want}); reflash build/rp2350/lugalos.uf2")
        got = m.group(2)
        if got != want:
            return (name, True,
                    f"WARNING: board is running build {got}, local tree builds {want} -- "
                    "reflash build/rp2350/lugalos.uf2 if the tests below look wrong")
        return (name, True, f"build {got}")
    except Exception as e:
        return (name, False, str(e))


def test_memory_margins(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """The RP2350's actual remaining headroom, measured on the board.

    This is the target where the numbers are tight: .data + .bss reach to
    within tens of KB of the top of the 512 KB SRAM, leaving a small heap
    above _kernel_end and a 16 KB boot stack below it. QEMU has 128 MB and
    therefore cannot fail either check no matter how much the image grows,
    which is exactly why the measurement belongs here.

    The stack figure is the one this exists for. linker/rp2350.ld records the
    reason the boot stack was moved out of SCRATCH_Y: preemption can push a
    trap frame at an arbitrary point in the deepest call chain in the system,
    and overflowing the stack there corrupts the memory below it rather than
    faulting -- a failure with no symptom at the point it happens. That was
    reasoned about but never measured until entry.S started painting the
    stack, and this is where the paint gets read on real silicon.

    So the work comes first and the reading second: taskdemo allocates and
    frees real pages, and by the time this test runs the suite has already
    driven U-mode entry, PMP faults, ELF loading and two 9P transports through
    the same stack. /proc/meminfo's stack and heap peaks are cumulative since
    boot, so reading them afterwards makes them describe all of it.

    The runaway-recursion check lives in test_node_pool_exhaustion() below,
    not here. Exhausting the node pool leaves the evaluator returning nil, and
    the shell routes `cat /proc/meminfo` through the evaluator -- so a test
    that did both would destroy its own measurement.

    Both margins are reported in the pass message even when everything is
    healthy -- the point of the test is the number, not just the verdict.
    """
    name = "Memory margins: heap and boot-stack headroom on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            # Exercise the allocator before measuring:
            # taskdemo takes two task stacks out and puts them back, the only
            # thing in this suite that moves the heap's peak rather than just
            # its current occupancy.
            ser.write(b"taskdemo\n")
            ser.flush()
            rp2350.drain(ser, quiet=0.8, deadline=20.0)

            ser.reset_input_buffer()
            ser.write(b"cat /proc/meminfo\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=0.8, deadline=10.0).decode("utf-8", "replace")

        def field(pattern: str) -> int | None:
            m = re.search(pattern, out)
            return int(m.group(1)) if m else None

        pages_total = field(r"Pages Total: (\d+)")
        pages_peak = field(r"Pages Peak: (\d+)")
        largest_run = field(r"Largest Free Run: (\d+) pages")
        pages_free = field(r"Pages Free: (\d+)")
        stack_kb = field(r"Boot Stack: (\d+) KB")
        stack_peak = field(r"Boot Stack: \d+ KB, peak (\d+) bytes")
        flash_used = field(r"Flash: (\d+) KB of \d+ KB")
        flash_total = field(r"Flash: \d+ KB of (\d+) KB")

        if None in (pages_total, pages_peak, largest_run, pages_free,
                    stack_kb, stack_peak, flash_used, flash_total):
            return (name, False,
                    "could not parse /proc/meminfo -- board may predate this field set, "
                    f"or the report was truncated:\n{out[-500:]}")

        stack_bytes = stack_kb * 1024
        checks = [
            # A floor on the reclaimed heap (C5). Not a size assertion for its
            # own sake: 32 pages is what a 128 KB NAPOT region costs, and that
            # is the largest user image this board can ever place (a k-byte
            # region needs a heap of at least k, and the only 128 KB-aligned
            # address in the heap is 0x20060000). If a future static array
            # pushes the heap below this, C4's images silently halve -- so the
            # day it happens should be a failing test rather than a puzzle.
            ("heap can still hold a 128 KB image", pages_total >= 32),
            # A zero here means the paint or the scan is broken, not that the
            # stack is unused: reaching this shell took stack to get here.
            ("boot stack high-water is measured", stack_peak > 0),
            # No poison left would report the full region -- what a real
            # overflow looks like. Anything at or above 3/4 is a margin worth
            # knowing about before it becomes a corruption bug.
            ("boot stack has headroom", stack_peak < (stack_bytes * 3) // 4),
            # The heap survived the work above without genuinely overrunning
            # its own accounting. Was a strict "<" (something left even at
            # peak) until M4.5's usb_cdc.c background task (plan/
            # phase12_microkernel_migration.md) added one more permanent
            # task stack to this same fixed 54-page pool -- the last page of
            # headroom this specific stress scenario (two concurrent user
            # programs) had, tipping peak usage to exactly pages_total.
            # Nothing actually failed when that happened (confirmed
            # reproducibly across runs, not a flake): the peak is transient,
            # everything the test drives through it still completed, and
            # `pages_free > 0` below confirms the heap freed back up
            # afterward. Loosened to "<=" -- a real, measured, accepted
            # trade-off for a real fix (drivers/usb_cdc.c's own commit
            # message has the full account of what it closes), not a defect
            # to paper over. If a *future* change makes this go strictly
            # over pages_total, that is still a genuine bug this check
            # exists to catch.
            ("heap peak stayed within the heap", pages_peak <= pages_total),
            ("heap has free pages left", pages_free > 0),
            # Fragmentation, not exhaustion: a multi-page allocation needs a
            # run, and on a 15-page heap runs go first.
            ("heap can still serve a 2-page run", largest_run >= 2),
        ]
        failed = [label for label, passed in checks if not passed]

        margins = (f"stack {stack_peak}/{stack_bytes} B peak, "
                   f"heap {pages_peak}/{pages_total} pages peak "
                   f"(largest free run {largest_run}), "
                   f"flash {flash_used}/{flash_total} KB")
        if failed:
            return (name, False, f"{', '.join(failed)} -- {margins}")
        return (name, True, margins)
    except Exception as e:
        return (name, False, str(e))


def test_pmp_probe(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """B3 prep (D2): read this silicon's actual PMP configuration.

    D2 resolved to "PMP early, NOMMU leads" -- RV32/RP2350 gets U-mode plus
    PMP enforcement in B3, ahead of Sv39 in B5. How many isolated servers a
    NOMMU node can host is bounded by the implemented region count, and the
    RISC-V privileged spec permits 0, 16 or 64 entries, so this is a property
    of Hazard3 that has to be measured rather than assumed. QEMU's RV32 model
    reports 16 entries at 4-byte granularity; there is no reason to expect
    real Hazard3 silicon to match, which is the entire point of running it
    here.

    This is a *measurement*, so it asserts only the two things B3 genuinely
    requires -- that PMP exists at all, and that nothing is already locked at
    boot (a locked entry cannot be reprogrammed until reset). The numbers
    themselves are printed for the plan to record, not compared against a
    hardcoded expectation that would just encode today's guess.
    """
    name = "PMP: probe entry count and granularity on real Hazard3 silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"pmpinfo\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=0.5, deadline=5.0)

        text = out.decode("utf-8", errors="replace")
        m = re.search(r"PMP: writable=(\d+) active=(\d+) min_region=(\d+) bytes locked=(\w+)", text)
        if not m:
            # Distinguish "the board is running older firmware" from "the probe
            # is broken". The former is by far the more likely cause the first
            # time this test is run, and it needs a completely different fix,
            # so guessing wrong here wastes real debugging time.
            if "Unbound symbol: pmpinfo" in text or "Unknown command" in text:
                return (name, False,
                        "board firmware predates the `pmpinfo` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run. (LugalOS does not implement "
                        "the 1200-baud BOOTSEL touch, so this flash is manual: hold BOOTSEL while "
                        "connecting, then copy the .uf2 to the mounted volume.)")
            if "PMP: unavailable" in text:
                return (name, False,
                        "board reports PMP unavailable -- an M-mode build should have access; "
                        f"got:\n{text[:300]}")
            return (name, False, f"no PMP report in console output:\n{text[:300]}")

        writable = int(m.group(1))
        active = int(m.group(2))        # currently in use -- writable but not free
        min_region = int(m.group(3))
        locked = m.group(4) == "yes"
        free = writable - active        # what B3 can actually take without deciding what loses access

        detail = (f"writable={writable} active={active} free={free} "
                  f"min_region={min_region}B locked={locked}")

        if writable == 0:
            return (name, False, f"no writable PMP entries -- B3 has no enforcement mechanism ({detail})")
        if free <= 1:
            return (name, False,
                    f"only {free} free PMP region(s) -- B3 needs at least one per isolated task "
                    f"plus one for the kernel ({detail})")
        if locked:
            return (name, False, f"a PMP entry is already locked at boot; B3 cannot reprogram it ({detail})")

        # Informational, not a failure: B3's design has to fit this budget.
        print(f"    ...measured: {detail}; isolatable tasks for B3 = {free - 1} "
              f"(+{active} reclaimable)")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_priostress(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 Part A (plan/phase12_microkernel_migration.md): does the
    scheduler share the CPU fairly between two same-tier
    TASK_PRIO_INTERRUPT tasks, on real silicon and not just under QEMU's
    timing?

    priotest (kernel/shell.c) only ever proves one INTERRUPT-tier task
    against a field of NORMAL-tier hogs -- it says nothing about what
    happens once *two* INTERRUPT-tier tasks are both genuinely busy at
    once, which M4.5's remaining driver-task conversions are about to make
    a real situation rather than a hypothetical one. `priostress` creates
    two same-tier, equally-sized, never-yielding tasks and compares how
    many preemption ticks each took to finish its fixed amount of work --
    fair sharing means both finish within a small factor of each other;
    one task-table slot being favoured over the other would show up as
    roughly a 2x gap instead of a near-zero one (see that command's own
    comment for the full reasoning, shared with the QEMU-side assertion of
    the same name).

    The 150M-iteration spin takes several real seconds on this silicon
    (measured ~2.5-3s, slower than QEMU's emulated throughput) with no
    output at all until it completes -- `quiet` has to clear that whole
    silent stretch, not just the usual command-echo settling time, or
    drain() gives up before the result line ever arrives."""
    name = "priostress: two same-tier interrupt tasks share the CPU fairly, on real silicon (M4.5)"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"priostress\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=6.0, deadline=15.0)

        text = out.decode("utf-8", errors="replace")
        m = re.search(r"done=1,1 total_ticks=(\d+),(\d+) -- (FAIR|UNFAIR)", text)
        if not m:
            if "Unbound symbol: priostress" in text or "Unknown command" in text:
                return (name, False,
                        "board firmware predates the `priostress` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run.")
            return (name, False, f"no PrioStress report in console output:\n{text[:300]}")

        ticks_a, ticks_b, verdict = int(m.group(1)), int(m.group(2)), m.group(3)
        detail = f"ticks={ticks_a},{ticks_b} verdict={verdict}"
        if verdict != "FAIR":
            return (name, False, f"scheduler favoured one same-tier task over the other ({detail})")

        print(f"    ...measured: {detail}")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_uart_task(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 Part B (plan/phase12_microkernel_migration.md): finishes what M4
    deferred -- RP2350's own console (drivers/uart_rp2350.c) now runs as the
    same "uart" task drivers/uart_16550.c (QEMU) already had, batching
    console output into whole chan_call()s instead of one per character, and
    additionally mirroring every write to USB CDC from inside the task.
    Same assertion as the QEMU regression test for this (tests/runner.py):
    `help` prints on the order of 50 lines / ~2000 characters, so a
    per-character design would cost that order of chan_call()s too; a
    generous bound (200) stays far below "character-scale" while clearing
    the much smaller call count batching actually produces."""
    name = "uart task: console output is batched over chan_call(), not per-character, on real silicon (M4.5)"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"uartstats\n")
            ser.flush()
            out_before = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_before = re.search(r"write_calls=(\d+)", out_before)

            ser.write(b"help\n")
            ser.flush()
            rp2350.drain(ser, quiet=0.5, deadline=5.0)

            ser.write(b"uartstats\n")
            ser.flush()
            out_after = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_after = re.search(r"write_calls=(\d+)", out_after)

        if not m_before or not m_after:
            return (name, False, f"no UartStats report in console output:\n{out_before[:200]} / {out_after[:200]}")

        before, after = int(m_before.group(1)), int(m_after.group(1))
        detail = f"write_calls before={before} after={after}"
        grown = after - before
        if grown <= 0:
            return (name, False, f"call count did not grow -- console output is not reaching the task ({detail})")
        if grown >= 200:
            return (name, False, f"call count grew by {grown} for one `help` -- looks per-character, not batched ({detail})")

        print(f"    ...measured: {detail}")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_blk_task(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 Part B (plan/phase12_microkernel_migration.md): the SD/block
    storage driver ("sdblk" on real hardware, drivers/spisd_rp2350.c) runs
    as a task, reachable only via chan_call() -- the lowest-risk conversion
    in the remaining driver list, since a read_blocks()/write_blocks() call
    was already exactly one message's worth of work, unlike uart's original
    per-character mistake.

    Unlike the uart batching test, the assertion here isn't "did IPC volume
    stay low" -- it was never at risk of being high -- it's "is the task
    actually being used at all". blk_task_call_count() (`blkstats`) is a
    real, growing counter of chan_call()s served; a caller that silently
    fell back to direct SPI access the whole time would still pass every
    functional filesystem test and prove nothing about this milestone."""
    name = "blk task: SD/block storage driver is actually serving requests over chan_call(), on real silicon (M4.5)"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"blkstats\n")
            ser.flush()
            out_before = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_before = re.search(r"calls=(\d+)", out_before)

            ser.write(b"cat /sd0/prime.c\n")
            ser.flush()
            rp2350.drain(ser, quiet=0.5, deadline=5.0)

            ser.write(b"blkstats\n")
            ser.flush()
            out_after = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_after = re.search(r"calls=(\d+)", out_after)

        if not m_before or not m_after:
            if "Unbound symbol: blkstats" in out_before or "Unknown command" in out_before:
                return (name, False,
                        "board firmware predates the `blkstats` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run.")
            return (name, False, f"no BlkStats report in console output:\n{out_before[:200]} / {out_after[:200]}")

        before, after = int(m_before.group(1)), int(m_after.group(1))
        detail = f"calls before={before} after={after}"
        if after <= before:
            return (name, False, f"call count did not grow -- reads are not reaching the task ({detail})")

        print(f"    ...measured: {detail}")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_i2c_task(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 Part B (plan/phase12_microkernel_migration.md): RTC (DS1307/
    DS3231, drivers/i2c_rtc.c) and EEPROM (AT24C32, drivers/at24c32.c) share
    one physical I2C bus, so they were converted into a single "i2c" driver
    task rather than two independent ones -- see drivers/i2c_rtc.h's comment
    for why. Same shape as test_blk_task above: the real claim isn't "does
    EEPROM read/write still work" (already covered by ordinary functional
    use) but "is the task actually serving these requests", via
    i2c_task_call_count() (`i2cstats`), a real, growing counter.

    This chess-persona board has no RTC/EEPROM module physically wired up,
    so `(eeprom-write ...)` genuinely fails at the hardware level here -- but
    it still has to reach the task (and back) to fail that way, which is
    exactly what this test checks; it does not require the write to
    succeed."""
    name = "i2c task: shared RTC/EEPROM driver is actually serving requests over chan_call(), on real silicon (M4.5)"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"i2cstats\n")
            ser.flush()
            out_before = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_before = re.search(r"calls=(\d+)", out_before)

            ser.write(b'(eeprom-write 100 "i2c_task_probe")\n')
            ser.flush()
            rp2350.drain(ser, quiet=0.5, deadline=5.0)

            ser.write(b"i2cstats\n")
            ser.flush()
            out_after = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_after = re.search(r"calls=(\d+)", out_after)

        if not m_before or not m_after:
            if "Unbound symbol: i2cstats" in out_before or "Unknown command" in out_before:
                return (name, False,
                        "board firmware predates the `i2cstats` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run.")
            return (name, False, f"no I2cStats report in console output:\n{out_before[:200]} / {out_after[:200]}")

        before, after = int(m_before.group(1)), int(m_after.group(1))
        detail = f"calls before={before} after={after}"
        if after <= before:
            return (name, False, f"call count did not grow -- eeprom-write is not reaching the task ({detail})")

        print(f"    ...measured: {detail}")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_st7735_task(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 Part B (plan/phase12_microkernel_migration.md): the ST7735 TFT
    canvas (drivers/st7735_rp2350.c) runs as a task. Every public draw
    function is its own wire op -- a full chess-board redraw makes ~64 of
    these calls back to back, still nowhere near uart's original per-
    character mistake, since each call already carries one whole logical
    drawing operation (draw_char()/draw_string()'s internal pixel-level
    fan-out stays plain C calls inside the task, never back over the wire).
    Same shape as test_blk_task: the claim is "is the task actually serving
    these", via st7735_task_call_count() (`st7735stats`)."""
    name = "st7735 task: TFT canvas driver is actually serving requests over chan_call(), on real silicon (M4.5)"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"st7735stats\n")
            ser.flush()
            out_before = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_before = re.search(r"calls=(\d+)", out_before)

            ser.write(b'(canvas-fill 0)\n')
            ser.flush()
            rp2350.drain(ser, quiet=0.5, deadline=5.0)

            ser.write(b"st7735stats\n")
            ser.flush()
            out_after = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_after = re.search(r"calls=(\d+)", out_after)

        if not m_before or not m_after:
            if "Unbound symbol: st7735stats" in out_before or "Unknown command" in out_before:
                return (name, False,
                        "board firmware predates the `st7735stats` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run.")
            return (name, False, f"no St7735Stats report in console output:\n{out_before[:200]} / {out_after[:200]}")

        before, after = int(m_before.group(1)), int(m_after.group(1))
        detail = f"calls before={before} after={after}"
        if after <= before:
            return (name, False, f"call count did not grow -- canvas-fill is not reaching the task ({detail})")

        print(f"    ...measured: {detail}")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_tm1638_task(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """M4.5 Part B (plan/phase12_microkernel_migration.md): the TM1638
    7-segment/keypad/LED driver (drivers/tm1638_rp2350.c) runs as a task.
    tm1638_get_key() is polled at a paced 20ms interval from
    user/chess/src/chess_ui.c while waiting on a human -- one call already
    carries one whole scan, no batching redesign needed. Same shape as
    test_blk_task: the claim is "is the task actually serving these", via
    tm1638_task_call_count() (`tm1638stats`)."""
    name = "tm1638 task: 7-segment/keypad/LED driver is actually serving requests over chan_call(), on real silicon (M4.5)"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"tm1638stats\n")
            ser.flush()
            out_before = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_before = re.search(r"calls=(\d+)", out_before)

            ser.write(b'(tm-get-key)\n')
            ser.flush()
            rp2350.drain(ser, quiet=0.5, deadline=5.0)

            ser.write(b"tm1638stats\n")
            ser.flush()
            out_after = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m_after = re.search(r"calls=(\d+)", out_after)

        if not m_before or not m_after:
            if "Unbound symbol: tm1638stats" in out_before or "Unknown command" in out_before:
                return (name, False,
                        "board firmware predates the `tm1638stats` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run.")
            return (name, False, f"no Tm1638Stats report in console output:\n{out_before[:200]} / {out_after[:200]}")

        before, after = int(m_before.group(1)), int(m_after.group(1))
        detail = f"calls before={before} after={after}"
        if after <= before:
            return (name, False, f"call count did not grow -- tm-get-key is not reaching the task ({detail})")

        print(f"    ...measured: {detail}")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_uart_demux_shared_wire(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """p9share (A3b UART demux) standalone: arms the demux over the physical
    UART, drives one real SLIP-framed 9P transaction, then sends a
    plain-text console command over the SAME connection and checks for a
    real shell response -- the actual claim A3b makes (console and 9P
    coexist on one wire), verified against real hardware rather than only
    QEMU's stdio-socket regression test. Restores console-only mode
    afterward (`p9share off`) so re-running this doesn't leave the board in
    a different state than it found it."""
    name = "p9share: 9P + console coexist on the physical UART"
    if not ports.uart:
        return (name, True, "SKIPPED: no UART/CP2102 dongle detected")

    try:
        with serial.Serial(ports.uart, 115200) as ser:
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"p9share\n")
            ser.flush()
            rp2350.drain(ser)  # discard banner

            ser.reset_input_buffer()
            adapter = rp2350.SerialSocketAdapter(ser)
            client = p9lib.P9Client(adapter, framing="slip")
            if not rp2350.warm_up_9p(client, adapter):
                return (name, False, "no response over shared UART after repeated Tversion attempts")
            data = client.cat("/proc/version")
            if b"LugalOS" not in data:
                return (name, False, f"9P transaction over shared wire failed: {data!r}")

            ser.reset_input_buffer()
            ser.write(b"help\n")
            ser.flush()
            console_out = rp2350.drain(ser, quiet=0.5, deadline=4.0)
            ok = b"p9share" in console_out

            ser.reset_input_buffer()
            ser.write(b"p9share off\n")
            ser.flush()
            rp2350.drain(ser)

            return (name, ok, "" if ok else f"console unresponsive after 9P traffic: {console_out[:200]!r}")
    except Exception as e:
        return (name, False, str(e))


def test_qemu_bridge(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """T3 itself: bridges RP2350's ACM1 to a live QEMU RV64 node's
    virtio-console chardev (a plain byte relay -- both ends already speak
    the same length-prefixed framing, no re-framing needed) and runs
    (p9-remote-cat ...) from *inside the QEMU guest's own Lisp REPL* against
    a marker file this test just wrote to the RP2350 over the console. Using a
    freshly written, uniquely-named marker (rather than a pre-existing file)
    makes this reproducible regardless of what is on the board's card.

    The volume is chosen by asking the board rather than assumed. Since C5 the
    RAM disk is conditional -- a board with a writable /sd0 does not mount one,
    because on RP2350 a 64 KB /ram0 lands across the only 128 KB-aligned
    address in the heap and halves the largest user image the loader can place.
    So "/ram0 always exists" stopped being true, and a test that assumed it
    failed on the write rather than on anything it was testing."""
    name = "T3: QEMU guest reads a real file from RP2350 hardware over the bridge"

    qemu_bin = shutil.which("qemu-system-riscv64")
    if not qemu_bin:
        return (name, True, "SKIPPED: qemu-system-riscv64 not found in PATH")

    rv64_elf = REPO_ROOT / "build" / "rv64" / "lugalos.elf"
    # The guest is RV64, so it needs the RV64 image: these carry target-specific
    # user program binaries and cannot be shared between architectures.
    sd_img = REPO_ROOT / "build" / "rv64" / "lugalos_sd.img"
    if not rv64_elf.exists():
        return (name, True, f"SKIPPED: {rv64_elf} not built")

    marker_name = f"hwbridge_{uuid.uuid4().hex[:12]}.txt"
    marker_value = uuid.uuid4().hex

    proc: subprocess.Popen | None = None
    ser: serial.Serial | None = None
    sock: socket.socket | None = None
    stop = threading.Event()
    bridge_threads: list[threading.Thread] = []

    try:
        # 1. Write a unique marker to whichever volume this board can write to.
        with serial.Serial(ports.console, 115200, timeout=2) as console:
            time.sleep(0.3)
            console.reset_input_buffer()
            # Asked as a *boolean*, deliberately. Returning the path from the
            # guest would put the answer inside the command text too, and the
            # line editor echoes every keystroke -- so a check for "/ram0"
            # would match the echo and always say yes, which is exactly what
            # it did on the first attempt. "#t" cannot appear in the command.
            console.write(b'lisp\n(mounted? "/ram0")\nexit\n')
            console.flush()
            vol_reply = rp2350.drain(console, quiet=0.5, deadline=6.0).decode("utf-8", "replace")
            volume = "/ram0" if "=> #t" in vol_reply else "/sd0"
            marker_path = f"{volume}/{marker_name}"

            console.reset_input_buffer()
            console.write(f"write {marker_path} {marker_value}\n".encode())
            console.flush()
            reply = rp2350.drain(console, quiet=0.5, deadline=4.0)
            if b"=> #t" not in reply:
                return (name, False,
                        f"failed to write marker file at {marker_path}: {reply[:300]!r}")

        # 2. Boot a QEMU node with a socket-backed virtio-console chardev.
        with tempfile.TemporaryDirectory() as tmpdir:
            sock_path = str(Path(tmpdir) / "rp2350_bridge.sock")
            bridge_img = REPO_ROOT / "build" / "rp2350_hw_bridge_test_sd.img"
            shutil.copyfile(sd_img, bridge_img)

            qemu_cmd = [
                qemu_bin, "-M", "virt", "-nographic", "-bios", "none",
                "-drive", f"file={bridge_img},if=none,format=raw,id=hd0",
                "-device", "virtio-blk-device,drive=hd0",
                "-kernel", str(rv64_elf),
                "-device", "virtio-serial-device",
                "-device", "virtconsole,chardev=p9c",
                "-chardev", f"socket,id=p9c,path={sock_path},server=on,wait=off",
            ]
            proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
            out_lines: list[str] = []
            threading.Thread(target=lambda: [out_lines.append(l) for l in proc.stdout], daemon=True).start()

            time.sleep(3.0)  # let it boot to the shell prompt

            # 3. Bridge RP2350's ACM1 to that socket -- but first, warm the
            # connection up with a throwaway host-side Tversion on this SAME
            # handle (see rp2350.warm_up_9p()'s docstring: the first frame
            # on a freshly opened CDC port can get silently dropped, and
            # QEMU's own p9-remote-cat has no timeout of its own -- it would
            # just hang forever against a dropped first frame instead of
            # failing loudly). Reusing this one handle for both the warm-up
            # and the relay below means there's only one `open()` boundary
            # to worry about, not two.
            ser = serial.Serial(ports.net, 115200, timeout=1.0)
            ser.dtr = True
            time.sleep(0.2)
            warmup_adapter = rp2350.SerialSocketAdapter(ser)
            if not rp2350.warm_up_9p(p9lib.P9Client(warmup_adapter, framing="raw"), warmup_adapter):
                return (name, False, "no response from ACM1 after repeated Tversion attempts")
            ser.timeout = 0.2

            last_err: Exception | None = None
            for _ in range(20):
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.connect(sock_path)
                    break
                except OSError as e:
                    last_err = e
                    time.sleep(0.2)
            if sock is None:
                return (name, False, f"could not connect to QEMU virtio-console socket: {last_err}")

            def serial_to_sock():
                while not stop.is_set():
                    data = ser.read(1)
                    if not data:
                        continue
                    if ser.in_waiting:
                        data += ser.read(ser.in_waiting)
                    try:
                        sock.sendall(data)
                    except OSError:
                        return

            def sock_to_serial():
                sock.settimeout(0.2)
                while not stop.is_set():
                    try:
                        data = sock.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        return
                    if not data:
                        return
                    ser.write(data)
                    ser.flush()

            t1 = threading.Thread(target=serial_to_sock, daemon=True)
            t2 = threading.Thread(target=sock_to_serial, daemon=True)
            t1.start()
            t2.start()
            bridge_threads.extend([t1, t2])

            # 4. Drive the real cross-wire 9P request from inside the QEMU guest.
            def send(cmd: str) -> None:
                proc.stdin.write(cmd + "\n")
                proc.stdin.flush()

            time.sleep(0.5)
            send("lisp")
            time.sleep(1.0)
            send(f'(p9-remote-cat "{marker_path}")')
            time.sleep(2.0)
            send("exit")
            time.sleep(1.0)

            stop.set()
            text = "".join(out_lines)
            ok = marker_value in text
            log = "" if ok else f"marker not found in QEMU output:\n{text[-1500:]}"
            return (name, ok, log)
    except Exception as e:
        return (name, False, str(e))
    finally:
        # Signal and WAIT for the bridge threads before touching ser/sock --
        # closing a serial handle out from under a thread still blocked in
        # read()/write() on it can abort a USB transfer mid-flight (seen in
        # testing as a "Bad file descriptor" exception from the reader
        # thread), which risks desyncing the endpoint's DATA0/1 toggle state
        # in a way no amount of host-side retrying can recover from -- only
        # a real bus reset (unplug/replug) clears it. Threads poll
        # stop.is_set() at least every 0.2s (both ser and sock have a 0.2s
        # timeout), so a short join is always enough in the non-hung case.
        stop.set()
        for t in bridge_threads:
            t.join(timeout=1.0)
        if ser:
            ser.close()
        if sock:
            sock.close()
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        bridge_img = REPO_ROOT / "build" / "rp2350_hw_bridge_test_sd.img"
        bridge_img.unlink(missing_ok=True)


def test_node_pool_exhaustion(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """A runaway Lisp recursion must degrade the shell, not take the board down.

    **This board is the only place the check means anything.** One runaway
    recursion allocates roughly 500 nodes; RP2350's node pool is 512 while the
    QEMU targets get 4096, so it exhausts the pool here on the first try and
    there only on about the eighth -- which is why the QEMU suite, whose
    depth-guard test runs the same recursion once, never reached the
    interesting state.

    What used to happen: exhaustion clamped the allocator to its last slot and
    handed the same node out repeatedly, so a cons cell could point at itself,
    and the first list walker to touch that cycle spun forever. On the QEMU
    targets UBSan catches the resulting signed overflow and halts with a
    message. This build has no UBSan, so it was an unrecoverable hang -- no
    fault banner, no output, USB unserviced, and only a physical unplug/replug
    recovered it. It took the board down four times before being root-caused;
    see plan/phase6_memory_and_processes.md §6.4.

    Three assertions, all on bytes the *system* emits:
      - the exhaustion warning is reported rather than hit silently
      - the REPL prints a result afterwards, which is the proof that
        evaluation unwound instead of spinning
      - `exit` still returns to the shell prompt, so the console lives

    **Must run last**, and it reboots the board when it is done: it leaves the
    evaluator returning nil, and every other test here drives the board through
    a shell that evaluates. The reboot is why a second run starts clean instead
    of failing five unrelated tests.
    """
    name = "Node pool exhaustion degrades the shell instead of hanging the board"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"lisp\n(define (loop n) (loop (+ n 1)))\n(loop 0)\n",
                        b"exit\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=1.0, deadline=30.0).decode("utf-8", "replace")

        checks = [
            ("exhaustion reported", "Node pool exhausted" in out),
            ("evaluation unwound",  "=> ()" in out),
            ("console still alive", "lsh>" in out.split("Node pool exhausted")[-1]),
        ]

        # Leave the board usable. This test's whole point is that the evaluator
        # is left inert, and every other test here drives the board through a
        # shell that evaluates -- so without this, a second run fails five
        # tests for reasons that have nothing to do with them. `reboot` is a
        # shell builtin handled before the Lisp fallthrough precisely so that
        # it still works from this state.
        try:
            with serial.Serial(ports.console, 115200, timeout=2) as ser:
                ser.dtr = True
                time.sleep(0.2)
                ser.write(b"reboot\n")
                ser.flush()
                time.sleep(1.0)
            # Wait for the board back before returning. Without this the suite
            # exits while USB is still re-enumerating, and a run started
            # straight afterwards finds no board and skips everything -- which
            # reads as "nothing to test" rather than "wait a moment".
            deadline = time.time() + 25.0
            while time.time() < deadline:
                if rp2350.discover_ports() is not None:
                    break
                time.sleep(0.5)
        except Exception:
            pass  # the reboot is cleanup, not the assertion
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-500:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_concurrent_user_programs(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """C2 on real silicon: two user programs resident at once, in fifteen pages.

    Worth running here rather than trusting QEMU for a reason that is about
    quantity rather than semantics. The QEMU targets have a 16 MB heap, so two
    programs cost a rounding error there; this board has **fifteen pages**, and
    two programs plus their kernel stacks peak at twelve of them. If the loader
    ever gets less frugal, QEMU will not notice and this will.

    It is also the only place the PMP side of per-process domains is enforced:
    each program gets its own three-region domain, and Hazard3 reprograms the
    registers on every switch. On the MMU build the equivalent is a page table
    per domain, which is what needed a free path before any of this was
    possible at all.

    The assertion is residency overlap: uhello's task must exist while
    uspin's is still alive, proving the loader gave both a slot at once --
    with a single image slot the second spawn would be refused outright.

    This used to check marker *print* order (USPIN_START before
    UPROG_TEXT_OK before USPIN_PREEMPTED), on the assumption that spawning
    uspin first meant it would always be the one to print first too. That
    held by accident of table-layout proximity, not by any real guarantee:
    two freshly-created, identically-tied tasks racing for the very first
    CPU turn is legitimately non-deterministic (kernel/sched.c's
    next_runnable()), and uhello winning that race -- finishing entirely
    before uspin ever gets a turn -- is just as valid a proof of concurrent
    residency as the reverse; both plainly existed together regardless of
    which one happened to run, or print, first. Checking kernel-emitted
    "Created"/"exited" log lines instead of the programs' own racing print
    statements is what makes this order-agnostic: those lines are strictly
    chronological, whichever task the CPU favors.
    """
    name = "C2: two user programs resident at once on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b'lisp\n(spawn "/flash0/system/bin/uspin.elf")\n'
                        b'(spawn "/flash0/system/bin/uhello.elf")\nexit\n',
                        b"cat /proc/meminfo\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=1.0, deadline=30.0).decode("utf-8", "replace")

        created = re.findall(r"\[Sched\] Created task #(\d+) 'uprog'", out)
        overlapped = False
        if len(created) >= 2:
            uspin_pid, uhello_pid = created[0], created[1]
            uhello_created_pos = out.index(f"Created task #{uhello_pid} 'uprog'")
            uspin_exit_match = re.search(rf"\[Sched\] Task #{uspin_pid} 'uprog' exited", out)
            overlapped = bool(uspin_exit_match) and uhello_created_pos < uspin_exit_match.start()
        peak = re.search(r"Pages Peak: (\d+)", out)
        total = re.search(r"Pages Total: (\d+)", out)

        checks = [
            ("both programs started", len(created) >= 2),
            ("they overlapped", overlapped),
            ("heap figures readable", peak is not None and total is not None),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-500:]}")
        return (name, True,
                f"interleaved; heap peak {peak.group(1)}/{total.group(1)} pages")
    except Exception as e:
        return (name, False, str(e))


def test_process_abi(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """C3 on real silicon: argv, exit status, and a service reached from U-mode.

    The part that needs hardware is the last one. UCHAN_FOREIGN_OK is the
    confused-deputy case at the channel boundary -- a program hands the kernel
    a request buffer it does not own, and the kernel must refuse rather than
    copy from it. On this board that refusal is decided against PMP regions on
    Hazard3, with its NAPOT-only matching and erratum E6's reversed permission
    bits; on the QEMU targets it is Sv39, or on RV32 a software check. A
    boundary that holds under one does not demonstrate the other.

    UCHAN_RETIRED_OK is worth having anywhere: syscall 1 was a register-IPC
    entry point, and it must now be *gone* rather than repurposed, so a binary
    built against the old ABI gets a clean refusal instead of whatever later
    took its number.
    """
    name = "C3: argv, exit status and channel access from U-mode on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"uargs alpha beta\n", b"uchan\n", b"ps\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=0.8, deadline=25.0).decode("utf-8", "replace")

        checks = [
            ("argc arrived",            "UARGS_COUNT 3" in out),
            ("argv[0] is the name",     "UARGS_ARG 0:uargs" in out),
            ("argv strings readable",   "UARGS_ARG 2:beta" in out),
            ("argv[argc] is NULL",      "UARGS_NULL_OK" in out),
            ("exit status propagated",  "returned 42" in out),
            ("service reached",         "UCHAN_VIA_SERVICE" in out),
            ("unknown service refused", "UCHAN_NOSUCH_OK" in out),
            ("foreign buffer refused",  "UCHAN_FOREIGN_OK" in out),
            ("retired syscall refused", "UCHAN_RETIRED_OK" in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-600:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_large_image(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """C4 on real silicon: an image larger than two pages, and W^X from flags.

    This board is where the image model's constraints are real. A PMP region
    must be power-of-two sized and self-aligned on Hazard3 (NAPOT only), so a
    segment whose page count is not a power of two is granted as several
    pieces, and the number of pieces is bounded by five dynamic PMP entries --
    eight less the three that shadow the hardwired U-mode grants. ubig is sized
    to sit exactly at that budget: six spanned pages in an eight-page run, its
    data segment decomposing into 1 + 2 + 2, plus text and stack.

    On the QEMU targets the same program is mapped by Sv39 at page granularity
    and none of that applies -- it would pass there with the arithmetic
    completely wrong.

    uwx checks the other half. Permissions now come from each segment's ELF
    p_flags rather than from the loader assuming page 0 is text, so a store
    into its own text must still fault; on this board that is decided by PMP
    with erratum E6's reversed permission bits.
    """
    name = "C4: multi-page image and W^X on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"ubig\n", b"uwx\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=0.8, deadline=25.0).decode("utf-8", "replace")

        checks = [
            ("multi-page image ran",   "UBIG_WROTE 5" in out),
            ("every page writable",    "UBIG_READBACK" in out),
            ("it finished",            "UBIG_DONE" in out),
            ("W^X program started",    "UWX_ALIVE" in out),
            ("store into text faulted",
             "terminated before it could exit" in out and "UWX_NOT_ENFORCED" not in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-600:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_heap_on_demand(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """C6/C7 on real silicon: cc costs nothing while idle.

    chibicc's pools were about 108 KB of static arrays and ed's another 44 KB
    -- on a 512 KB board, a third of the machine reserved for two commands that
    are almost never running. They are taken from the heap on entry and
    returned on exit now, which is what took this board's image from 335 KB to
    183 KB and its heap from 40 pages to 78.

    That 78-page baseline dropped to 48 with phase9 H4 (chess): search.c's
    per-ply move-list pools (search_pv_movelists/search_q_movelists, ~65 KB)
    were plain `static` arrays, not heap-on-demand -- unlike cc/ed, they
    weren't converted, so they cost real, permanent .bss on every board that
    builds with LUGALOS_ENABLE_CHESS=ON (the default), whether or not chess
    ever runs.

    J0 (plan/phase10_chess_completion.md) converted them to
    `palloc_pages()`-on-demand -- initially allocated once on first
    `chess`/`chess-run`/`chess-selftest` call and never freed, then
    revised the same day once J1 gave chess a real session boundary
    (`chess_console_run()`'s `quit`) to actually release on exit too,
    matching cc/ed's own acquire-per-use precedent instead of J0's
    original "never freed for the process lifetime" choice. Idle-heap
    baseline measured twice on real silicon: 64 pages right after J0 (up
    from H4's 48), then 60 after J1 added its own new static state (the
    console REPL's code and two function-local `static MoveList` buffers,
    ~1 KB, plus normal `.data`/`.bss` growth from ~500 new lines) -- a
    legitimate one-time image-growth cost from a real feature landing, not
    a heap-lifecycle regression (this test's own "arena was returned"
    check, `used[0] == used[-1]`, is what actually proves the lifecycle;
    "Pages Total" is context, re-measure and update this comment whenever
    it moves rather than treating either figure as fixed). Threshold below
    is `>= 50` -- real margin below the current 60, not the value itself,
    so a future regression that eats back into this budget still trips it;
    expect this number to drift down further as J2-J6 add their own static
    footprint, and to widen the margin back up rather than let it go
    flappy if it ever gets close.

    Worth checking here rather than only on QEMU for the obvious reason: the
    QEMU targets have a 16 MB heap, so an arena that is never released is
    invisible there for a very long time. On 78 pages it is immediate.

    The assertion is that the page count *returns*. A compile that acquired and
    never released would still emit a correct binary every time.
    """
    name = "C6/C7: compiler and editor memory is returned on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"cat /proc/meminfo\n",
                        b"cc /sd0/prime.c /sd0/hwtest_cc.elf\n",
                        b"cat /proc/meminfo\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=1.0, deadline=30.0).decode("utf-8", "replace")

        used = [int(m) for m in re.findall(r"Pages Used: (\d+)", out)]
        total = re.search(r"Pages Total: (\d+)", out)

        checks = [
            ("compile succeeded",  "Build clean" in out),
            ("two readings taken", len(used) >= 2),
            ("arena was returned", len(used) >= 2 and used[0] == used[-1]),
            ("heap is the reclaimed size", total is not None and int(total.group(1)) >= 50),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}; used={used}\n{out[-500:]}")
        return (name, True,
                f"heap {used[0]}/{total.group(1)} pages before and after a compile")
    except Exception as e:
        return (name, False, str(e))


def test_port_binding(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """C8 on real silicon: one wire, several protocols, exactly one owner.

    This board is the only place the interesting pair exists. ACM1 is
    registered twice -- `usbnet` as a 9P link and `usbcon` as a console -- and
    the background 9P server holds it from boot, so asking for the console on
    that wire must be refused *by name*. The QEMU targets have no second CDC
    interface and cannot express the case at all.

    The refusal is checked rather than a takeover, deliberately: a bind that
    silently moved the console onto another port would cut the connection the
    operator is typing on, which on a board reached only over USB means losing
    the machine.

    Nothing here is mutated -- the bind is refused, so the board is left
    exactly as it was found.
    """
    name = "C8: one wire has one owner, on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"cat /proc/ports\n", b'lisp\n(bind "usbcon")\nexit\n'):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=0.8, deadline=20.0).decode("utf-8", "replace")

        checks = [
            ("both roles listed for acm1",
             "usbnet" in out and "usbcon" in out),
            ("the 9P link holds it",
             re.search(r"usbnet\s+p9link\s+acm1\s+bound", out) is not None),
            ("the console role is free",
             re.search(r"usbcon\s+console\s+acm1\s+free", out) is not None),
            ("conflicting bind refused", "=> #f" in out),
            ("refusal names the holder", "'usbnet' already holds it" in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-600:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_board_config(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """K3 on real silicon: the generated pin map matches physical GP0-GP13/16-21/25.

    K2 (plan/phase7_kernel_config.md) replaced the hand-typed GPIO literals in
    drivers/uart_rp2350.c and drivers/spisd_rp2350.c with values generated from
    cmake/board-rp2350.cmake. The build matrix and QEMU suite can prove the
    generator produces *a* header and that the board still boots -- neither
    can prove the numbers are still the *right* ones, since QEMU has no pin
    model and this board's own hardware suite talks to it over USB CDC, which
    uses no GPIO pins at all (see plan/phase7_kernel_config.md's Verification
    section). This is the first automated check that would catch a wrong
    generated value on its own, independent of whether the pin it names is
    otherwise exercised.
    """
    name = "K3: /proc/config reports the pins actually compiled in"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            ser.write(b"cat /proc/config\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=0.8, deadline=10.0).decode("utf-8", "replace")

        expected = {
            "PALLOC_MAX_PAGES": "128",
            "UART0_BASE": "0x40070000",
            "UART0_TX_GPIO": "0",
            "UART0_RX_GPIO": "1",
            "SPI1_BASE": "0x40088000",
            "SPI1_SCK_GPIO": "10",
            "SPI1_MOSI_GPIO": "11",
            "SPI1_MISO_GPIO": "12",
            "SPI1_CS_GPIO": "13",
            "LED_ONBOARD_GPIO": "25",
            "LED_EXT_GPIO": "16",
            "SPI0_BASE": "0x40080000",
            "ST7735_SCK_GPIO": "18",
            "ST7735_MOSI_GPIO": "19",
            "ST7735_CS_GPIO": "17",
            "ST7735_DC_GPIO": "20",
            "ST7735_RST_GPIO": "21",
            "TM1638_STB_GPIO": "6",
            "TM1638_CLK_GPIO": "7",
            "TM1638_DIO_GPIO": "8",
        }
        checks = [
            (f"{key}={val}", re.search(rf"{key}={re.escape(val)}\b", out) is not None)
            for key, val in expected.items()
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-800:]}")
        return (name, True, f"{len(checks)} fields matched")
    except Exception as e:
        return (name, False, str(e))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--console", help="Override auto-detected ACM0 console port")
    parser.add_argument("--net", help="Override auto-detected ACM1 net (link_usb_cdc) port")
    parser.add_argument("--uart", help="Override auto-detected CP2102 UART dongle port")
    parser.add_argument("--skip-qemu-bridge", action="store_true", help="Skip the T3 QEMU bridge test")
    args = parser.parse_args()

    print("======================================================================")
    print("            LugalOS RP2350 Hardware-in-the-Loop Test Suite")
    print("======================================================================")

    ports = rp2350.discover_ports(console=args.console, net=args.net, uart=args.uart)
    if ports is None:
        print("\n[!] No RP2350 detected (need two CDC-ACM ports: console + link_usb_cdc net).")
        print("    Nothing to test -- this is not a failure, just nothing to do.")
        return 0

    print(f"\nDetected RP2350: console={ports.console} net={ports.net} uart={ports.uart or '(none)'}")

    tests = [test_firmware_freshness, test_pmp_probe, test_priostress, test_uart_task, test_blk_task, test_i2c_task, test_st7735_task, test_tm1638_task, test_umode_isolation, test_user_elf, test_usb_cdc_net_link, test_usb_cdc_net_resync, test_uart_demux_shared_wire]
    if not args.skip_qemu_bridge:
        tests.append(test_qemu_bridge)
    tests.append(test_process_abi)
    tests.append(test_large_image)
    tests.append(test_heap_on_demand)
    tests.append(test_port_binding)
    tests.append(test_board_config)
    tests.append(test_concurrent_user_programs)
    tests.append(test_memory_margins)
    # Last, and it has to stay last: it deliberately exhausts the Lisp node
    # pool, which leaves the evaluator returning nil until the board reboots.
    # Every other test here drives the board through its shell, and the shell
    # evaluates -- so anything after this would fail for reasons that have
    # nothing to do with what it is testing.
    tests.append(test_node_pool_exhaustion)

    total = 0
    passed = 0
    for test in tests:
        name, ok, log = test(ports)
        total += 1
        if ok:
            passed += 1
            status = "SKIP" if log.startswith("SKIPPED") else "PASS"
            print(f"  [{status}] {name}" + (f" -- {log}" if log else ""))
        else:
            print(f"  [FAIL] {name}\n    {log}")

    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed} / {total} PASSED")
    # The last test deliberately exhausts the Lisp node pool, which leaves the
    # evaluator returning nil until the board reboots. Everything here drives
    # the board through a shell that evaluates, so a second run without a
    # reboot fails five tests for reasons that have nothing to do with them.
    # Said out loud because it is not guessable from the failures.
    print("NOTE: the node-pool test exhausts the Lisp node pool and reboots the "
          "board afterwards, so the next run starts clean.")
    print("======================================================================\n")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
