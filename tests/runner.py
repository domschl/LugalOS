#!/usr/bin/env python3
"""Automated Test Runner & Inspection Framework for LugalOS Microkernel Operating System."""

from __future__ import annotations

import os
import re
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import BinaryIO


def expected_version() -> str:
    """The version string kernel/include/kernel/version.h defines, as a regex.

    Read from the header rather than hardcoded: the version is now bumped
    regularly, and a literal here would break several unrelated tests on every
    bump -- the same brittleness that hardcoding init.lisp's byte length caused
    (see expected_init_lisp below)."""
    src = Path(__file__).resolve().parent.parent / "kernel" / "include" / "kernel" / "version.h"
    m = re.search(r'#define\s+LUGALOS_VERSION\s+"([^"]+)"', src.read_text())
    if not m:
        raise RuntimeError("could not read LUGALOS_VERSION from version.h")
    return re.escape(m.group(1))


def expected_init_lisp() -> bytes:
    """The bytes /sd0/system/etc/init.lisp should contain, read from the source
    that tools/create_sd_image.py copies onto the image.

    The three 9P tests below assert on the full length of this file to prove a
    complete multi-read transfer rather than a truncated one -- a real thing to
    check. They used to hardcode that length (515), which meant any edit to the
    boot script broke three unrelated 9P transport tests with a confusing
    "unexpected content" failure. Deriving it keeps the same assertion without
    the brittleness.
    """
    src = Path(__file__).resolve().parent.parent / "tools" / "sd_root" / "system" / "etc" / "init.lisp"
    return src.read_bytes()


class QemuSession:
    """Manages an interactive QEMU session for testing LugalOS serial I/O."""

    # Markers that indicate the guest has crashed or corrupted state. If any of
    # these appear in the output window, the test is failed immediately rather
    # than being allowed to match its expected pattern anyway (or padding out
    # to a timeout with no useful diagnostic).
    FAULT_MARKERS = ("[UBSan Fault]", "[UBSan Fatal]", "[Trap Exception]", "[Fatal]")

    def __init__(self, elf_path: Path, img_path: Path, arch: str) -> None:
        self.elf_path: Path = elf_path
        self.img_path: Path = img_path
        self.arch: str = arch
        self.process: subprocess.Popen[bytes] | None = None
        self._log_path: Path | None = None
        self._log_file: "BinaryIO | None" = None  # read-mode file object, opened in start()

    def start(self, extra_qemu_args: list[str] | None = None) -> None:

        qemu_bin = "qemu-system-riscv64" if "64" in self.arch else "qemu-system-riscv32"
        cmd: list[str] = [
            qemu_bin,
            "-M", "virt",
            "-nographic",
            "-bios", "none",
            "-d", "guest_errors,unimp",
            "-drive", f"file={self.img_path},if=none,format=raw,id=hd0",
            "-device", "virtio-blk-device,drive=hd0",
            "-kernel", str(self.elf_path),
        ]
        if extra_qemu_args:
            cmd.extend(extra_qemu_args)

        # QEMU's '-nographic' stdio chardev sets O_NONBLOCK on its console
        # fd(s), and a write() that returns EAGAIN because the reading side
        # has not drained fast enough is not always retried -- documented,
        # longstanding QEMU behavior, not something specific to this
        # harness or a PTY vs. pipe choice. The Linux kernel's own nolibc
        # selftests hit exactly this running QEMU the same way (Willy
        # Tarreau, LKML thread on QEMU-based selftests, 2023): piping
        # QEMU's '-nographic' output "randomly mangled and/or missing
        # contents... I suspect it has something to do with non-blocking
        # writes being used to avoid blocking the emulation." Their fix,
        # adopted here: "It's only when sent to a file that it's OK." A
        # regular file's write() essentially never returns EAGAIN, so
        # QEMU's console output goes to a real log file instead of a pipe
        # or PTY, and this class tails that file rather than reading a
        # chardev fd directly. Commands still go in over a plain pipe on
        # stdin -- the documented issue and every report of it are
        # specifically about QEMU's *output* side.
        #
        # Worth keeping despite the *actual* cause of this suite's own
        # multi-hundred-second stalls turning out to be different (see
        # send_and_expect()'s comment): breaking into QEMU's monitor
        # mid-stall, while this was still suspect #1, read back a fully
        # completed guest exchange from well under a second of simulated
        # time -- proof the *guest* was never at fault, even though the
        # real proximate cause on the *host* side was elsewhere. The
        # O_NONBLOCK/EAGAIN behavior above is still real, still documented,
        # and still the shape every external report of "QEMU output piped
        # somewhere unreliable" describes, so this stays as a genuine
        # defense against a class of failure this harness cannot fully
        # rule out having also hit, just no longer the presumed sole cause.
        log_fd, log_path_str = tempfile.mkstemp(prefix="lugalos_qemu_", suffix=".log")
        self._log_path = Path(log_path_str)
        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=log_fd,
            stderr=log_fd,
            close_fds=True,
        )
        # The child (QEMU) received its own duplicated copy of log_fd via
        # dup2() when Popen() set up its stdout/stderr; this process's copy
        # is no longer needed once that handoff has happened; nothing here
        # reads or writes through log_fd itself; reading happens in the
        # separate read-mode handle below.
        os.close(log_fd)
        self._log_file = open(self._log_path, "rb")

    def _drain(self) -> None:
        """Discard any output left over from a previous call by seeking
        past it, rather than reading and throwing it away -- the log file
        keeps growing underneath this session for its whole lifetime, so
        "discard" here means "start the next read from here", not "erase".
        Without this, a pattern from the *next* test's own echoed command
        (or trailing output the previous test never consumed) can bleed in
        and produce a false PASS before the new command has even been
        sent. """
        if self._log_file is None:
            return
        self._log_file.seek(0, os.SEEK_END)

    @staticmethod
    def _strip_echo(text: str, command: str) -> str:
        """Remove the guest's echo of what we just typed from the captured
        output before matching. The interactive line editor echoes every
        keystroke back (including embedded newlines between subcommands,
        normalized to '\\r\\n'), so the literal command text is always the
        first thing to appear -- and an expected_pattern that happens to be a
        substring of the command itself would otherwise match its own echo
        instead of any real response from the guest.

        count=1 matters: _drain() guarantees the buffer is empty right
        before we write the command, so the *first* occurrence of the
        command text is always the actual echo -- but a short, common
        command (e.g. "help") can legitimately recur later in real output
        (e.g. inside "(help)"). Stripping every occurrence, not just the
        first, silently deleted that later legitimate text too and broke
        matching against it."""
        if not command:
            return text
        escaped_lines = [re.escape(line) for line in command.split("\n")]
        echo_pattern = r"\r?\n".join(escaped_lines)
        return re.sub(echo_pattern, "", text, count=1)

    def send_and_expect(self, command: str, expected_pattern: str, timeout: float = 4.0) -> tuple[bool, str]:
        """Writes `command` to QEMU's stdin, then tails its log file --
        read()-what's-new, sleep briefly, repeat -- until `expected_pattern`
        matches or `timeout` elapses.

        This method (and how it reads QEMU's output -- see start()'s
        comment for that half) went through several revisions chasing what
        looked, for a long time, like an external QEMU stdio quirk: a
        pipe- or PTY-based reader would go 60+ real seconds without seeing
        already-produced guest output at all, on two different host
        platforms, confirmed by breaking into QEMU's own monitor mid-stall
        and reading back a fully completed guest exchange from well under
        a second of simulated time. That evidence was real, but pointed at
        the wrong layer: QEMU's chardev *is* documented to have exactly
        this failure shape (O_NONBLOCK on its console fd, a write() that
        returns EAGAIN when the reader hasn't drained fast enough, not
        always retried -- see start()'s comment), so every symptom lined
        up. The switch to a log file (this method's actual current shape)
        was made on that basis and is kept -- it is still a real, cheap
        defense against that documented class of failure.

        It did not fix the stalls, though -- not on its own. The actual
        cause, found only once a stall reproduced with the *file* already
        holding the correct, complete answer while this method still
        hadn't returned: `check()` below calls `regex.search()` against
        `expected_pattern` compiled with re.DOTALL, and roughly forty of
        this suite's patterns wrote `(.|\\n)*` for "any text, including
        newlines" -- redundant under DOTALL, since `.` already matches
        `\\n` there, but not *harmless*: an alternation between two things
        that can both match the same character, repeated with `*`, is a
        textbook catastrophic-backtracking shape, and Python's `re` engine
        has no protection against it. On specific accumulated-output
        lengths this took tens of seconds to minutes to fail to complete
        at all -- indistinguishable, from outside this function, from "no
        new output arrived," which is exactly why it read as a QEMU/host
        problem for so long. Replacing `(.|\\n)*` with `.*` throughout this
        file (semantically identical under DOTALL) removed the pathological
        case entirely; every stall this suite had been hitting, across two
        platforms and every I/O transport tried, stopped reproducing once
        that one change landed -- the file-based read stayed in as
        legitimate hardening, not as the fix that actually mattered. """
        if not self.process or self.process.stdin is None or self._log_file is None:
            return False, "Process not running"

        self._drain()

        if command:
            self.process.stdin.write((command + "\n").encode())
            self.process.stdin.flush()

        accumulated = b""
        start_time = time.time()
        regex = re.compile(expected_pattern, re.MULTILINE | re.DOTALL)

        def check(raw: bytes) -> tuple[bool, str]:
            text = raw.decode("utf-8", "replace")
            return bool(regex.search(self._strip_echo(text, command))), text

        while time.time() - start_time < timeout:
            chunk = self._log_file.read()
            if not chunk:
                time.sleep(0.02)
                continue
            accumulated += chunk

            if any(marker.encode() in accumulated for marker in self.FAULT_MARKERS):
                return False, accumulated.decode("utf-8", "replace")
            matched, text = check(accumulated)
            if matched:
                return True, text

        return False, accumulated.decode("utf-8", "replace")

    def close(self) -> None:
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=1.0)
            except Exception:
                self.process.kill()
            if self.process.stdin:
                try:
                    self.process.stdin.close()
                except OSError:
                    pass
            self.process = None
        if self._log_file is not None:
            try:
                self._log_file.close()
            except OSError:
                pass
            self._log_file = None
        if self._log_path is not None:
            try:
                self._log_path.unlink()
            except OSError:
                pass
            self._log_path = None


def test_qemu_architecture(elf_path: Path, img_path: Path, arch_name: str) -> list[tuple[str, bool, str]]:
    """Runs functional integration tests against a specific LugalOS build target."""
    import shutil
    arch_img = img_path.with_name(f"test_{arch_name}_sd.img")
    shutil.copyfile(img_path, arch_img)

    results: list[tuple[str, bool, str]] = []
    session = QemuSession(elf_path, arch_img, arch_name)

    try:
        session.start()

        # 1. Boot Verification
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        results.append(("Kernel Boot & Shell Prompt", ok, log if not ok else ""))

        # 2. Plan 9 /proc/ Metrics
        ok, log = session.send_and_expect("cat /proc/version", rf"LugalOS v{expected_version()}", timeout=3.0)

        results.append(("/proc/version Metrics", ok, log if not ok else ""))

        # B2: /proc/ps now renders the real task table. Before B2 it was a
        # hardcoded string naming four tasks that did not exist, so this used
        # to assert on "vfs_server" -- a name nothing was ever scheduled under.
        ok, log = session.send_and_expect("cat /proc/ps", r"0\s+RUNNING\s+kernel", timeout=3.0)
        results.append(("/proc/ps Renders The Real Task Table (B2)", ok, log if not ok else ""))

        # B12 (closed in B6): the ELF loader must reject malformed headers
        # rather than reading outside the buffer it loaded the file into.
        #
        # tools/sd_root/badelf.bin is a syntactically valid ELF32 RISC-V header
        # whose e_phoff points far past the end of the file and whose e_phnum
        # claims 100 program headers. That is the exact defect B12 recorded:
        # e_phoff indexed the read buffer unchecked and e_phnum drove a loop
        # over it. `exec` runs whatever is on the SD card, so this is reachable.
        #
        # The assertion is both halves: a clean diagnostic AND the shell still
        # alive afterwards. send_and_expect()'s FAULT_MARKERS check makes the
        # second half real -- under UBSan an out-of-bounds read halts the guest.
        ok, log = session.send_and_expect(
            "exec /sd0/badelf.bin\nhelp",
            r"(does not fit|past end of file).*Available LugalOS Shell Commands",
            timeout=6.0)
        results.append(("ELF Loader Rejects Malformed Program Headers (B12)",
                        ok, log if not ok else ""))

        # B6: a separately linked ELF, loaded from the filesystem into pages
        # the allocator handed out, running in U-mode under a memory domain.
        #
        # Each marker is a different part of the image model, so a failure says
        # which part broke rather than only that the program did not finish:
        #
        #   UPROG_TEXT_OK   a string literal read from the program's own
        #                   .rodata -- the thing kernel/shell.c's `.utext`
        #                   probes cannot do, and the reason they assemble
        #                   every message one character at a time
        #   UPROG_DATA_OK   a store to a writable global on the image's second
        #                   page, which faults if that page is not mapped R|W
        #   UPROG_FILE_OK   a pointer-taking syscall: the kernel read a path
        #                   out of user .rodata and copied file content back
        #                   into a user .bss buffer, both validated against
        #                   this task's domain
        #   returned 7      the program ended by *returning*, through the exit
        #                   stub the loader plants at its return address, with
        #                   its value carried across as the exit status
        #
        # The version string is matched inside UPROG_FILE_OK deliberately: a
        # syscall returning a non-negative number would satisfy a weaker test
        # without any bytes having crossed the boundary.
        ok, log = session.send_and_expect(
            "exec /flash0/system/bin/uhello.elf",
            r"UPROG_TEXT_OK.*UPROG_DATA_OK.*UPROG_FILE_OK LugalOS v"
            + expected_version() + r".*returned 7",
            timeout=6.0)
        results.append(("Separately Linked ELF Runs In U-mode (B6)", ok, log if not ok else ""))

        # B6: and the same loader path is confined.
        #
        # `isolationtest` already probes a domain, but with kernel code in a
        # kernel section -- it cannot show that a program which came off the
        # filesystem and was placed in allocator pages is contained, because
        # before B6 such a program was *called* at kernel privilege.
        #
        # Both outcomes are asserted. UISO_NOT_ISOLATED is printed by the
        # program itself if its out-of-domain store was allowed, so a build
        # that grants too much fails loudly instead of looking like a pass.
        ok, log = session.send_and_expect(
            "exec /flash0/system/bin/uisolate.elf\nhelp",
            r"UISO_ALIVE.*terminated before it could exit"
            r".*Available LugalOS Shell Commands",
            timeout=6.0)
        if ok and "UISO_NOT_ISOLATED" in log:
            ok = False
        results.append(("Loaded User Program Is Confined By Its Domain (B6)",
                        ok, log if not ok else ""))

        # B6: a timer interrupt reaches user code, and user code survives it.
        #
        # `preempttest` above covers the same mechanism for a *kernel* task.
        # This is the narrower question about the code least entitled to
        # cooperate: a program loaded off the filesystem, running in U-mode.
        #
        # It reads the kernel's tick counter, spins making no syscall at all,
        # and reads it again. Only the timer interrupt handler advances that
        # counter, so a count that moved means an interrupt was taken while
        # *user* code was running -- and that the program was resumed
        # correctly, since it is still there to print the result.
        #
        # This test cannot fail on QEMU, and that is worth stating plainly:
        # QEMU implements the privileged spec's rule that interrupts for a
        # higher privilege level are always enabled while the hart runs at a
        # lower one, so U-mode is preemptible there whatever mstatus.MIE says.
        # Real RP2350 silicon does not -- the same program reports zero ticks
        # across a 0.4 s spin unless umode.S sets MPIE before mret. So the
        # assertion that has teeth is the hardware one in
        # tests/hw/test_rp2350.py; this is its QEMU-side twin, kept so the
        # program and the marker stay exercised on every run.
        #
        # The program prints USPIN_NOT_PREEMPTED itself when the count did not
        # move, so a failure is a loud line rather than a missing one.
        ok, log = session.send_and_expect(
            "exec /flash0/system/bin/uspin.elf",
            r"USPIN_START.*USPIN_PREEMPTED.*USPIN_YIELD_OK.*USPIN_SERVE_REFUSED",
            timeout=25.0)
        if ok and "USPIN_NOT_PREEMPTED" in log:
            ok = False
        results.append(("U-mode Code Is Preemptible (B6)", ok, log if not ok else ""))

        # M5, plan/phase12_microkernel_migration.md: SYS_YIELD/SYS_TIME_MS
        # round-trip correctly from U-mode -- the two syscalls a long-lived
        # U-mode driver task's own poll loop needs, verified here (cheaply,
        # on QEMU) before the RP2350-specific heartbeat-to-U-mode conversion
        # that actually depends on them touches real hardware. Already
        # exercised as part of the uspin run above; this just asserts the
        # marker specifically, so a regression here fails with its own name
        # rather than only showing up as a symptom of the B6 test above.
        results.append(("U-mode SYS_YIELD/SYS_TIME_MS Round-Trip (M5)",
                        "USPIN_YIELD_OK" in log, log if "USPIN_YIELD_OK" not in log else ""))

        # M5 Phase 2, plan/phase12_microkernel_migration.md: SYS_DELAY_US and
        # the SYS_CHAN_SERVE_WAIT/SYS_CHAN_SERVE_REPLY plumbing, verified
        # here before the tm1638-to-U-mode conversion that depends on them
        # touches real hardware. USPIN_SERVE_NOT_REFUSED would mean either
        # call somehow succeeded against a name nothing ever registered --
        # a false pass wearing a "refused" label, not just a missing marker.
        results.append(("U-mode SYS_DELAY_US Round-Trip (M5 Phase 2)",
                        "USPIN_DELAY_OK" in log, log if "USPIN_DELAY_OK" not in log else ""))
        serve_ok = "USPIN_SERVE_REFUSED" in log and "USPIN_SERVE_NOT_REFUSED" not in log
        results.append(("U-mode SYS_CHAN_SERVE_WAIT/REPLY Refuse Unknown Endpoint (M5 Phase 2)",
                        serve_ok, log if not serve_ok else ""))

        # M5 Phase 2: a real client blocking on chan_call() into a real
        # U-mode server, which itself blocks mid-ecall-trap inside
        # SYS_CHAN_SERVE_WAIT waiting to be woken by that same call --
        # `chanechotest` (kernel/shell.c). Found and fixed here, on QEMU,
        # before this mechanism ever touched real hardware again: the
        # string literal endpoint name in the first attempt at this landed
        # in ordinary .rodata, outside every region the task's domain
        # grants, so strncpy_from_user() correctly refused it -- on real
        # RP2350 silicon that refusal manifested as a full board hang
        # rather than a clean error, which is exactly why this generic
        # probe exists (kept, not thrown away once the bug was found) --
        # so the *mechanism itself* is falsified on QEMU before any driver
        # built on top of it ever risks a hardware hang again.
        ok, log = session.send_and_expect("chanechotest\n", r"ECHO_OK|ECHO_MISMATCH", timeout=10.0)
        echo_ok = ok and "ECHO_OK" in log
        results.append(("U-mode chan_call() Round-Trip Into A Real U-mode Server (M5 Phase 2)",
                        echo_ok, log if not echo_ok else ""))

        # C2: two user programs resident at once.
        #
        # This was the headline limitation the README carried: a single image
        # slot, allocated once and reused, so exec ran one program to
        # completion before another could start. The blocker was concrete --
        # the Sv39 backend caches a page table per domain and nothing could
        # free a tree, so a domain per exec would leak several pages every
        # time. C2 added vmm_free_table() and mem_domain_destroy() on top of
        # it, and the loader now gives each program its own image, stack and
        # domain.
        #
        # The assertion is residency overlap: uhello's task must exist while
        # uspin's is still alive, proving the loader gave both a slot at
        # once -- with the old single slot the second spawn was refused
        # outright.
        #
        # This used to check marker *print* order (USPIN_START ...
        # UPROG_TEXT_OK ... USPIN_PREEMPTED), on the assumption that spawning
        # uspin first meant it would always be the one to print first too.
        # That held under the old scheduler by accident of table-layout
        # proximity, not by any real guarantee, and a correctly fair
        # scheduler doesn't owe it: two freshly-created, identically-tied
        # tasks racing for the very first CPU turn is legitimately
        # non-deterministic (kernel/sched.c's next_runnable()), and uhello
        # winning that race (then finishing before uspin ever gets a turn)
        # is just as valid a proof of concurrent residency as the reverse --
        # both tasks plainly existed together regardless of which one
        # happened to run, or print, first. Checking kernel-emitted
        # "Created"/"exited" log lines instead of the programs' own racing
        # print statements is what makes this order-agnostic: those lines
        # are strictly chronological, whichever task the CPU favors.
        #
        # (spawn) rather than exec: exec waits for its program to die, so a
        # caller using it can only ever have one resident no matter how many
        # slots the loader has.
        ok, log = session.send_and_expect(
            'lisp\n(spawn "/flash0/system/bin/uspin.elf")\n'
            '(spawn "/flash0/system/bin/uhello.elf")\nexit',
            r"(?=.*USPIN_PREEMPTED)(?=.*UPROG_FILE_OK)", timeout=30.0)
        if ok:
            created = re.findall(r"\[Sched\] Created task #(\d+) 'uprog'", log)
            if len(created) < 2:
                ok = False
            else:
                uspin_pid, uhello_pid = created[0], created[1]
                uhello_created_pos = log.index(f"Created task #{uhello_pid} 'uprog'")
                uspin_exit_match = re.search(rf"\[Sched\] Task #{uspin_pid} 'uprog' exited", log)
                ok = bool(uspin_exit_match) and uhello_created_pos < uspin_exit_match.start()
        results.append(("Two User Programs Are Resident At Once (C2)", ok, log if not ok else ""))

        # C4: a user program larger than two pages.
        #
        # Under the model this replaces, ubig would not have *linked*:
        # linker/user.ld asserted that .data + .bss fit in one page, because
        # the loader could only ever allocate two. The image is now sized from
        # the program headers and rounded up to a power-of-two page run.
        #
        # The program writes across every page of its 20 KB .bss and reads it
        # back, which is what makes this a test of the *grants* rather than of
        # the allocation: a .bss that is merely declared would run identically
        # whether or not the domain covered it.
        ok, log = session.send_and_expect(
            "ubig", r"UBIG_WROTE 5.*UBIG_READBACK.*UBIG_DONE", timeout=25.0)
        results.append(("A User Program Larger Than Two Pages Runs (C4)", ok, log if not ok else ""))

        # ...and the padding that costs is visible rather than inferred.
        # ubig spans six pages and is given eight, because a PMP region must be
        # a power of two. On a 40-page heap that difference is worth being able
        # to see: it is what distinguishes "the heap is full" from "the heap is
        # full of padding".
        ok, log = session.send_and_expect(
            'lisp\n(spawn "/sd0/system/bin/ubig.elf")\nexit\ncat /proc/meminfo',
            r"User Images: 8 pages, 6 spanned \(2 padding\)", timeout=25.0)
        results.append(("NAPOT Rounding Loss Is Reported (C4)", ok, log if not ok else ""))

        # C4: W^X still holds, and now for a reason that could break.
        #
        # The loader used to grant page 0 as R|X and page 1 as R|W *by
        # position* -- it knew the layout because the linker script asserted
        # it. Permissions now come from each segment's ELF p_flags, so "the
        # text page is not writable" went from being an assumption to being a
        # thing the loader has to read correctly. A loader that mixed the flags
        # up, or granted a NAPOT piece with the wrong permissions, would pass
        # every other test here.
        #
        # The program prints UWX_NOT_ENFORCED itself if the store succeeds, so
        # a failure is a loud line rather than a missing one.
        ok, log = session.send_and_expect(
            "uwx", r"UWX_ALIVE.*terminated before it could exit", timeout=25.0)
        if ok and "UWX_NOT_ENFORCED" in log:
            ok = False
        results.append(("W^X Is Enforced From Segment Flags (C4)", ok, log if not ok else ""))

        # C6/C7: cc and the editor cost nothing while idle.
        #
        # Their pools used to be static arrays -- about 108 KB for chibicc and
        # 44 KB for ed, a third of an RP2350's SRAM reserved for two commands
        # that are almost never running. They are now taken from the heap on
        # entry and returned on exit.
        #
        # The assertion is that the page count *returns*, not merely that cc
        # works: a compile that acquired and never released would still produce
        # a correct binary and would leak an arena every time. Two compiles
        # rather than one, so a leak of even a single arena is visible.
        _, log_before = session.send_and_expect("cat /proc/meminfo", r"Storage:", timeout=5.0)
        m_before = re.search(r"Pages Used: (\d+)", log_before)
        session.send_and_expect("cc /sd0/hello.c /ram0/hd1.elf", r"Build clean", timeout=20.0)
        session.send_and_expect("cc /sd0/hello.c /ram0/hd2.elf", r"Build clean", timeout=20.0)
        _, log_after = session.send_and_expect("cat /proc/meminfo", r"Storage:", timeout=5.0)
        m_after = re.search(r"Pages Used: (\d+)", log_after)

        cc_before = int(m_before.group(1)) if m_before else None
        cc_after = int(m_after.group(1)) if m_after else None
        ok = (cc_before is not None and cc_before == cc_after)
        results.append((
            "Compiler Arena Is Returned After Each Compile (C6)",
            ok,
            f"pages used before={cc_before} after={cc_after} "
            f"(two compiles in between; a climb means an arena leaked)" if not ok else "",
        ))

        # ...and the binary it produced still runs, so the arena being freed
        # did not take the output with it.
        ok, log = session.send_and_expect("exec /ram0/hd2.elf", r"returned", timeout=20.0)
        results.append(("A Heap-Compiled Binary Still Runs (C6)", ok, log if not ok else ""))

        # The editor's buffers likewise. Entering and leaving it must leave the
        # heap where it was.
        _, log_before = session.send_and_expect("cat /proc/meminfo", r"Storage:", timeout=5.0)
        m_before = re.search(r"Pages Used: (\d+)", log_before)
        session.send_and_expect("e\nabc\x18\x03y\n", r"lsh>", timeout=8.0)
        _, log_after = session.send_and_expect("cat /proc/meminfo", r"Storage:", timeout=5.0)
        m_after = re.search(r"Pages Used: (\d+)", log_after)
        ed_before = int(m_before.group(1)) if m_before else None
        ed_after = int(m_after.group(1)) if m_after else None
        ok = (ed_before is not None and ed_before == ed_after)
        results.append((
            "Editor Buffers Are Returned On Exit (C7)",
            ok,
            f"pages used before={ed_before} after={ed_after}" if not ok else "",
        ))

        # C8: one wire, several protocols, exactly one owner.
        #
        # The UART is registered three times -- `uart` as a console,
        # `uartslip` as dedicated SLIP-framed 9P, `uartdemux` as both
        # demultiplexed. That was already true; what was missing is that
        # nothing recorded they were the same piece of hardware, so binding the
        # console to `uart` while something drove `uartslip` gave two owners the
        # same registers. It was masked only because p9serve never returns.
        #
        # Asserted as a *cycle*, in both directions, because a check that only
        # ever refuses would pass for an implementation that refuses
        # everything:
        #   1. uartslip is refused while the console holds the wire
        #   2. releasing the console frees it
        #   3. uartslip now succeeds
        #   4. and the console is refused in turn
        # Step 4 is what proves the rule is about the wire rather than about
        # one privileged name.
        ok, log = session.send_and_expect(
            'lisp\n(bind "uartslip")\n(release "uart")\n(bind "uartslip")\n(bind "uart")\nexit',
            r"=> #f.*=> #t.*=> #t.*=> #f", timeout=8.0)
        if ok and "already holds it" not in log:
            ok = False  # refused, but without saying what it collided with
        results.append(("One Wire Has One Owner, In Both Directions (C8)",
                        ok, log if not ok else ""))

        # Put the console's wire back, so later tests are not running against a
        # UART owned by a 9P link.
        session.send_and_expect('lisp\n(release "uartslip")\n(bind "uart")\nexit',
                                r"=> #t", timeout=6.0)

        # /proc/ports says what each name is and which wire it drives, which is
        # what makes a refusal legible rather than mysterious.
        ok, log = session.send_and_expect(
            "cat /proc/ports", r"uart\s+console\s+uart0.*uartslip\s+p9link\s+uart0",
            timeout=5.0)
        results.append(("/proc/ports Reports Wires And Their Holders (C8)",
                        ok, log if not ok else ""))

        # C3: a program receives its own arguments.
        #
        # Typed with arguments at the shell, so this exercises the whole path:
        # the shell splits the line, the loader copies the strings into the
        # program's own stack page and hands it a pointer to a vector there,
        # and the program dereferences that vector. argv[0] is the name as
        # typed -- the loader deliberately does not invent one.
        #
        # The program reads through the pointers rather than just counting
        # them, which is what makes this a test of the *addresses* being valid
        # in U-mode: a vector built at a kernel address would fault here rather
        # than silently appearing to work.
        ok, log = session.send_and_expect(
            "uargs alpha beta gamma",
            r"UARGS_COUNT 4.*UARGS_ARG 0:uargs.*UARGS_ARG 3:gamma"
            r".*UARGS_NULL_OK.*UARGS_DONE",
            timeout=25.0)
        results.append(("A User Program Receives argc/argv (C3)", ok, log if not ok else ""))

        # C3: the exit status reaches the shell, and /proc/ps tells a clean
        # exit apart from a kill.
        #
        # Both halves matter. A status alone cannot distinguish them -- 0 is an
        # ordinary return value and also what a killed task leaves behind --
        # so /proc/ps reports "killed" rather than a number for a task the
        # fault handler ended. uargs returns 42 precisely because it is
        # neither 0 nor the 7 uhello returns, so a passing result cannot come
        # from a stale or defaulted field.
        ok, log = session.send_and_expect("uargs\nps", r"uprog\s+42", timeout=25.0)
        results.append(("Exit Status Reaches The Shell And /proc/ps (C3)", ok, log if not ok else ""))

        ok, log = session.send_and_expect(
            "exec /flash0/system/bin/uisolate.elf\nps", r"uprog\s+killed", timeout=25.0)
        results.append(("A Killed Program Is Not Reported As A Clean Exit (C3)",
                        ok, log if not ok else ""))

        # C3: a user program reaches a *service*, and the boundary holds.
        #
        # This is the capability the phase exists to establish: before
        # SYS_CHAN_CALL a U-mode program could print and touch files and
        # nothing else, so moving a kernel subsystem into a process had no way
        # for anything to reach the result.
        #
        # Four claims in one program, each with its own marker:
        #   UCHAN_VIA_SERVICE -- emitted by the *console server*, not by the
        #                        program's own SYS_PRINT, so it proves the
        #                        message crossed the channel
        #   UCHAN_NOSUCH_OK   -- an unregistered name is refused; without this
        #                        the first check would pass for a kernel that
        #                        accepted every name and did nothing
        #   UCHAN_FOREIGN_OK  -- the confused-deputy case at the channel
        #                        boundary: a request buffer the program does
        #                        not own is refused, not copied from
        #   UCHAN_RETIRED_OK  -- syscall 1 is gone rather than repurposed, so a
        #                        binary built against the old register-IPC ABI
        #                        gets a refusal instead of some later syscall
        ok, log = session.send_and_expect(
            "uchan",
            r"UCHAN_VIA_SERVICE.*UCHAN_NOSUCH_OK.*UCHAN_FOREIGN_OK"
            r".*UCHAN_RETIRED_OK.*UCHAN_DONE",
            timeout=25.0)
        results.append(("U-mode Reaches A Service Over A Channel (C3)", ok, log if not ok else ""))

        # ...and everything they took comes back.
        #
        # Parses actual page counts either side of several loads rather than
        # matching text, because this specifically catches a *quantity*
        # regression: with vmm_free_table() stubbed out, each exec on the MMU
        # build strands a page-table tree and the count climbs monotonically.
        # Five loads make a per-load leak of even one page unmistakable.
        #
        # Reaping happens at the start of a load, so the second reading is
        # taken after a further exec -- otherwise the last program's pages are
        # legitimately still held and the test would fail on correct behaviour.
        session.send_and_expect("exec /flash0/system/bin/uhello.elf", r"returned 7", timeout=25.0)
        _, log_before = session.send_and_expect("cat /proc/meminfo", r"Storage:", timeout=5.0)
        m_before = re.search(r"Pages Used: (\d+)", log_before)
        for _ in range(5):
            session.send_and_expect("exec /flash0/system/bin/uhello.elf", r"returned 7", timeout=25.0)
        _, log_after = session.send_and_expect("cat /proc/meminfo", r"Storage:", timeout=5.0)
        m_after = re.search(r"Pages Used: (\d+)", log_after)

        used_before = int(m_before.group(1)) if m_before else None
        used_after = int(m_after.group(1)) if m_after else None
        ok = (used_before is not None and used_before == used_after)
        results.append((
            "User Program Pages And Page Tables Are Reclaimed (C2)",
            ok,
            f"pages used before={used_before} after={used_after} "
            f"(five loads in between; a climb means a per-load leak)" if not ok else "",
        ))

        # B6: the timer preempts a task that never yields.
        #
        # This cannot pass under cooperative scheduling, which is the point:
        # the spinner task never yields and the waiting loop never yields, so
        # whichever runs first would run to completion and the flag would stay
        # clear forever. Observing it set means a timer interrupt switched
        # tasks at an arbitrary instruction.
        #
        # taskdemo's interleaving test does NOT already cover this -- its tasks
        # yield explicitly, so it passes with or without a timer.
        ok, log = session.send_and_expect(
            "preempttest", r"PREEMPTED \(a task ran without anyone yielding\)", timeout=30.0)
        results.append(("Timer Preempts A Task That Never Yields (B6)", ok, log if not ok else ""))

        # M3 (plan/phase12_microkernel_migration.md): next_runnable() picks by
        # priority tier, not ring position. priotest creates four
        # never-yielding NORMAL-tier hogs (each long enough to still be READY,
        # not already exited, at the moment the check below fires -- that's
        # what makes this test actually exercise the tie-break rather than
        # pass by the hogs having gotten out of the way on their own),
        # task_blocks() a TASK_PRIO_INTERRUPT "urgent" task, then unblocks it
        # and measures how many ticks pass before it actually runs. Bounded to
        # <=2 ticks: under the plain round-robin this replaced, wake-to-run
        # latency would scale with how many equal-priority tasks sit ahead of
        # urgent in the ring, not stay flat regardless of them.
        ok, log = session.send_and_expect(
            "priotest", r"ran=1 ticks_to_run=(\d+) -- BOUNDED \(priority won\)", timeout=30.0)
        results.append(("A Higher-Priority Task Wins The Next Reschedule Over Hogs (M3)",
                        ok, log if not ok else ""))

        # M4.5 Part A (plan/phase12_microkernel_migration.md): priotest above
        # only ever proves one TASK_PRIO_INTERRUPT task against a field of
        # NORMAL-tier hogs -- it says nothing about what happens when *two*
        # INTERRUPT-tier tasks are both genuinely busy at once, which M4.5's
        # driver-task conversions are about to make a real situation (more
        # than one driver task plausibly wants this tier). priostress creates
        # two same-tier, equally-sized, never-yielding tasks and checks they
        # finish within a small factor of each other -- fair sharing -- rather
        # than one running to completion while the other is starved until it,
        # which would show up as roughly a 2x gap between their finish ticks
        # instead of a near-zero one. Measured on QEMU: both finish at the
        # same tick, or within 1-2 ticks, every time on both rv64 and rv32.
        ok, log = session.send_and_expect(
            "priostress", r"done=1,1 total_ticks=(\d+),(\d+) -- FAIR", timeout=30.0)
        results.append(("Two Same-Tier Interrupt Tasks Share The CPU Fairly (M4.5)",
                        ok, log if not ok else ""))

        # B4/D4: the 9P server -- which is also the filesystem server, since
        # its handlers are VFS calls -- is a scheduled task rather than
        # something pumped from the console's busy-wait. Before this a node
        # answered its peers only while sitting at the prompt.
        ok, log = session.send_and_expect("cat /proc/ps", r"\d+\s+\w+\s+p9srv", timeout=3.0)
        results.append(("9P/Filesystem Server Runs As A Task (B4, D4)", ok, log if not ok else ""))

        # M4 (plan/phase12_microkernel_migration.md): the uart driver runs as
        # a task, reachable only via chan_call(), and console output batches
        # into whole-message writes rather than one chan_call() per
        # character. The first version of this milestone got the second half
        # wrong -- routed every byte through its own chan_call(), which
        # manufactured scheduling frequency next_runnable() had never been
        # exercised at and triggered a day of scheduler redesigns chasing a
        # problem that was never in the scheduler. Reverted, reformulated,
        # and rebuilt around batching (see that document's M4 section for
        # the fuller account); this is the regression test the reformulation
        # promised: `help` prints on the order of 50 lines and ~2000
        # characters, so a per-character design would cost on that order of
        # chan_call()s too. A generous bound (200) still leaves a wide margin
        # below "character-scale" while comfortably clearing the ~60 calls
        # actually measured, so a real regression back toward per-character
        # batching trips this long before the margin gets tight.
        ok, log = session.send_and_expect("uartstats", r"write_calls=(\d+)", timeout=3.0)
        before_calls = int(re.search(r"write_calls=(\d+)", log).group(1)) if ok else None
        ok2, log2 = session.send_and_expect("help", r"reboot", timeout=5.0)
        ok3, log3 = session.send_and_expect("uartstats", r"write_calls=(\d+)", timeout=3.0)
        after_calls = int(re.search(r"write_calls=(\d+)", log3).group(1)) if ok3 else None
        batched = (before_calls is not None and after_calls is not None
                  and 0 < (after_calls - before_calls) < 200)
        results.append(("Console Output Is Batched, Not Per-Character (M4)",
                        ok and ok2 and ok3 and batched,
                        "" if (ok and ok2 and ok3 and batched) else
                        f"before={before_calls} after={after_calls}\n{log2[-300:]}"))

        # M4.5 Part B (plan/phase12_microkernel_migration.md): the SD/block
        # storage driver ("blk" on QEMU, "sdblk" on real RP2350 hardware) as
        # a task, reachable only via chan_call(). Unlike uart, this needed
        # no batching redesign -- a read_blocks()/write_blocks() call was
        # already exactly one message's worth of work -- so the check here
        # is simpler than the console one above: not "did IPC volume stay
        # low" but "is the task actually being used at all", i.e. a real,
        # growing call count rather than every caller silently falling back
        # to direct hardware access the whole time (which would still pass
        # every functional test, just prove nothing about this milestone).
        ok, log = session.send_and_expect("blkstats", r"calls=(\d+)", timeout=3.0)
        before_blk = int(re.search(r"calls=(\d+)", log).group(1)) if ok else None
        ok2, log2 = session.send_and_expect(
            "cat /flash0/system/etc/init.lisp", r"\(lsh\)\)\)", timeout=5.0)
        ok3, log3 = session.send_and_expect("blkstats", r"calls=(\d+)", timeout=3.0)
        after_blk = int(re.search(r"calls=(\d+)", log3).group(1)) if ok3 else None
        blk_used = (before_blk is not None and after_blk is not None
                   and after_blk > before_blk)
        results.append(("SD/Block Storage Driver Task Is Actually Serving Requests (M4.5)",
                        ok and ok2 and ok3 and blk_used,
                        "" if (ok and ok2 and ok3 and blk_used) else
                        f"before={before_blk} after={after_blk}\n{log2[-300:]}"))

        # M4.5 Part B: the shared "i2c" driver task (RTC + EEPROM, one task
        # since both sit on the same physical bus -- drivers/i2c_rtc.h).
        # Same "is it actually serving requests" shape as blkstats above;
        # the EEPROM read/write itself is already covered functionally by
        # the AT24C32 test below (which now transparently exercises this
        # same task, since at24c32_read()/write() kept their original names).
        ok, log = session.send_and_expect("i2cstats", r"calls=(\d+)", timeout=3.0)
        before_i2c = int(re.search(r"calls=(\d+)", log).group(1)) if ok else None
        ok2, log2 = session.send_and_expect(
            "(eeprom-write 100 \"i2c_task_probe\")", r".", timeout=4.0)
        ok3, log3 = session.send_and_expect("i2cstats", r"calls=(\d+)", timeout=3.0)
        after_i2c = int(re.search(r"calls=(\d+)", log3).group(1)) if ok3 else None
        i2c_used = (before_i2c is not None and after_i2c is not None
                   and after_i2c > before_i2c)
        results.append(("RTC/EEPROM Shared I2C Driver Task Is Actually Serving Requests (M4.5)",
                        ok and ok2 and ok3 and i2c_used,
                        "" if (ok and ok2 and ok3 and i2c_used) else
                        f"before={before_i2c} after={after_i2c}\n{log2[-300:]}"))

        # 3. VFS & Storage Engine (mkdir, rmdir, cp, touch, write, cat, rm)
        cmd_vfs = (
            "mkdir /sd0/testdir\n"
            "write /sd0/testdir/a.txt Hello_LugalOS_VFS\n"
            "cp /sd0/testdir/a.txt /sd0/testdir/b.txt\n"
            "cat /sd0/testdir/b.txt"
        )
        ok, log = session.send_and_expect(cmd_vfs, r"Hello_LugalOS_VFS", timeout=4.0)
        results.append(("VFS mkdir, write, cp, cat", ok, log if not ok else ""))

        # Test non-empty rmdir failure & cleanup
        cmd_rmdir = (
            "rmdir /sd0/testdir\n"
            "rm /sd0/testdir/a.txt\n"
            "rm /sd0/testdir/b.txt\n"
            "rmdir /sd0/testdir"
        )
        ok, log = session.send_and_expect(cmd_rmdir, r"=> #t", timeout=4.0)
        results.append(("VFS rmdir & rm File Cleanup", ok, log if not ok else ""))


        # 4. Extended Unix ed Editor
        cmd_ed = (
            "ed /sd0/test_ed.txt\n"
            "a\n"
            "line1_first\n"
            "line2_second\n"
            ".\n"
            "1,$n\n"
            ",s/first/updated/\n"
            "1,$n\n"
            "w\n"
            "q\n"
            "cat /sd0/test_ed.txt"
        )
        ok, log = session.send_and_expect(cmd_ed, r"line1_updated", timeout=4.0)
        results.append(("Unix ed Line Editor", ok, log if not ok else ""))

        # 5. chibicc C11 Compiler & Execution
        cmd_cc = "cc /sd0/hello.c /sd0/hello.elf\nexec /sd0/hello.elf"
        # The compiled program's own output, not just the loader's "returned"
        # line: the previous pattern matched a status the loader prints whether
        # or not the program produced anything. Since B6 this runs in U-mode,
        # so the greeting is now also evidence that a syscall crossed the
        # privilege boundary and came back.
        ok, log = session.send_and_expect(
            cmd_cc, r"Hello from LugalOS.*returned 0", timeout=5.0)
        results.append(("chibicc C11 Compiler & Exec", ok, log if not ok else ""))

        # 5b. Filesystem Usage Metrics (df) & System Monitor (top)
        cmd_df = "cat /proc/df"
        ok, log = session.send_and_expect(cmd_df, r"Filesystem\s+512-blocks", timeout=4.0)
        results.append(("Filesystem Usage Metrics (df)", ok, log if not ok else ""))

        cmd_top = "cat /proc/ps"
        ok, log = session.send_and_expect(cmd_top, r"PID\s+State\s+Name", timeout=4.0)
        results.append(("System Process & Memory Monitor (top)", ok, log if not ok else ""))

        # System Time & Date Clock Verification
        cmd_time = "date\ndate 2026-08-05 16:00:00\ndate"
        ok, log = session.send_and_expect(cmd_time, r"2026-08-05 16:00", timeout=4.0)
        results.append(("System Time & Date Clock", ok, log if not ok else ""))

        # AT24C32 EEPROM (/dev/eeprom & eeprom-read/write)
        cmd_eeprom = (
            "write /dev/eeprom EEPROM_PERSIST_OK\n"
            "cat /dev/eeprom\n"
            "lisp\n"
            "(eeprom-write 0 \"LISP_EEPROM_OK\")\n"
            "(eeprom-read 0 14)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_eeprom, r"LISP_EEPROM_OK", timeout=4.0)
        results.append(("AT24C32 EEPROM (/dev/eeprom & eeprom-read/write)", ok, log if not ok else ""))

        # 9P Protocol Serialization & Loopback Transport (Phase 1)
        cmd_9p_syntax = "lisp\n9p\nexit"
        ok, log = session.send_and_expect(cmd_9p_syntax, r"Invalid identifier starting with digit: '9p'", timeout=4.0)
        results.append(("Strict Scheme Syntax Parser (Digit-Prefixed Symbol Rejection)", ok, log if not ok else ""))

        cmd_9p_rpc = (
            "write /srv/p9_loopback P9_VFS_WRITE_OK\n"
            "cat /srv/p9_loopback\n"
            "lisp\n"
            "(p9-loopback \"P9_LISP_RPC_PASSED\")\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_9p_rpc, r"=> \"P9_LISP_RPC_PASSED\"", timeout=4.0)
        results.append(("9P2000 Protocol & p9-loopback Primitive Execution (Phase 1)", ok, log if not ok else ""))

        # SLIP RFC 1055 UART Network Transport (Phase 2)
        cmd_uart_9p = (
            "write /srv/uart_9p SLIP_UART_WRITE_OK\n"
            "cat /srv/uart_9p\n"
            "lisp\n"
            "(p9-uart-send \"P9_SLIP_UART_PASSED\")\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_uart_9p, r"=> \"P9_SLIP_UART_PASSED\"", timeout=4.0)
        results.append(("SLIP RFC 1055 UART Transport & p9-uart-send (Phase 2)", ok, log if not ok else ""))

        # 9P server wired to the real VFS handle API, not a single global
        # echo buffer (A2): p9-cat drives a full Tattach("/") + multi-
        # component Twalk + Topen + Tread + Tclunk session against a real,
        # pre-existing file that 9P itself never wrote -- proving the fid
        # table actually resolves arbitrary namespace paths through the VFS.
        cmd_9p_vfs_wiring = (
            "lisp\n"
            "(p9-cat \"/sd0/system/etc/init.lisp\")\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_9p_vfs_wiring, r"LugalOS System Initialization Script", timeout=4.0)
        results.append(("9P Server Wired to Real VFS Files via p9-cat (A2)", ok, log if not ok else ""))


        # 6. Lugal-Lisp REPL Core Engine & Arithmetic (+, -, *, =)
        cmd_arith = "lisp\n(+ 10 20 30)\n(- 100 40)\n(* 6 7)\n(= 42 42)\nexit"
        ok, log = session.send_and_expect(cmd_arith, r"42", timeout=4.0)
        results.append(("Lisp Arithmetic Primitives (+, -, *, =)", ok, log if not ok else ""))

        # 7. Lisp Special Forms (define, lambda, quote, ', if, begin, let, cond)
        cmd_forms = (
            "lisp\n"
            "(define double (lambda (n) (* n 2)))\n"
            "(double 21)\n"
            "(quote (1 2 3))\n"
            "'symbol_test\n"
            "(if (= 1 1) 'if_ok 'if_fail)\n"
            "(begin 1 2 'begin_ok)\n"
            "(let ((x 15) (y 25)) (+ x y))\n"
            "(cond ((= 1 0) 'cond_fail) (else 'cond_ok))\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_forms, r"cond_ok", timeout=5.0)
        results.append(("Lisp Special Forms (define, lambda, quote, if, begin, let, cond)", ok, log if not ok else ""))

        # 8. Lisp File I/O Primitives (write-file, read-file, load, display, newline)
        cmd_lisp_io = (
            "lisp\n"
            "(write-file \"/sd0/lisp_io.txt\" \"Lisp_File_IO_Passed\")\n"
            "(read-file \"/sd0/lisp_io.txt\")\n"
            "(display \"Lisp_Display_Msg\\n\")\n"
            "(load \"/sd0/system/etc/stdlib.lisp\")\n"
            "(not #f)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_lisp_io, r"=> #t", timeout=5.0)
        results.append(("Lisp File I/O & Load Primitives (write-file, read-file, load)", ok, log if not ok else ""))

        # 9. Lisp Microkernel VFS & Process Operations (mkdir, touch, write, cp, cat, rm, rmdir, ps, meminfo, version)
        cmd_lisp_vfs = (
            "(mkdir \"/sd0/lisp_vfs\")\n"
            "(write \"/sd0/lisp_vfs/a.txt\" \"VFS_Lisp_Data\")\n"
            "(cp \"/sd0/lisp_vfs/a.txt\" \"/sd0/lisp_vfs/b.txt\")\n"
            "(cat \"/sd0/lisp_vfs/b.txt\")\n"
            "(rm \"/sd0/lisp_vfs/a.txt\")\n"
            "(rm \"/sd0/lisp_vfs/b.txt\")\n"
            "(rmdir \"/sd0/lisp_vfs\")\n"
            "(ps)\n"
            "(meminfo)\n"
            "(version)"
        )
        # Asserts on real /proc content, so (ps)/(meminfo)/(version) are
        # exercised as genuine byte streams read through the VFS handle API,
        # not the old printk-side-effect path. The marker was "kernel_idle"
        # until B2 replaced /proc/ps's hardcoded string with the real task
        # table; "Pages Total" from /proc/meminfo is the equivalent today --
        # it likewise only appears in real generated content (and is itself
        # B2 output: the live page allocator's counters).
        ok, log = session.send_and_expect(cmd_lisp_vfs, r"Pages Total", timeout=5.0)
        results.append(("Lisp Microkernel VFS Primitives (mkdir, write, cp, cat, rm, ps, meminfo)", ok, log if not ok else ""))


        # 10. Lisp Compiler & Native Binary Primitives (cc, exec)
        cmd_lisp_cc = (
            "(cc \"/sd0/hello.c\" \"/sd0/hello_lisp.elf\")\n"
            "(exec \"/sd0/hello_lisp.elf\")"
        )
        ok, log = session.send_and_expect(
            cmd_lisp_cc, r"Hello from LugalOS.*returned 0", timeout=5.0)
        results.append(("Lisp Compiler & Binary Exec Primitives (cc, exec)", ok, log if not ok else ""))

        # 10b. Chess console REPL (J1, plan/phase10_chess_completion.md).
        # `(chess)` on a QEMU target (no display/TM1638) dispatches to
        # chess_console_run() -- the first automated test to exercise chess
        # beyond a single fixed chess-selftest search (phase9 H4 added zero
        # QEMU coverage for anything else). `level 1` first keeps `e2e4`'s
        # auto engine-reply search (the only one this sequence triggers) to
        # a 1-second budget rather than the 2-second default. Exercises the
        # full command surface in one pass: new/level/a move (which triggers
        # the engine reply)/board/eval/moves/undo/redo/fen/save/load/quit.
        cmd_chess = (
            "(chess)\n"
            "new\n"
            "level 1\n"
            "e2e4\n"
            "board\n"
            "eval\n"
            "moves\n"
            "undo\n"
            "redo\n"
            "fen\n"
            "save\n"
            "load\n"
            "quit"
        )
        ok, log = session.send_and_expect(cmd_chess, r"Position loaded from", timeout=15.0)
        results.append(("Chess Console REPL: new/move+engine-reply/board/eval/moves/undo/redo/fen/save/load (J1)", ok, log if not ok else ""))

        # 10c. Chess heap is fully released on `quit`. Revises J0's original
        # "never freed for the process lifetime" choice -- correct when J0
        # made it (no chess entry point had a session boundary to free at
        # yet), stale once J1 gave chess_console_run() one. Reads
        # /proc/meminfo before touching chess at all, then again after a
        # full session (a real engine-reply search included, so the
        # move-list pools and TT both actually get touched) ending in
        # `quit`, and asserts "Pages Used" is identical -- not just "didn't
        # grow further" but genuinely returned, the same shape as C6/C7's
        # own cc/ed check in tests/hw/test_rp2350.py. Two separate
        # send_and_expect calls, not one combined string: "Pages Used"
        # appears in both readings, and a single regex would match (and
        # return) on the first one before chess ever ran.
        ok0, log0 = session.send_and_expect("cat /proc/meminfo", r"Pages Used: \d+", timeout=4.0)
        before = re.search(r"Pages Used: (\d+)", log0)
        cmd_release = "(chess)\nlevel 1\ne2e4\nquit\ncat /proc/meminfo"
        ok1, log1 = session.send_and_expect(cmd_release, r"Pages Used: \d+", timeout=8.0)
        after = re.search(r"Pages Used: (\d+)", log1)
        heap_ok = (ok0 and ok1 and before is not None and after is not None
                   and before.group(1) == after.group(1))
        results.append((
            "Chess Heap Is Fully Released On quit (revises J0's never-freed choice)",
            heap_ok,
            "" if heap_ok else
            f"before={before.group(1) if before else '?'} "
            f"after={after.group(1) if after else '?'}\n{log1}"))

        # 10d. Chess game-outcome detection (J2, plan/phase10_chess_completion
        # .md): checkmate and stalemate, the "unfinished integration" the
        # phase's own original proposal named. Fool's mate reached directly
        # via FEN (1.f3 e5 2.g4, Black to move) rather than playing it out
        # move by move -- cheap and deterministic, no search needed for the
        # *detection* side. Qh4# should end the game immediately, before any
        # engine reply is attempted.
        #
        # Each sub-test is two send_and_expect() calls, not one combined
        # string ending in "quit" -- found live (the hard way: it hung the
        # rest of the suite) that matching on the outcome message returns
        # before `quit`, sent in the same write right after it, has actually
        # been read and processed by the guest. The *next* test's own first
        # write can then arrive while the guest is still mid-transition out
        # of the previous chess session, landing at an unpredictable point
        # relative to whatever that test assumed was a clean chess> prompt.
        # Waiting explicitly for "lsh>" after `quit` closes that gap.
        cmd_checkmate = (
            "(chess)\n"
            "fen rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2\n"
            "d8h4"
        )
        ok, log = session.send_and_expect(cmd_checkmate, r"Checkmate! Black wins!", timeout=6.0)
        results.append(("Chess Checkmate Detection (fool's mate, J2)", ok, log if not ok else ""))
        session.send_and_expect("quit", r"lsh>", timeout=4.0)

        # A textbook stalemate (Black king a8, White queen b6 covers a7/b7/b8
        # without checking a8 itself) loaded directly via FEN, confirmed via
        # `moves` (0 legal moves) and then `go`, which must report the
        # outcome instead of attempting a search with no legal replies.
        cmd_stalemate = (
            "(chess)\n"
            "fen k7/8/1Q6/8/8/8/8/7K b - - 0 1\n"
            "go"
        )
        ok, log = session.send_and_expect(cmd_stalemate, r"Stalemate! Game is a draw", timeout=6.0)
        results.append(("Chess Stalemate Detection (J2)", ok, log if not ok else ""))
        session.send_and_expect("quit", r"lsh>", timeout=4.0)

        # 10e. Chess perft move-generation correctness (J4, plan/phase10
        # _chess_completion.md). `(perft)` is a top-level Lisp primitive,
        # not a chess-console command -- no `(chess)`/`quit` wrapper needed,
        # same shape as the cc/exec test just above. Depth 3 across the
        # full 24-case table (start pos, kiwipete, and 22 tactical `pej-*`
        # positions covering castling/en-passant/promotion) is sub-second
        # even at QEMU host speed and exercises every legal-move-generation
        # edge case perft.c's own table was built for -- a correctness
        # check on move generation, not a performance benchmark, so this
        # doesn't need hardware coverage at all (per
        # [[falsify_on_hardware_not_qemu]], perft correctness doesn't
        # depend on any board-specific code path). Asserts "0 errors"
        # literally rather than capturing and comparing the count -- a
        # nonzero error count simply won't match, which is exactly the
        # failure this test exists to catch.
        ok, log = session.send_and_expect(
            "(perft 3)", r"PERFT Results: \d+ passed depths, 0 errors", timeout=15.0)
        results.append(("Chess Perft Move-Generation Correctness (depth 3, J4)", ok, log if not ok else ""))

        # Ctrl-C interrupting an unbounded (Level 8) search (J2) is
        # deliberately NOT an automated test here, after trying: it needs a
        # real wall-clock delay between starting an off-book search (a2a3 --
        # every common first move is an opening-book entry that returns
        # instantly, so only an off-book move forces a genuine iterative-
        # deepening search to interrupt) and sending the interrupt byte,
        # which makes it a real-time race against this specific test
        # session's own accumulated state and host-machine load rather than
        # a deterministic check. It hung the *entire* suite (every test
        # after it, on whichever architecture drew the bad timing) when the
        # 1.5s window landed wrong, even with a synchronized setup step and
        # a best-effort second-Ctrl-C cleanup attempt -- both tried and both
        # still observed to hang live. The mechanism itself is verified
        # thoroughly by hand instead, with a proper negative control: `(chess)
        # / level 8 / a2a3` alone left the search still mid-depth-10 with no
        # bestmove after 6 real seconds; the same sequence with a raw 0x03
        # sent 1.5s in reliably returned a bestmove within ~3s of the
        # interrupt, repeatedly, from a clean boot. Automating this
        # reliably would need the search itself to expose a way to block
        # until "in progress" rather than racing a sleep() against it --
        # a real follow-up, not attempted here.

        # 11. Phase 3: Persistent History Logging (/sd0/system/history.lisp)
        cmd_hist_check = "cat /sd0/system/history.lisp"
        ok, log = session.send_and_expect(cmd_hist_check, r"history", timeout=4.0)
        results.append(("Persistent Command History (/sd0/system/history.lisp)", ok, log if not ok else ""))

        # 12. Phase 3: Emacs-style Multiline Editor (Ctrl-X Ctrl-E -> Eval, e [filename])
        # \x18\x05 launches box editor, (+ 555 444) typed, \x18\x05 evaluates
        cmd_box = "\x18\x05(+ 555 444)\x18\x05"
        ok, log = session.send_and_expect(cmd_box, r"999", timeout=4.0)
        results.append(("Emacs Multiline Lisp Editor (Ctrl-X Ctrl-E)", ok, log if not ok else ""))

        # 13. Direct Shell Editor Command (`e /sd0/test_e.lisp`)
        cmd_e_file = "e /sd0/test_e.lisp\n(+ 777 222)\x18\x05"
        ok, log = session.send_and_expect(cmd_e_file, r"999", timeout=4.0)
        results.append(("Direct Shell Editor Command (e <filename>)", ok, log if not ok else ""))

        # 14. Emacs Quit Keybinding (`Ctrl-X Ctrl-C` with unsaved changes confirmation)
        cmd_e_quit = "e\nmodified\x18\x03y\n"
        ok, log = session.send_and_expect(cmd_e_quit, r"lsh>", timeout=4.0)
        results.append(("Emacs Quit Keybinding (Ctrl-X Ctrl-C with unsaved changes confirmation)", ok, log if not ok else ""))



        # 13. Filesystem Namespace Listing (ls /sd0/, /ram0/, /proc/, /dev/, /srv/)
        cmd_fs_ls = (
            "ls /sd0/\n"
            "ls /ram0/\n"
            "ls /proc/\n"
            "ls /dev/\n"
            "ls /srv/"
        )
        ok, log = session.send_and_expect(cmd_fs_ls, r"lisp", timeout=4.0)
        results.append(("Plan 9 Namespace Directory Listing (ls /sd0/, /ram0/, /proc/, /dev/, /srv/)", ok, log if not ok else ""))

        # 13b. B0 (plan/phase5_distributed_design.md §5.4): the kernel log ring
        # and its detachable output sinks. Before B0, printk() called
        # uart_putc() directly, so repurposing a UART (p9serve / a login shell)
        # silently destroyed all kernel log output. The claim under test is
        # that output produced while the console sink is detached is invisible
        # on the terminal yet fully retained in the ring for /proc/kmsg.
        ok, log = session.send_and_expect("klog", r"console: attached", timeout=3.0)
        results.append(("Kernel Log Sink Registry Lists Console Sink (B0)", ok, log if not ok else ""))

        # 13c. B0 device registry (kernel/device.h, kernel/board.c): what
        # hardware exists is a per-board table probed at boot, not a sequence
        # of #ifs inline in kernel_main(). /proc/devices exposes it, so it is
        # readable over 9P by another node like every other /proc file.
        # `vblk` present proves a probe that really ran (virtio-blk is what
        # /sd0 is mounted from); `uartslip p9link` proves a device registered
        # with no probe function is still reported present and typed.
        ok, log = session.send_and_expect(
            "cat /proc/devices",
            r"uartslip\s+p9link\s+present.*vblk\s+block\s+present",
            timeout=4.0)
        results.append(("Device Registry Enumerated Via /proc/devices (B0)", ok, log if not ok else ""))

        # 13d. B0 part 3: the registries are bindable from Lisp, so init.lisp
        # owns policy instead of it being compiled into kernel_main().
        # (dev-present? ...) is what lets a boot script branch on what the
        # board actually has rather than on which target it was built for.
        cmd_bind = (
            '(dev-present? "vblk")\n'
            '(dev-present? "no-such-device")'
        )
        ok, log = session.send_and_expect(cmd_bind, r"=> #t.*=> #f", timeout=4.0)
        results.append(("Lisp (dev-present?) Queries Device Registry (B0)", ok, log if not ok else ""))

        ok, log = session.send_and_expect("(klog-sinks)", r"console: attached", timeout=3.0)
        results.append(("Lisp (klog-sinks) Lists Log Sinks (B0)", ok, log if not ok else ""))

        # 13c-bis. B3 prep (D2): the PMP probe must complete without faulting.
        # It touches all 64 pmpaddr CSRs, most of which may not exist on a
        # given core, so the real assertion is the absence of a fault --
        # send_and_expect()'s FAULT_MARKERS check. RV32 runs in M-mode and
        # reports real numbers; RV64 runs in S-mode where PMP CSRs are
        # inaccessible, so both outcomes are accepted here. The measurement
        # that matters is on real Hazard3 silicon -- see tests/hw/.
        ok, log = session.send_and_expect(
            "pmpinfo", r"PMP: (writable=\d+|unavailable)", timeout=5.0)
        results.append(("PMP Probe Completes Without Faulting (B3 prep)", ok, log if not ok else ""))

        # 13c-ter. B3: the M->U (or S->U) transition and the trap path's
        # scratch-CSR stack swap.
        #
        # The assertion that matters is the trap CAUSE, not the output. A
        # kernel-mode task making the same ecalls prints byte-identical
        # output, so "UMODE_OK appeared" would pass whether or not the
        # privilege level ever dropped. The hardware sets cause 8 only for an
        # ecall taken FROM U-mode (9 = S-mode, 11 = M-mode), so that number is
        # evidence the kernel cannot fake.
        #
        # Reaching it also exercises the B3 stack swap end to end: each ecall
        # enters the kernel on the task's kernel stack rather than the user
        # stack it was running on.
        ok, log = session.send_and_expect(
            "usertest",
            r"UMODE_OK.*cause: 8 \(U-mode.*ended cleanly",
            timeout=10.0)
        results.append(("U-mode Task Runs And Syscalls Back (B3)", ok, log if not ok else ""))

        # B3+B5: per-task memory domains enforce, on BOTH memory models.
        #
        # This used to branch by target: RV32 asserted isolation while RV64
        # asserted that isolation was honestly reported as *unavailable*,
        # because D2 put enforcement on the M-mode targets first and Sv39 was
        # still B5. With B5 landed the two converge, and the branch is gone --
        # which is the clearest statement of Rule 0 this suite makes: one
        # description of a task's memory, enforced by PMP regions on one
        # target and Sv39 page tables on the other, asserted identically.
        ok, log = session.send_and_expect(
            "isolationtest",
            r"faulted: cause \d+.*ISOLATED \(kernel memory untouched\)",
            timeout=12.0)
        results.append(("U-mode Task Cannot Write Kernel Memory (B3/B5, both models)",
                        ok, log if not ok else ""))

        # 13c-quinquies. B3: the syscall boundary validates user pointers.
        #
        # The kernel runs where PMP does not restrict it, so a syscall that
        # dereferences a caller-supplied address lets a U-mode task reach
        # memory it cannot touch itself -- the restriction intact and wholly
        # bypassed. SYS_READ_FILE's `buf` is the vehicle: the kernel writes
        # into it, so it must be validated against the caller's own domain.
        #
        # BOTH halves are asserted. "Refused" alone would pass for a syscall
        # layer that rejects every pointer, which is useless rather than
        # secure, so the task must also succeed with a buffer it does own.
        #
        # Runs on both targets even though only RV32 enforces: the check reads
        # the domain's region list rather than the hardware, which is what
        # keeps copy-in/copy-out honest on a build whose mechanism (Sv39) is
        # still B5.
        ok, log = session.send_and_expect(
            "deputytest",
            r"DEPUTY_REFUSED.*OWNBUF_OK.*UNTOUCHED",
            timeout=12.0)
        results.append(("Syscall Boundary Rejects A Foreign Pointer (B3, copy-in/out)",
                        ok, log if not ok else ""))

        # 13c-sexies. B4: the console is a server, not a renamed printf.
        #
        # Two independent claims. First, it is reachable through a channel:
        # writing to /srv/console emits on the terminal, using the same
        # copy-always IPC as every other service -- which is what lets a task,
        # or a remote node over 9P, drive it without being the kernel.
        #
        # The marker is inside the typed command, which would normally make
        # this match its own echo (the trap that made the B4 stream test
        # vacuous). It does not here: send_and_expect() strips the first
        # occurrence of the command text before matching, so the only way a
        # match survives is if the endpoint independently emitted the marker.
        # Without the endpoint running, the reply is a bare "=> #t".
        ok, log = session.send_and_expect(
            "write /srv/console CONSOLE_VIA_CHANNEL",
            r"CONSOLE_VIA_CHANNEL", timeout=4.0)
        results.append(("Console Reachable As A Channel Service (B4)", ok, log if not ok else ""))

        # Second, ownership is bound by name from the device registry, so
        # init.lisp decides who owns the terminal. A name that does not
        # resolve must fail rather than silently leave the console bound to
        # whatever it had -- otherwise "bound" would mean nothing.
        ok, log = session.send_and_expect(
            'lisp\n(console-device)\n(console-bind "nosuchdev")\n(console-bind "uart")\nexit',
            r'=> "uart".*=> #f.*=> #t', timeout=6.0)
        results.append(("Console Device Bound By Name At Runtime (B4)", ok, log if not ok else ""))

        # 13d-bis. B2: the scheduler actually switches.
        # The assertion is *interleaving*, not that output appears: if
        # sched_yield() were still the pre-B2 no-op, each task would run to
        # completion first (A1 A2 A3 B1 B2 B3) and every marker would still be
        # printed. An early version of kernel/sched.c had exactly that bug -- a
        # "currently switching" flag that a freshly created task could never
        # clear, since it enters at the trampoline rather than returning from
        # ctx_switch() -- and an ordering check is what caught it.
        #
        # It asks for B1 before A3 rather than strict alternation
        # (A1 B1 A2 B2 A3 B3), which is what it used to require. Strict
        # alternation stopped being a property of a correct system when B6
        # added preemption: a timer tick can switch back to A after A yields
        # but before B has printed, giving A1 A2 B1 -- correct behaviour that
        # the old pattern called a failure. Measured at roughly one run in
        # eight on a loaded host, on this build *and* on the commit before it,
        # so it was the assertion that was wrong rather than anything it was
        # watching.
        #
        # B1 landing before A3 still fails for the no-op-yield bug this exists
        # to catch, which is the property worth keeping.
        ok, log = session.send_and_expect(
            "taskdemo",
            r"A1.*B1.*A3.*B3",
            timeout=8.0)
        results.append(("Tasks Interleave Rather Than Run To Completion (B2)", ok, log if not ok else ""))

        # Both task stacks must come back to the page allocator on exit. The
        # counts are printed by the demo itself; equality is the assertion.
        #
        # B6 changed HOW: task_exit() no longer frees the stack it is still
        # running on -- it hands it to a reaper that the next task runs. The
        # assertion is unchanged because the observable outcome should be, and
        # running the demo twice checks the reaper actually keeps up rather
        # than leaking one stack per exit.
        session.send_and_expect("taskdemo", r"Done\. Heap", timeout=8.0)
        ok, log = session.send_and_expect("taskdemo", r"free before=(\d+) after=\1", timeout=8.0)
        results.append(("Task Stacks Are Reclaimed On Exit, Via The Reaper (B2/B6)",
                        ok, log if not ok else ""))

        # M0 (plan/phase12_microkernel_migration.md): task_create_sized() lets
        # a caller ask for fewer pages than TASK_STACK_PAGES. Two things have
        # to both be true, and either failing alone would be a regression:
        # the requested size actually took effect (a 1-page stack, not the
        # default 2), and it was freed for exactly that many pages rather
        # than TASK_STACK_PAGES -- the latter is the more dangerous failure,
        # since it would either leak a page per run or free one page too many
        # into a neighboring allocation's territory.
        ok, log = session.send_and_expect("sizedtaskdemo", r"stack \w+, 4 KB", timeout=8.0)
        results.append(("task_create_sized() Honors A Non-Default Page Count (M0)",
                        ok, log if not ok else ""))
        ok, log = session.send_and_expect("sizedtaskdemo", r"free before=(\d+) after=\1", timeout=8.0)
        results.append(("A Non-Default-Size Stack Is Reclaimed For Its Real Page Count (M0)",
                        ok, log if not ok else ""))

        # M1 (plan/phase12_microkernel_migration.md): the buddy allocator.
        # First assertion is rounding *and* self-alignment together (Rule 6)
        # -- a 33/65/1025-byte request must come back as a 64/128/2048-byte
        # block whose address is itself a multiple of that size, the
        # property that makes a block usable as a PMP region later. Second
        # is the actual fragmentation claim the milestone exists to make: a
        # 50-round mixed-size churn across 8 concurrently-live slots must
        # still coalesce all the way back to exactly one arena-sized free
        # block, not merely "some free space" -- split/merge losing track of
        # a single block anywhere in the run would leave a permanent
        # fragment here instead.
        ok, log = session.send_and_expect("ballocdemo", r"Rounding/alignment: OK", timeout=8.0)
        results.append(("Buddy Blocks Round Up And Stay Self-Aligned (M1, Rule 6)",
                        ok, log if not ok else ""))
        ok, log = session.send_and_expect(
            "ballocdemo", r"largest=(\d+) arena=\1", timeout=8.0)
        results.append(("Buddy Allocator Fully Coalesces After Mixed-Size Churn (M1)",
                        ok, log if not ok else ""))

        # 13e. B1: this node's own namespace, mounted over a local copy-always
        # channel (kernel/chan.c) and reached through the *same* 9P client code
        # that talks to a peer over a wire. Nothing above the channel can tell
        # local from remote -- vfs_mount_local() is vfs_mount_remote() handed a
        # channel-backed link, with no separate local path anywhere below it.
        ok, log = session.send_and_expect(
            'lisp\n(mount-local "self")\nexit', r"=> #t", timeout=5.0)
        results.append(("Lisp (mount-local) Attaches Own Namespace Over A Channel (B1)",
                        ok, log if not ok else ""))

        # Reading through the mount: the bytes crossed serialized 9P frames and
        # two chan_call() copies to reach the same file /proc/version names.
        ok, log = session.send_and_expect(
            "cat /self/proc/version", rf"LugalOS v{expected_version()}", timeout=5.0)
        results.append(("9P Read Through Local Channel Mount (B1)", ok, log if not ok else ""))

        # The stronger claim: a *write* through the channel mount lands on the
        # real local filesystem. Confirmed by reading it back at its ordinary
        # local path, not through /self/ -- so a passing result cannot come
        # from the mount echoing its own state back.
        ok, log = session.send_and_expect(
            "write /self/ram0/viachan.txt HELLO_VIA_LOCAL_CHANNEL\ncat /ram0/viachan.txt",
            r"HELLO_VIA_LOCAL_CHANNEL", timeout=6.0)
        results.append(("9P Write Through Local Channel Lands On Real Local Disk (B1)",
                        ok, log if not ok else ""))

        # chan_call()'s re-entrancy guard: walking into the mount recursively
        # must fail cleanly rather than hang or corrupt the outer call's
        # single-slot request buffer. The FAULT_MARKERS check in
        # send_and_expect() also makes this a crash test.
        ok, log = session.send_and_expect(
            "cat /self/self/proc/version", r"cannot read path", timeout=6.0)
        results.append(("Recursive Local Mount Is Refused, Not Fatal (B1)", ok, log if not ok else ""))

        # NOTE: the "unknown link name is rejected" assertion deliberately does
        # NOT live here. On this single-node session no virtconsole is
        # attached, so a broken name lookup would fall back to a default link
        # that is also absent and still return #f -- passing for the wrong
        # reason. It runs in test_9p_remote_mount() instead, where the default
        # link genuinely exists.

        # B4: the two streams are now independent, which is the whole point.
        # Detaching the kernel-log sink must stop DIAGNOSTICS reaching the
        # terminal while the SHELL keeps working -- the §5.2 scenario this
        # track exists to deliver. Before B4 printk() carried both, so
        # detaching silenced everything, and this test asserted that weaker
        # (and undesirable) behaviour.
        # Stage the probe file BEFORE detaching, so the marker exists only in
        # the file's *content* and never in a command typed afterwards. The
        # line editor echoes keystrokes to the console, so a marker inside a
        # typed command would be matched from its own echo and the check would
        # pass no matter what -- which is exactly what happened when this was
        # first rewritten, and what B0's original comment had warned about.
        session.send_and_expect(
            "write /ram0/klogprobe.txt KLOG_RING_PROBE_MARKER", r"=> ", timeout=3.0)
        session.send_and_expect("klog detach console", r"\Z\A", timeout=1.0)

        # 1. User-facing output still reaches the terminal. The typed command
        #    does not contain the marker, so only cat's output can match.
        shell_ok, shell_log = session.send_and_expect(
            "cat /ram0/klogprobe.txt", r"KLOG_RING_PROBE_MARKER", timeout=4.0)

        # 2. Diagnostics do not. `taskdemo` emits several bracketed [Sched]
        #    lines through printk(); none may appear while the sink is off.
        _, diag_log = session.send_and_expect("taskdemo", r"\Z\A", timeout=3.0)
        diagnostics_silent = "[Sched]" not in diag_log and "[TaskDemo]" not in diag_log

        # 3. The ring kept them anyway, so nothing was lost.
        ok, log = session.send_and_expect(
            "klog attach console\ncat /proc/kmsg", r"\[TaskDemo\]", timeout=6.0)

        detail = ""
        if not shell_ok:
            detail = "shell output was suppressed along with the log:\n" + shell_log
        elif not diagnostics_silent:
            detail = "diagnostics still reached the terminal while detached:\n" + diag_log
        elif not ok:
            detail = "ring did not retain diagnostics emitted while detached:\n" + log
        results.append(("Log And Console Are Independent Streams (B4)",
                        shell_ok and diagnostics_silent and ok, detail))

        # C0: command output belongs to the console stream, not the log ring.
        #
        # The B4 test above proves the two streams *can* be separated. This one
        # proves specific commands are actually on the right side of the split
        # -- which several were not: `ls` (fs/fat32.c) and `i2c`
        # (drivers/i2c_rtc.c) still called printk(), so their output went into
        # the klog ring and turned up again in `cat /proc/kmsg`.
        #
        # Table-driven because this is a *class* of defect, not three
        # instances: every verb that answers a typed command is a candidate,
        # and adding a row is how the next one gets caught. Each verb is
        # checked in both directions, which is what makes it meaningful --
        # "appears on the console" alone would pass for output that goes to
        # both places, which is the bug.
        session.send_and_expect("klog detach console", r"\Z\A", timeout=1.0)
        stream_rows = [
            ("ls /flash0/", "Directory Listing (FAT32)"),
            ("i2c",         "I2C Bus Scan"),
        ]
        stream_fail = []
        for verb, marker in stream_rows:
            # 1. Still reaches the terminal with the log sink detached.
            on_console, _ = session.send_and_expect(verb, re.escape(marker), timeout=5.0)
            if not on_console:
                stream_fail.append(f"'{verb}' output vanished when the log sink was detached")

        # 2. And none of it landed in the ring. Read the log back once, with
        #    the sink still detached, so the only way a marker can appear is if
        #    the verb itself wrote it there. One read covers every row: a leak
        #    from any verb puts its marker in the same ring.
        _, ring = session.send_and_expect("cat /proc/kmsg", r"lsh>", timeout=6.0)
        for verb, marker in stream_rows:
            if marker in ring:
                stream_fail.append(f"'{verb}' output was copied into the kernel log ring")
        session.send_and_expect("klog attach console", r"\Z\A", timeout=1.0)

        results.append(("Command Output Is On The Console Stream, Not The Log (C0)",
                        not stream_fail, "; ".join(stream_fail)))

        # C0: the editor's box always ends where it says it ends.
        #
        # redraw_box() cleared each line it painted with \033[K but never
        # erased below the last one, so loading a 5-line file after a 10-line
        # file left lines 6-10 of the previous document on screen, below the
        # status line, looking like part of the new file.
        #
        # This asserts the erase sequence rather than the screen state, and
        # that is a deliberate limit rather than laziness: the stale text is
        # never re-transmitted -- it is already on the terminal and simply is
        # not cleared -- so the defect is invisible in the byte stream unless
        # the test models a screen. Position is what makes the assertion
        # meaningful: \033[J must come immediately after the status line, the
        # last thing the box draws. Emitted earlier it would erase the box
        # itself, and a bare "contains \033[J" check would not notice.
        ok, log = session.send_and_expect("e\nabc\x18\x03y\n",
                                          r"C-X C-C: exit ───\033\[0m\033\[J",
                                          timeout=5.0)
        results.append(("Editor Erases Below The Status Line (C0)", ok, log if not ok else ""))

        # NOTE: the "unknown link name is rejected" assertion deliberately does
        # NOT live here. On this single-node session no virtconsole is
        # attached, so a broken name lookup would fall back to a default link
        # that is also absent and still return #f -- passing for the wrong
        # reason. It runs in test_9p_remote_mount() instead, where the default
        # link genuinely exists.

        # 14. Interactive Line Editor Backward Cursor Insertion & Backspace Deletion
        # Type "ac", Left Arrow (\x1b[D), type "b" -> "abc", Backspace -> "ac"
        cmd_edit_cursor = "ac\x1b[Db\x7f"
        ok, log = session.send_and_expect(cmd_edit_cursor + "\n", r"Unbound symbol: ac", timeout=4.0)
        results.append(("Line Editor Backward Cursor Insertion & Deletion", ok, log if not ok else ""))

        # 15. Regression: primitive arity/type mismatches must not crash the
        # kernel (B1, see plan/completed/2026-08-07_review_and_remediation.md). Each of
        # these previously dereferenced a NULL pointer (or worse) when called
        # with too few / wrong-typed arguments. If any of them still crashed,
        # FAULT_MARKERS would fail this test outright; the trailing "=> 4"
        # check on unrelated, unaffected arithmetic additionally proves the
        # shell is still alive and evaluating correctly afterward, not just
        # that it failed to print a fault banner.
        cmd_arity_safety = (
            "lisp\n"
            "(= 1)\n"
            "(poke 1)\n"
            "(write \"/sd0/x.txt\")\n"
            "(cp \"/sd0/x.txt\")\n"
            "(cc \"/sd0/x.txt\")\n"
            "(write-file \"/sd0/x.txt\")\n"
            "(compile-file \"/sd0/x.txt\")\n"
            "(+ 2 2)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_arity_safety, r"=> 4", timeout=5.0)
        results.append(("Primitive Arity/Type Safety (no crash on malformed args, B1)", ok, log if not ok else ""))

        # 16. Regression: an oversized shell command line must be rejected
        # cleanly instead of overflowing the fixed-size S-expression
        # transformer buffer (B2, see
        # plan/completed/2026-08-07_review_and_remediation.md). 160 single-char tokens
        # previously walked sexpr[512] far out of bounds. The trailing
        # "(+ 3 3)" check on the same session proves the shell survived and
        # is still evaluating correctly.
        cmd_overflow = "ls " + " ".join(["a"] * 160) + "\n(+ 3 3)"
        ok, log = session.send_and_expect(cmd_overflow, r"=> 6", timeout=4.0)
        results.append(("Shell Command-Line Overflow Rejection (no crash on long input, B2)", ok, log if not ok else ""))

        # 17. Regression: self-recursion via (define name (lambda ...)) (B3,
        # see plan/completed/2026-08-07_review_and_remediation.md). The lambda used to
        # capture a frozen snapshot of the environment *before* its own
        # binding was added, so it could never see itself.
        cmd_recursion = (
            "lisp\n"
            "(define f (lambda (n) (if (= n 0) 1 (* n (f (- n 1))))))\n"
            "(f 5)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_recursion, r"=> 120", timeout=5.0)
        results.append(("Lisp Self-Recursion via (define name (lambda ...)) (B3)", ok, log if not ok else ""))

        # 18. Regression: (define (fn args...) body...) function-signature
        # form (B4). This is the exact factorial example from the README
        # that previously produced "Unbound symbol" errors instead of 720.
        cmd_define_fn_form = (
            "lisp\n"
            "(define (factorial n) (if (= n 0) 1 (* n (factorial (- n 1)))))\n"
            "(factorial 6)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_define_fn_form, r"=> 720", timeout=5.0)
        results.append(("Lisp (define (fn args) body) Function-Signature Form (B4)", ok, log if not ok else ""))

        # 19. Regression: = must actually differentiate string content, not
        # just always return the same answer (B5). Nested if makes a single
        # assertion catch both "same strings compare equal" and "different
        # strings compare unequal" -- a broken always-true or always-false
        # implementation would fail this even though a weaker "=> #t appears
        # somewhere" check would not.
        cmd_str_eq = (
            "lisp\n"
            "(if (= \"abc\" \"abc\") (if (= \"abc\" \"xyz\") 'both_wrong 'correct) 'first_wrong)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_str_eq, r"=> correct", timeout=4.0)
        results.append(("Lisp String Equality Correctly Differentiates via = (B5)", ok, log if not ok else ""))

        # 20. Regression: a lambda/function body of multiple forms must
        # evaluate all of them in sequence (like `begin`), not silently
        # drop everything after the first.
        cmd_multi_body = (
            "lisp\n"
            "(define (multi x) (display \"multi_body_side_effect\\n\") (* x 2))\n"
            "(multi 21)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_multi_body, r"multi_body_side_effect.*=> 42", timeout=4.0)
        results.append(("Lisp Multi-Body Lambda Evaluates All Forms In Sequence", ok, log if not ok else ""))

        # 21. Regression: a runaway/non-terminating recursive definition
        # must be stopped by the evaluation-depth guard (A4) instead of
        # overflowing the C stack. The trailing "(+ 5 5)" check on the same
        # session proves the shell survived and is still evaluating
        # correctly afterward, not just that it failed to print a fault
        # banner.
        cmd_depth_guard = (
            "lisp\n"
            "(define (loop n) (loop (+ n 1)))\n"
            "(loop 0)\n"
            "(+ 5 5)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_depth_guard, r"=> 10", timeout=5.0)
        results.append(("Lisp Recursion Depth Guard (no crash/hang on runaway recursion, A4)", ok, log if not ok else ""))


        # 22. Discoverability: the (help) Lisp primitive lists bound globals
        # (D2/D3), and the POSIX-shell `help` command points to it.
        ok, log = session.send_and_expect("lisp\n(help)\nexit", r"Bound Globals.*primitive", timeout=4.0)
        results.append(("Lisp (help) Primitive Lists Bound Globals (D2/D3)", ok, log if not ok else ""))

        ok, log = session.send_and_expect("help", r"\(help\)", timeout=3.0)
        results.append(("Shell help Command Mentions Lisp (help) Primitive (D2/D3)", ok, log if not ok else ""))

        # 23. Regression: fat32_write_file() must free a file's old cluster
        # chain when overwriting it, instead of leaking a new chain on every
        # write (B8, see plan/completed/2026-08-07_review_and_remediation.md).
        # Overwrite the same file 20 times and confirm reported free space
        # is unchanged (each write needs exactly 1 cluster; under the old
        # bug this would leak 19 of them). Parses actual numbers from two
        # separate `df` calls rather than just regex-matching text, since
        # this specifically needs to catch a *quantity* regression.
        # Create the file first (its very first write is a fresh allocation,
        # not an overwrite, and correctly costs one net cluster) so both
        # measurements below are taken with it already existing -- isolating
        # just the repeated-*overwrite* behavior this test targets.
        session.send_and_expect("write /ram0/leaktest.txt payload_0", r"=> #t", timeout=3.0)

        # /ram0/ appears twice in this output line (Filesystem column *and*
        # Mounted-on column), so the expected_pattern must wait for the
        # trailing occurrence -- matching the leading one returns before the
        # numeric fields in between have even streamed in yet.
        _, log_before = session.send_and_expect("cat /proc/df", r"%\s+/ram0/", timeout=3.0)
        m_before = re.search(r"/ram0/\s+(\d+)\s+(\d+)\s+(\d+)", log_before)
        free_before = int(m_before.group(3)) if m_before else None

        cmd_repeated_write = "\n".join(f"write /ram0/leaktest.txt payload_{i}" for i in range(1, 20))
        session.send_and_expect(cmd_repeated_write, r"=> #t", timeout=6.0)

        _, log_after = session.send_and_expect("cat /proc/df", r"%\s+/ram0/", timeout=3.0)
        m_after = re.search(r"/ram0/\s+(\d+)\s+(\d+)\s+(\d+)", log_after)
        free_after = int(m_after.group(3)) if m_after else None

        ok = (free_before is not None and free_before == free_after)
        results.append((
            "FAT32 Cluster Chain Freed on File Overwrite (no leak, B8)",
            ok,
            f"free_before={free_before} free_after={free_after}" if not ok else "",
        ))
        session.send_and_expect("rm /ram0/leaktest.txt", r"=> #t", timeout=3.0)

        # 24. Regression: directory scanning (find/list/write/mkdir/rm) must
        # correctly handle a directory whose entries span more than one
        # cluster in its FAT chain -- fat32_write_file's and fat32_mkdir's
        # free-slot searches used to only look at a directory's *first*
        # cluster at all, and every scan only read a cluster's first sector
        # (B9). Create several files in a subdirectory and verify each is
        # found with correct, uncorrupted content.
        cmd_multi_file = (
            "mkdir /ram0/many\n"
            "write /ram0/many/f0.txt content_zero\n"
            "write /ram0/many/f1.txt content_one\n"
            "write /ram0/many/f2.txt content_two\n"
            "write /ram0/many/f3.txt content_three\n"
            "cat /ram0/many/f0.txt\n"
            "cat /ram0/many/f1.txt\n"
            "cat /ram0/many/f2.txt\n"
            "cat /ram0/many/f3.txt"
        )
        ok, log = session.send_and_expect(
            cmd_multi_file,
            r"content_zero.*content_one.*content_two.*content_three",
            timeout=6.0,
        )
        results.append(("FAT32 Directory Scan Finds All Entries, Uncorrupted (B9)", ok, log if not ok else ""))
        session.send_and_expect(
            "rm /ram0/many/f0.txt\nrm /ram0/many/f1.txt\nrm /ram0/many/f2.txt\nrm /ram0/many/f3.txt\nrmdir /ram0/many",
            r"=> #t",
            timeout=4.0,
        )

        # 25. Regression: a corrupt/blank volume must no longer be silently
        # auto-formatted on mount (B10); (format "<path>") is now the only
        # way to initialize one. Format /ram0 explicitly and confirm the
        # freshly-formatted volume is immediately usable.
        cmd_format = (
            "lisp\n"
            "(format \"/ram0\")\n"
            "(write \"/ram0/after_format.txt\" \"format_worked\")\n"
            "(cat \"/ram0/after_format.txt\")\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_format, r"format_worked", timeout=5.0)
        results.append(("Explicit (format \"<path>\") Initializes a Usable Volume (B10)", ok, log if not ok else ""))

        # C1: a bare name at the shell runs /<vol>/system/bin/<name>.elf.
        #
        # The marker comes from inside the program, so a pass means the loader
        # actually ran a separately linked binary that nothing in the command
        # named by path -- resolution had to find it.
        ok, log = session.send_and_expect("uhello", r"UPROG_TEXT_OK", timeout=8.0)
        results.append(("Bare Name Resolves Through The Search Path (C1)", ok, log if not ok else ""))

        # ...and the path is the thing that decided which file that was.
        # (which) reports the winner without running it, which is what makes
        # the override test below checkable rather than inferred.
        # /sd0 rather than /flash0: one staging directory builds both images, so
        # the utility ships on every volume and the *first* one in the path is
        # the correct answer. Asserting flash0 here would pass only by the path
        # being broken.
        ok, log = session.send_and_expect(
            'lisp\n(which "uhello")\n(which "definitely_not_a_program")\nexit',
            r'=> "/sd0/system/bin/uhello.elf".*=> #f', timeout=6.0)
        results.append(("(which) Reports Resolution And Refuses Unknown Names (C1)",
                        ok, log if not ok else ""))

        # Precedence: a copy on a higher-priority volume shadows the shipped
        # one. This is the property the whole convention exists for -- `cc`
        # writing a new binary to /ram0/system/bin has to be reachable by name
        # immediately -- and it is asserted by *moving the answer*, not by
        # reading the path back. (which) pointing at /ram0 after the copy, when
        # it pointed at /flash0 before it, can only happen if order is real.
        session.send_and_expect("mkdir /ram0/system", r"=> ", timeout=4.0)
        session.send_and_expect("mkdir /ram0/system/bin", r"=> ", timeout=4.0)
        session.send_and_expect("cp /flash0/system/bin/uhello.elf /ram0/system/bin/uhello.elf",
                                r"=> ", timeout=6.0)
        ok, log = session.send_and_expect(
            'lisp\n(which "uhello")\nexit', r'=> "/ram0/system/bin/uhello.elf"', timeout=6.0)
        results.append(("Higher-Priority Volume Shadows A Shipped Utility (C1)",
                        ok, log if not ok else ""))

        # A full path is never searched for: it names exactly one file, so it
        # still reaches the flash copy while /ram0 is shadowing the name. This
        # is what keeps a specific build addressable no matter what is on the
        # path in front of it.
        ok, log = session.send_and_expect("exec /flash0/system/bin/uhello.elf",
                                          r"UPROG_TEXT_OK", timeout=8.0)
        results.append(("A Full Path Bypasses The Search Path (C1)", ok, log if not ok else ""))

        # The path is policy, readable as a file like everything else, and
        # settable at runtime. Reordering must change where a name resolves --
        # asserted the same way, by the answer moving back to /flash0.
        ok, log = session.send_and_expect("cat /proc/path", r"ram0 sd0 flash0", timeout=4.0)
        results.append(("/proc/path Reports The Search Path (C1)", ok, log if not ok else ""))

        ok, log = session.send_and_expect(
            'lisp\n(path-set "flash0 sd0 ram0")\n(which "uhello")\nexit',
            r'=> "/flash0/system/bin/uhello.elf"', timeout=6.0)
        results.append(("(path-set) Reorders Resolution At Runtime (C1)", ok, log if not ok else ""))
        session.send_and_expect('lisp\n(path-set "ram0 sd0 flash0")\nexit', r"=> ", timeout=4.0)

        # 26. /proc/meminfo reports measured runtime figures, not constants.
        #
        # Deliberately the LAST test in the suite. The stack high-water mark
        # and the heap's peak page count are cumulative since boot, so reading
        # them here makes them cover everything the suite has just done --
        # including test 21's runaway recursion, which drives the Lisp
        # evaluator into the deepest call chain in the system, and the task
        # demos, which are what actually allocate pages. Read at boot these
        # same fields would only ever describe the boot path, and a stack
        # measurement that never sees a deep stack proves nothing.
        #
        # Every assertion is a *relationship between the reported numbers*
        # rather than an expected size. Sizes here change whenever a static
        # array does, and a test that has to be re-baselined on every such
        # change gets re-baselined without being read. Relationships do not
        # move: what they catch is a field that has silently stopped being
        # measured (peak stuck at 0), or one that has started reporting
        # something impossible.
        #
        # Matching on the Storage line is itself one of the assertions. It is
        # generated last into the handle's fixed 512-byte proc_buf, and
        # ksnprintf() truncates rather than overruns -- so if the additions
        # above it ever outgrow the buffer, this match is what fails.
        ok, log = session.send_and_expect(
            "cat /proc/meminfo", r"Storage: /flash0/", timeout=4.0)

        def field(pattern: str) -> int | None:
            m = re.search(pattern, log)
            return int(m.group(1)) if m else None

        pages_free = field(r"Pages Free: (\d+)")
        pages_used = field(r"Pages Used: (\d+)")
        pages_peak = field(r"Pages Peak: (\d+)")
        largest_run = field(r"Largest Free Run: (\d+) pages")
        ram_kb = field(r"RAM: (\d+) KB total")
        image_kb = field(r"Image \([^)]*\): (\d+) KB")
        stack_kb = field(r"Boot Stack: (\d+) KB")
        stack_peak = field(r"Boot Stack: \d+ KB, peak (\d+) bytes")
        heap_kb = field(r"Heap: \d+ KB managed of (\d+) KB")

        checks: list[tuple[str, bool]] = []
        if None in (pages_free, pages_used, pages_peak, largest_run,
                    ram_kb, image_kb, stack_kb, stack_peak, heap_kb):
            checks.append(("all fields present", False))
        else:
            checks += [
                # The paint in entry.S ran, the scan in kernel/meminfo.c found
                # it, and the kernel really does use stack. A 0 here means the
                # measurement is broken, not that the stack is unused.
                ("stack high-water is measured", stack_peak > 0),
                # ...and the other end: no poison survived would report the
                # full region, which is what a genuine overflow looks like.
                ("stack did not overflow", stack_peak < stack_kb * 1024),
                # The peak is monotonic, so it can never be below the live
                # count taken from the same snapshot.
                ("peak pages >= live pages", pages_peak >= pages_used),
                # Something allocated during the suite -- task stacks at the
                # very least. A peak of 0 pages would mean the counter is not
                # being updated on the allocation path.
                ("heap peak is measured", pages_peak > 0),
                # A contiguous run is a subset of the free pages.
                ("largest run <= free pages", largest_run <= pages_free),
                # The RAM map's parts fit inside the region they describe.
                ("image + heap fit in RAM", image_kb + heap_kb <= ram_kb),
            ]

        failed = [label for label, passed in checks if not passed]
        detail = ""
        if not ok:
            detail = "meminfo did not render in full (truncated proc_buf?):\n" + log
        elif failed:
            detail = f"inconsistent figures: {', '.join(failed)}\n{log}"
        results.append(("/proc/meminfo Reports Measured Heap, Stack And RAM Map",
                        ok and not failed, detail))

        # 21b. Exhausting the node pool must not take the machine down.
        #
        # alloc_node() clamps to its last slot when the pool runs out, so every
        # further allocation returns the *same node* -- and a cons cell built
        # from two of them points at itself. The first list walker to touch
        # that cycle never returns: prim_add() spins while accumulating into
        # `sum`, which on these targets is a signed-overflow UBSan trap and on
        # RP2350 (built without UBSan) an unrecoverable hang needing a physical
        # replug. lisp_eval() now refuses to descend once the pool is gone.
        #
        # Ten runaway recursions rather than one, because pool size is a *per
        # target* constant: RP2350 has 512 nodes and exhausts on the first,
        # while these builds have 4096 and need about eight. That difference is
        # exactly why this went unseen -- the suite's depth-guard test above
        # runs the same recursion once and never reaches the interesting state.
        #
        # The assertion is two things the *system* emits, never anything typed:
        # the exhaustion warning, then the REPL printing a result. Getting a
        # result at all is the proof -- it means evaluation unwound and the
        # read-eval-print loop came back round, which is precisely what a walk
        # into a cyclic list never does. A hang fails on timeout; a UBSan trap
        # fails on FAULT_MARKERS before the regex is even tried.
        #
        # Deliberately not asserting a return to the `lsh>` prompt: every
        # command typed here appends to /sd0/system/history.lisp, and by this
        # point in the session that file is large enough that the FAT32 writes,
        # not the evaluator, dominate the runtime.
        cmd_pool_exhaust = ("lisp\n(define (loop n) (loop (+ n 1)))\n"
                            + "(loop 0)\n" * 8 + "exit\n")
        ok, log = session.send_and_expect(
            cmd_pool_exhaust, r"Node pool exhausted.*=> \(\)", timeout=25.0)
        results.append(("Node Pool Exhaustion Degrades Instead Of Hanging (P6 §6.4)",
                        ok, log if not ok else ""))

    finally:
        session.close()

    return results


def test_qemu_architecture_with_retry(
    elf_path: Path, img_path: Path, arch_name: str, max_attempts: int = 3,
) -> list[tuple[str, bool, str]]:
    """test_qemu_architecture(), retried on a specific, narrow failure shape.

    The stalls this was originally built to paper over turned out to have
    a different, now-fixed root cause -- catastrophic regex backtracking
    in send_and_expect()'s own matching, not QEMU (see that method's
    comment for the full story: every reproduction stopped once
    `(.|\\n)*` was replaced with `.*` throughout this file's patterns, on
    both platforms tested). This wrapper stays anyway, because the
    reasoning it was built on is still independently true and still not
    fully ruled out: QEMU's '-nographic' chardev sets O_NONBLOCK on its
    console fd and is documented not to always retry a write() that
    returns EAGAIN (see start()'s comment) -- a real, external failure
    mode this harness cannot fully control, only make unlikely. Cheap
    insurance against a class of flake that is no longer the *known*
    cause of anything, kept in case it is ever the cause of something new.

    Retrying is deliberately narrow, not "retry any failure": a genuine
    kernel hang would produce the exact same *shape* of failure (output
    stops, timeout elapses) as a host artifact, so retrying indiscrimin-
    ately would risk quietly re-running past a real regression instead of
    reporting it -- the opposite of what this suite exists to catch. Only
    retried when every failing result's log is silent on FAULT_MARKERS
    (nothing crashed) -- consistent with "guest finished cleanly, host
    never saw it" and not with a fault the guest itself reported. Retries
    the whole architecture's run from a fresh boot, not just the one
    failing step: QemuSession's state is one continuous session across
    every test in test_qemu_architecture(), so there is no way to resume
    just the failing step without re-deriving everything the tests after
    it already assumed. Bounded at max_attempts (3): a flake this suite
    cannot shake after that many full fresh boots is worth seeing as a real
    failure, not retried away indefinitely. """
    results = test_qemu_architecture(elf_path, img_path, arch_name)
    for attempt in range(2, max_attempts + 1):
        failed = [(name, log) for name, ok, log in results if not ok]
        if not failed:
            return results
        looks_like_host_stall = all(
            not any(marker in log for marker in QemuSession.FAULT_MARKERS)
            for _, log in failed
        )
        if not looks_like_host_stall:
            return results
        print(f"  [Retry] {arch_name}: {len(failed)} failure(s) with no fault "
              f"marker (possible QEMU host-stdio stall, not a guest crash) -- "
              f"retrying the whole run from a fresh boot "
              f"(attempt {attempt}/{max_attempts}) before treating as real.")
        results = test_qemu_architecture(elf_path, img_path, arch_name)
    return results


def test_terminal_crlf(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """C0: every newline reaching the terminal carries a carriage return.

    Boots its own QEMU **in binary mode**, which is the whole reason this is
    not a test inside QemuSession. That class opens the process with
    `text=True` and default newline handling, so Python's universal-newline
    translation rewrites '\\r\\n' and lone '\\r' to '\\n' before any assertion
    can see them -- fixed and broken output are byte-identical by the time
    they reach a test there. The defect is invisible to the entire existing
    suite, which is exactly how it survived.

    The bug: vprintk_to() inserted '\\r' only for newlines written literally
    in a format string, so bytes passed through %s went out untranslated.
    `cat` on any multi-line file printed a staircase -- each line starting at
    the column where the previous one ended. Now the console stream
    (console_emit) converts on the way to the device, so it cannot depend on
    how the bytes got there.

    Asserts on the *file's own* newlines, taken from the region between the
    first line of content and the closing brace, so an unrelated bare '\\n'
    elsewhere in the boot log cannot mask or fake the result.
    """
    name = f"Terminal CRLF On File Content (C0, {arch_name})"
    import shutil
    arch_img = img_path.with_name(f"test_{arch_name}_crlf_sd.img")
    shutil.copyfile(img_path, arch_img)

    qemu_bin = "qemu-system-riscv64" if "64" in arch_name else "qemu-system-riscv32"
    proc = subprocess.Popen(
        [qemu_bin, "-M", "virt", "-nographic", "-bios", "none",
         "-drive", f"file={arch_img},if=none,format=raw,id=hd0",
         "-device", "virtio-blk-device,drive=hd0",
         "-kernel", str(elf_path)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # Read until the file's closing brace has arrived rather than waiting for
    # the guest to exit: `exit` returns to the shell, it does not terminate
    # QEMU, so communicate() would always burn its full timeout here.
    out = b""
    try:
        assert proc.stdin is not None and proc.stdout is not None
        proc.stdin.write(b"cat /flash0/hello.c\n")
        proc.stdin.flush()
        deadline = time.time() + 20.0
        while time.time() < deadline:
            if not select.select([proc.stdout], [], [], 0.2)[0]:
                continue
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            out += chunk
            marker = out.find(b"#include <lugal.h>")
            if marker >= 0 and out.find(b"}", marker) >= 0:
                break
    except Exception as e:
        return (name, False, f"{e}\n{out[-400:].decode('utf-8', 'replace')}")
    finally:
        proc.kill()
        proc.wait(timeout=5)

    start = out.find(b"#include <lugal.h>")
    end = out.find(b"}", start) if start >= 0 else -1
    if start < 0 or end < 0:
        return (name, False, f"cat output not found in {len(out)} bytes:\n"
                             f"{out[-400:].decode('utf-8', 'replace')}")

    body = out[start:end]
    bare = len(re.findall(rb"(?<!\r)\n", body))
    total = body.count(b"\n")
    if bare:
        return (name, False,
                f"{bare} of {total} newlines in the file body lack a carriage return:\n"
                f"{body.decode('utf-8', 'replace')!r}")
    return (name, True, f"{total} newlines, all CRLF")


def test_9p_virtio_link(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """A3: proves the 9P server is reachable over a real, separate wire
    (drivers/virtio_console.c) by an external host process -- not any of
    LugalOS's own in-kernel 9P client code (drivers/loopback_net.c /
    drivers/uart_net.c). Boots its own dedicated QEMU instance with a
    virtio-serial chardev backed by a unix socket, connects tests/p9lib.py's
    independent Python 9P client to it, and reads a real pre-existing file
    (/sd0/system/etc/init.lisp) end to end: Tversion, Tattach("/"), a
    multi-component Twalk, Topen, Tread, Tclunk."""
    import shutil
    import tempfile
    import p9lib

    arch_img = img_path.with_name(f"test_{arch_name}_9p_sd.img")
    shutil.copyfile(img_path, arch_img)

    with tempfile.TemporaryDirectory() as tmpdir:
        sock_path = str(Path(tmpdir) / "p9c.sock")
        session = QemuSession(elf_path, arch_img, arch_name)
        try:
            session.start(extra_qemu_args=[
                "-device", "virtio-serial-device",
                "-device", "virtconsole,chardev=p9c",
                "-chardev", f"socket,id=p9c,path={sock_path},server=on,wait=off",
            ])
            # Wait for boot (also the shell prompt, so the guest is fully up
            # and virtio_console_init() has already run by the time we dial).
            ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
            if not ok:
                return ("9P Server Reachable Over VirtIO-Console Link (A3, external client)", False, log)

            last_err = ""
            for _ in range(20):  # the chardev socket file can lag slightly behind boot output
                try:
                    client = p9lib.P9Client.connect_unix(sock_path, timeout=2.0)
                    break
                except (FileNotFoundError, ConnectionRefusedError, OSError) as e:
                    last_err = str(e)
                    time.sleep(0.2)
            else:
                return ("9P Server Reachable Over VirtIO-Console Link (A3, external client)", False,
                        f"could not connect to {sock_path}: {last_err}")

            try:
                data = client.cat("/sd0/system/etc/init.lisp")
            finally:
                client.close()

            _want = expected_init_lisp()
            ok = b"LugalOS System Initialization Script" in data and len(data) == len(_want)
            log = "" if ok else f"unexpected content ({len(data)} bytes): {data[:120]!r}"
            return ("9P Server Reachable Over VirtIO-Console Link (A3, external client)", ok, log)
        except Exception as e:
            return ("9P Server Reachable Over VirtIO-Console Link (A3, external client)", False, str(e))
        finally:
            session.close()


def test_9p_uart_slip_link(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """A3a: proves the headless UART/SLIP 9P mode (kernel/shell.c's `p9serve`
    command) works over a real wire. Unlike the virtio-console test above,
    this can't use QemuSession's stdio-multiplexed console at all: QEMU's
    `-nographic` stdio backend intercepts byte 0x01 (Ctrl-A) as its own
    console/monitor escape character and never forwards it to the guest --
    which any binary 9P frame will eventually contain (e.g. as a tag byte).
    So this boots with the guest's serial port redirected to its own unix
    socket chardev (`-serial unix:...`) instead of stdio, with `-monitor
    none` freeing stdio entirely -- exactly how a real UART/CP2102 wire
    behaves (no escape-character interception), and sidesteps the
    stdio-mux issue rather than working around it. Speaks plain text
    ("p9serve\\n") to trigger headless mode, then switches the same
    connection to SLIP-framed binary 9P."""
    import shutil
    import tempfile
    import p9lib

    arch_img = img_path.with_name(f"test_{arch_name}_9p_uart_sd.img")
    shutil.copyfile(img_path, arch_img)

    name = "9P Server Reachable Over UART/SLIP Link (A3a headless p9serve, external client)"

    with tempfile.TemporaryDirectory() as tmpdir:
        sock_path = str(Path(tmpdir) / "uart9p.sock")
        session = QemuSession(elf_path, arch_img, arch_name)
        raw_sock: socket.socket | None = None
        try:
            session.start(extra_qemu_args=[
                "-serial", f"unix:{sock_path},server=on,wait=off",
                "-monitor", "none",
            ])

            last_err = ""
            for _ in range(30):
                try:
                    raw_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    raw_sock.settimeout(2.0)
                    raw_sock.connect(sock_path)
                    break
                except OSError as e:
                    last_err = str(e)
                    time.sleep(0.2)
            if raw_sock is None:
                return (name, False, f"could not connect to {sock_path}: {last_err}")

            time.sleep(2.5)  # let boot finish
            raw_sock.settimeout(0.3)
            try:
                while raw_sock.recv(65536):
                    pass
            except socket.timeout:
                pass

            raw_sock.settimeout(5.0)
            raw_sock.sendall(b"p9serve\n")
            time.sleep(0.5)
            raw_sock.settimeout(0.3)
            try:
                while raw_sock.recv(65536):
                    pass
            except socket.timeout:
                pass

            raw_sock.settimeout(5.0)
            client = p9lib.P9Client(raw_sock, framing="slip")
            data = client.cat("/sd0/system/etc/init.lisp")

            _want = expected_init_lisp()
            ok = b"LugalOS System Initialization Script" in data and len(data) == len(_want)
            log = "" if ok else f"unexpected content ({len(data)} bytes): {data[:120]!r}"
            return (name, ok, log)
        except Exception as e:
            return (name, False, str(e))
        finally:
            if raw_sock:
                raw_sock.close()
            session.close()


def test_9p_uart_demux_shared_wire(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """A3b: proves the shared-wire demux (kernel/shell.c's `p9share` command,
    drivers/uart_net.c's uart_demux_*()) actually demultiplexes -- unlike
    A3a's p9serve, the console must still be alive *after* real 9P traffic
    has gone over the same wire. Same stdio-mux workaround as
    test_9p_uart_slip_link() above (dedicated `-serial unix:...` chardev,
    `-monitor none`): speaks plain text ("p9share\\n") to arm the demux,
    performs one full SLIP-framed 9P transaction, then sends a plain-text
    console command over the SAME socket/connection and checks for a real
    shell response -- something p9serve's headless mode could never do."""
    import shutil
    import tempfile
    import p9lib

    arch_img = img_path.with_name(f"test_{arch_name}_9p_demux_sd.img")
    shutil.copyfile(img_path, arch_img)

    name = "9P + Console Coexist On One UART (A3b p9share demux, external client)"

    with tempfile.TemporaryDirectory() as tmpdir:
        sock_path = str(Path(tmpdir) / "uartdemux9p.sock")
        session = QemuSession(elf_path, arch_img, arch_name)
        raw_sock: socket.socket | None = None
        try:
            session.start(extra_qemu_args=[
                "-serial", f"unix:{sock_path},server=on,wait=off",
                "-monitor", "none",
            ])

            last_err = ""
            for _ in range(30):
                try:
                    raw_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    raw_sock.settimeout(2.0)
                    raw_sock.connect(sock_path)
                    break
                except OSError as e:
                    last_err = str(e)
                    time.sleep(0.2)
            if raw_sock is None:
                return (name, False, f"could not connect to {sock_path}: {last_err}")

            time.sleep(2.5)  # let boot finish
            raw_sock.settimeout(0.3)
            try:
                while raw_sock.recv(65536):
                    pass
            except socket.timeout:
                pass

            # Arm the demux -- console stays live, unlike p9serve.
            raw_sock.settimeout(5.0)
            raw_sock.sendall(b"p9share\n")
            time.sleep(0.5)
            raw_sock.settimeout(0.3)
            try:
                while raw_sock.recv(65536):
                    pass
            except socket.timeout:
                pass

            # A real 9P transaction over the now-shared wire.
            raw_sock.settimeout(5.0)
            client = p9lib.P9Client(raw_sock, framing="slip")
            data = client.cat("/sd0/system/etc/init.lisp")
            _want = expected_init_lisp()
            p9_ok = b"LugalOS System Initialization Script" in data and len(data) == len(_want)
            if not p9_ok:
                return (name, False, f"9P transaction failed: {len(data)} bytes: {data[:120]!r}")

            # The console must still work on the SAME connection afterward --
            # the actual point of A3b vs. A3a's headless mode. A plain-text
            # command should get a plain-text response containing content
            # only the real shell (not a stray 9P reply) would produce.
            raw_sock.settimeout(0.3)
            try:
                while raw_sock.recv(65536):
                    pass
            except socket.timeout:
                pass

            raw_sock.settimeout(5.0)
            raw_sock.sendall(b"help\n")
            deadline = time.time() + 5.0
            console_buf = b""
            while time.time() < deadline:
                raw_sock.settimeout(max(0.1, deadline - time.time()))
                try:
                    chunk = raw_sock.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                console_buf += chunk
                if b"p9share" in console_buf:
                    break

            console_ok = b"p9share" in console_buf
            if not console_ok:
                return (name, False,
                        f"console unresponsive after 9P traffic on shared wire: {console_buf[:200]!r}")

            return (name, True, "")
        except Exception as e:
            return (name, False, str(e))
        finally:
            if raw_sock:
                raw_sock.close()
            session.close()


def test_9p_multinode_heterogeneous(rv64_elf: Path, rv32_elf: Path,
                                    img64: Path, img32: Path) -> tuple[str, bool, str]:
    """A4/T2: the milestone that satisfies this phase's stated goal -- two
    different memory models, two different word widths, real 9P frames over
    a real socket, no hardware required, CI-runnable. Boots two independent
    LugalOS nodes (RV64 Sv39 MMU and RV32 NOMMU) and bridges their
    virtio-console links directly to each other over TCP -- QEMU's own
    socket chardev backend does the wiring natively (`server=on` on one
    side, a bare connecting socket chardev on the other), no host-side
    relay code needed. Node B (RV64) writes a marker file only it has;
    Node A (RV32) fetches it via `(p9-remote-cat ...)` -- genuine node-to-
    node traffic (proven by content only B could have produced), not
    host-to-node like the single-node A3 tests above.

    Port choice: a fixed port, matching scripts/run-qemu-multinode.sh's
    existing precedent (which used 4444) -- simple, but means this test
    cannot run concurrently with another instance of itself on the same
    host. Acceptable for a sequential CI run; worth revisiting (ephemeral
    port + readback) if that ever changes."""
    import shutil

    name = "9P Node-to-Node: RV32 NOMMU <-> RV64 MMU over Bridged VirtIO-Console (A4, T2)"
    port = 15590

    # Each node gets an image built for *its own* target. They shared one
    # until 2026-08-10, which meant one of the two nodes was loading user
    # programs for the other's instruction set.
    img_b = img64.with_name("test_multinode_b_sd.img")
    img_a = img32.with_name("test_multinode_a_sd.img")
    shutil.copyfile(img64, img_b)
    shutil.copyfile(img32, img_a)

    session_b = QemuSession(rv64_elf, img_b, "rv64")
    session_a = QemuSession(rv32_elf, img_a, "rv32")
    try:
        # Node B: server role. Its virtio-console chardev listens for the
        # bridge connection; it never initiates 9P traffic of its own.
        session_b.start(extra_qemu_args=[
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=p9c",
            "-chardev", f"socket,id=p9c,host=127.0.0.1,port={port},server=on,wait=off",
        ])
        ok, log = session_b.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        if not ok:
            return (name, False, f"Node B (RV64) failed to boot:\n{log}")

        marker = "HELLO_FROM_NODE_B_A4_T2"
        cmd = f'lisp\n(write-file "/ram0/multinode_marker.txt" "{marker}")\nexit'
        ok, log = session_b.send_and_expect(cmd, r"=> #t", timeout=4.0)
        if not ok:
            return (name, False, f"Node B failed to write its marker file:\n{log}")

        # Node A: client role. Dials directly into B's now-listening bridge
        # port; from here on the two GUEST kernels talk to each other, not
        # to the host.
        session_a.start(extra_qemu_args=[
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=p9c",
            "-chardev", f"socket,id=p9c,host=127.0.0.1,port={port}",
        ])
        ok, log = session_a.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        if not ok:
            return (name, False, f"Node A (RV32) failed to boot:\n{log}")

        # B2/D5: spawn a task that pumps this link's background server while
        # the client exchange below is in flight. Without it the yield inside
        # the client's reply-wait has nobody to switch to and the hazard never
        # occurs -- the test would pass whether or not frames are routed.
        # With it, the pump task reads the client's own replies off the wire,
        # and only fs/p9_link.c's type-parity + tag routing keeps this working.
        cmd = 'lisp\n(spawn-pump 512)\n(p9-remote-cat "/ram0/multinode_marker.txt")\nexit'
        ok, log = session_a.send_and_expect(cmd, re.escape(marker), timeout=5.0)
        return (name, ok, log if not ok else "")
    except Exception as e:
        return (name, False, str(e))
    finally:
        session_a.close()
        session_b.close()


def test_9p_remote_mount(rv64_elf: Path, rv32_elf: Path,
                         img64: Path, img32: Path) -> tuple[str, bool, str]:
    """A5: the actual "distributed namespace" payoff -- (mount-remote ...)
    attaches Node B's entire namespace at /netb/ on Node A, and from then on
    the *standard* shell commands (ls, cat, write) work through it exactly
    like any local mount, proven by round-tripping through Node B's real
    filesystem: Node A lists Node B's real /ram0/shared directory, reads a
    file Node B wrote, writes a *new* file through the mount, then Node B
    (after Node A disconnects) reads that file back from its own local
    /ram0/ to confirm the write genuinely landed there -- not just that
    Node A's `write` command returned success.

    Node B's own namespace root is what gets attached (aname left empty in
    p9_remote_mount_open()), not just one subtree -- so paths on Node A
    look like /netb/ram0/shared/..., not /netb/shared/... A different,
    separate port from test_9p_multinode_heterogeneous()'s, so the two
    tests' topologies never collide even if this file is ever changed to
    run them concurrently."""
    import shutil

    name = "9P Remote Mount: ls/cat/write Through /netb/ (A5, distributed namespace)"
    port = 15591

    img_b = img64.with_name("test_mount_b_sd.img")
    img_a = img32.with_name("test_mount_a_sd.img")
    shutil.copyfile(img64, img_b)
    shutil.copyfile(img32, img_a)

    session_b = QemuSession(rv64_elf, img_b, "rv64")
    session_a = QemuSession(rv32_elf, img_a, "rv32")
    try:
        session_b.start(extra_qemu_args=[
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=p9c",
            "-chardev", f"socket,id=p9c,host=127.0.0.1,port={port},server=on,wait=off",
        ])
        ok, log = session_b.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        if not ok:
            return (name, False, f"Node B (RV64) failed to boot:\n{log}")

        cmd = 'lisp\n(mkdir "/ram0/shared")\n(write-file "/ram0/shared/greeting.txt" "hello from B")\nexit'
        ok, log = session_b.send_and_expect(cmd, r"=> #t", timeout=4.0)
        if not ok:
            return (name, False, f"Node B failed to set up /ram0/shared:\n{log}")

        session_a.start(extra_qemu_args=[
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=p9c",
            "-chardev", f"socket,id=p9c,host=127.0.0.1,port={port}",
        ])
        ok, log = session_a.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        if not ok:
            return (name, False, f"Node A (RV32) failed to boot:\n{log}")

        # B0 part 3: an unknown link name must fail rather than silently
        # falling back to the default background link. This assertion has to
        # run *here*, on a node where the default link genuinely exists and
        # works -- in the single-node arch suite no virtconsole is attached, so
        # the fallback returns NULL too and #f would prove nothing. (Confirmed
        # the hard way: a version of this check placed there passed even with
        # name resolution deliberately removed.)
        ok, log = session_a.send_and_expect(
            'lisp\n(mount-remote "bogus" "no-such-link")\nexit', r"=> #f", timeout=4.0)
        if not ok:
            return (name, False,
                    f"Node A accepted an unknown link name instead of rejecting it:\n{log}")

        # Explicit device name: resolves the link through the device registry
        # by name. test_9p_multinode_heterogeneous()'s (p9-remote-cat "path")
        # still exercises the no-device-argument default, so both are covered.
        cmd = 'lisp\n(mount-remote "netb" "vconsole")\nexit'
        ok, log = session_a.send_and_expect(cmd, r"=> #t", timeout=4.0)
        if not ok:
            return (name, False, f"Node A failed to mount Node B's namespace:\n{log}")

        ok, log = session_a.send_and_expect("ls /netb/ram0/shared", r"GREETING\.TXT", timeout=4.0)
        if not ok:
            return (name, False, f"ls through the remote mount didn't see Node B's real directory:\n{log}")

        ok, log = session_a.send_and_expect("cat /netb/ram0/shared/greeting.txt", r"hello from B", timeout=4.0)
        if not ok:
            return (name, False, f"cat through the remote mount didn't return Node B's real content:\n{log}")

        ok, log = session_a.send_and_expect("write /netb/ram0/shared/from_a.txt written_by_node_A", r"=> #t", timeout=4.0)
        if not ok:
            return (name, False, f"write through the remote mount failed:\n{log}")

        session_a.close()

        # Read the file back from Node B's own local filesystem -- proof
        # the write genuinely reached Node B's disk, not just that Node A's
        # `write` command claimed success.
        ok, log = session_b.send_and_expect("cat /ram0/shared/from_a.txt", r"written_by_node_A", timeout=4.0)
        return (name, ok, log if not ok else "")
    except Exception as e:
        return (name, False, str(e))
    finally:
        session_a.close()
        session_b.close()


def test_host_fat32_image(img_path: Path) -> tuple[bool, str]:
    """Inspects the raw FAT32 disk image directly on the host OS."""
    if not img_path.exists():
        return False, f"Image file '{img_path}' does not exist"

    try:
        with open(img_path, "rb") as f:
            img = f.read()

        if len(img) < 524288:
            return False, f"Image size {len(img)} bytes is less than expected 512KB"

        # Check FAT32 Boot Sector Signature 0x55AA at offset 510
        if img[510:512] != b"\x55\xAA":
            return False, "Invalid FAT32 boot sector signature"

        # Check Root Directory LBA 48 (Cluster 2) for entries
        root_lba = 48
        root_sec = img[root_lba * 512 : (root_lba + 1) * 512]
        entries_found: list[str] = []
        for i in range(16):
            entry = root_sec[i * 32 : (i + 1) * 32]
            if entry[0] == 0:
                break
            if entry[0] == 0xE5:
                continue
            name = entry[0:11].decode("ascii", errors="ignore")
            entries_found.append(name.strip())

        if not entries_found:
            return False, "No directory entries found in FAT32 root sector"

        return True, f"Verified FAT32 Boot Record & Root Entries: {', '.join(entries_found[:5])}"

    except Exception as e:
        return False, f"Error inspecting FAT32 image: {e}"


def main() -> int:
    """Main entry point for LugalOS Test Suite."""
    project_root = Path(__file__).resolve().parent.parent
    build_dir = project_root / "build"
    # Per target: the image carries target-specific user program binaries, so
    # rv32 and rv64 cannot share one. They did until 2026-08-10, and an RV32
    # binary running on the RV64 kernel merely looked odd rather than failing
    # -- until a program that dereferences a pointer arrived.
    def img_for(arch: str) -> Path:
        return build_dir / arch / "lugalos_sd.img"

    rv64_elf = build_dir / "rv64" / "lugalos.elf"
    rv32_elf = build_dir / "rv32" / "lugalos.elf"

    print("======================================================================")
    print("                  LugalOS Automated Test Suite")
    print("======================================================================")

    total_tests = 0
    passed_tests = 0
    start_time = time.time()

    def _run_single(result: tuple[str, bool, str]) -> None:
        nonlocal total_tests, passed_tests
        name, ok, log = result
        total_tests += 1
        if ok:
            passed_tests += 1
            print(f"  [PASS] {name}")
        else:
            print(f"  [FAIL] {name}\n    Log Output:\n{log}")

    # 1. Host Disk Inspection
    print("\n[Host FAT32 Storage Inspection]")
    ok, info = test_host_fat32_image(img_for("rv32"))
    total_tests += 1
    if ok:
        passed_tests += 1
        print(f"  [PASS] {info}")
    else:
        print(f"  [FAIL] {info}")

    # 2. RV64 Target
    if rv64_elf.exists():
        print("\n[Target: RV64 Sv39 MMU Virtual Memory]")
        rv64_results = test_qemu_architecture_with_retry(rv64_elf, img_for("rv64"), "rv64")
        for name, ok, log in rv64_results:
            total_tests += 1
            if ok:
                passed_tests += 1
                print(f"  [PASS] {name}")
            else:
                print(f"  [FAIL] {name}\n    Log Output:\n{log}")

        _run_single(test_terminal_crlf(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_9p_virtio_link(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_9p_uart_slip_link(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_9p_uart_demux_shared_wire(rv64_elf, img_for("rv64"), "rv64"))
    else:
        print(f"\n[!] RV64 binary not found at '{rv64_elf}'. Skipping RV64 tests.")

    # 3. RV32 Target
    if rv32_elf.exists():
        print("\n[Target: RV32 NOMMU Microcontroller]")
        rv32_results = test_qemu_architecture_with_retry(rv32_elf, img_for("rv32"), "rv32")
        for name, ok, log in rv32_results:
            total_tests += 1
            if ok:
                passed_tests += 1
                print(f"  [PASS] {name}")
            else:
                print(f"  [FAIL] {name}\n    Log Output:\n{log}")

        _run_single(test_terminal_crlf(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_virtio_link(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_uart_slip_link(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_uart_demux_shared_wire(rv32_elf, img_for("rv32"), "rv32"))
    else:
        print(f"\n[!] RV32 binary not found at '{rv32_elf}'. Skipping RV32 tests.")

    # 4. A4/T2: multi-node heterogeneous interconnect
    if rv64_elf.exists() and rv32_elf.exists():
        print("\n[Target: Multi-Node RV32 <-> RV64 Heterogeneous Interconnect]")
        _run_single(test_9p_multinode_heterogeneous(rv64_elf, rv32_elf, img_for("rv64"), img_for("rv32")))
        _run_single(test_9p_remote_mount(rv64_elf, rv32_elf, img_for("rv64"), img_for("rv32")))
    else:
        print("\n[!] RV64 and/or RV32 binary not found. Skipping multi-node test.")

    duration = time.time() - start_time
    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed_tests} / {total_tests} Tests PASSED ({duration:.2f}s)")
    print("======================================================================\n")

    return 0 if (passed_tests == total_tests and total_tests > 0) else 1


if __name__ == "__main__":
    sys.exit(main())
