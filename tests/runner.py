#!/usr/bin/env python3
"""Automated Test Runner & Inspection Framework for LugalOS Microkernel Operating System."""

from __future__ import annotations

import os
import hashlib
import re
import select
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import BinaryIO

# host/p9lib is the promoted, product-grade home of the 9P client this file
# used to import as a same-directory sibling (tests/p9lib.py, now retired) --
# see host/p9lib/README.md.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host" / "p9lib" / "src"))


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

    def start(self, extra_qemu_args: list[str] | None = None, identity_img_path: "Path | None" = None) -> None:

        qemu_bin = "qemu-system-riscv64" if "64" in self.arch else "qemu-system-riscv32"
        cmd: list[str] = [
            qemu_bin,
            "-M", "virt",
            "-nographic",
            "-bios", "none",
            "-d", "guest_errors,unimp",
        ]
        if identity_img_path is not None:
            # I2, plan/phase21_identity_and_authentication.md: the identity
            # store's second virtio-blk device MUST be declared before hd0's
            # below, not after (e.g. via extra_qemu_args, which lands at the
            # end of the command line). QEMU's riscv "virt" machine binds
            # virtio-mmio transport slots to `-device virtio-blk-device`
            # entries in REVERSE command-line order -- the LAST such device
            # on the line gets the LOWEST MMIO address, which is the first
            # one both drivers' address-ascending probes see. Declaring hd1
            # here, ahead of hd0, is what keeps hd0 the first blk device
            # found (unchanged, existing behaviour) and hd1 the second
            # (drivers/virtio_blk_id.c), rather than silently swapping which
            # driver mounts which disk -- found empirically while bringing
            # this test up, not asserted from documentation.
            cmd += [
                "-drive", f"file={identity_img_path},if=none,format=raw,id=hd1",
                "-device", "virtio-blk-device,drive=hd1",
            ]
        cmd += [
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

        **Expect the last line the command prints, not the first.** This
        returns as soon as the pattern matches, so output produced afterwards
        is captured only if it happened to arrive in the same read. Matching an
        early confirmation and then asserting on a later line is a race that
        passes on an idle machine and fails under load -- and fails looking
        like a fault in the guest.

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
                # One more non-blocking read before returning.
                #
                # This returns on the first chunk in which the pattern
                # matches, so anything the guest printed *after* it is in the
                # result only by luck of chunk boundaries. A caller that waits
                # for one line and then asserts on a later one is therefore
                # racing, and it fails as a firmware fault rather than as a
                # test bug -- which cost several dismissals of an "unexplained
                # flake" before it was tracked down (2026-09-04, the I6 WLAN
                # credential test).
                #
                # This does not make such a caller correct; the fix for that is
                # to expect the *last* line the command produces. But it costs
                # nothing, it strictly widens what is captured, and it removes
                # the most common version of the race -- output already written
                # to the log and simply not read yet.
                trailing = self._log_file.read()
                if trailing:
                    accumulated += trailing
                    text = accumulated.decode("utf-8", "replace")
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

        # S0, plan/phase22_smp_locking_foundation.md §6.2: the kernel can
        # identify the hart it is running on.
        #
        # Worth a test of its own because no other test here can fail on it.
        # Every build before S0 passed this whole suite while being completely
        # unable to answer the question, and the interesting half of the
        # answer is target-specific: on rv64-mmu the kernel runs in S-mode,
        # where the obvious implementation (`csrr mhartid`) traps as an
        # illegal instruction, because entry.S performs the M->S transition
        # itself with no SBI firmware underneath to ask instead. So this
        # asserting on *both* arches is the point -- passing on rv32 alone
        # would say nothing about the case that motivated the work.
        #
        # `consistent: yes` is the real assertion. It checks that the record
        # `tp` points at is the array slot the id inside that record names,
        # which is what distinguishes a working hart pointer from one that
        # merely holds a plausible address.
        ok, log = session.send_and_expect(
            "cat /proc/cpuinfo", r"hart:\s+0", timeout=3.0)
        results.append(("Hart Identity Readable From The Kernel (S0)", ok, log if not ok else ""))

        expect_priv = "S" if "64" in arch_name else "M"
        ok, log = session.send_and_expect(
            "cat /proc/cpuinfo",
            rf"consistent:\s+yes[\s\S]*priv:\s+{expect_priv}", timeout=3.0)
        results.append((f"Hart Record Is Self-Consistent, In {expect_priv}-Mode (S0)",
                        ok, log if not ok else ""))

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

        # D1 (plan/phase17_clock_ui_and_dcf77.md): the DCF-77 frame decoder,
        # against synthetic sample streams rather than a radio.
        #
        # This runs on the QEMU targets deliberately, and it is the only part
        # of the DCF-77 work that can be tested at all without hardware: the
        # decoder consumes timestamped pin samples, never a register, so the
        # frame logic -- parity, the two-frame agreement rule, polarity
        # auto-detection, the "time is the NEXT minute" rule, and date
        # rollover -- is exercised here in milliseconds instead of by flashing
        # a board and waiting minutes for a signal that may not arrive.
        # Everything downstream of it stays hardware-only, per
        # [[falsify_on_hardware_not_qemu]]; this is the half that is portable.
        # N1 (plan/phase18_networking_and_auth.md): the first cryptography in
        # this tree, against the published vectors. Target-independent for the
        # same reason the DCF-77 decoder below is: an auth exchange's fiddly
        # part is its arithmetic, and arithmetic must not be debugged by
        # flashing a board. RFC 4231's cases 6 and 7 are the load-bearing ones
        # -- they exercise the key-longer-than-the-block branch that every
        # short-key test passes without.
        ok, log = session.send_and_expect("hmacselftest\n",
                                          r"HMAC_SELFTEST_(OK|FAIL)", timeout=30.0)
        hmac_ok = ok and "HMAC_SELFTEST_OK" in log
        results.append(("SHA-256/HMAC-SHA-256 Against FIPS and RFC 4231 Vectors (N1)",
                        hmac_ok, log if not hmac_ok else ""))

        # S1, plan/phase22_smp_locking_foundation.md: the two cross-hart lock
        # types, before anything is converted to use them.
        #
        # Six checks, and the one that carries the phase is the third: the
        # tick counter must be frozen across an irqsave-held spinlock and
        # moving when it is not held. That is random.c's standard -- a
        # measured effect rather than the implementation restating itself --
        # and it was confirmed by removing irq_save() from the acquire and
        # watching it fail (ticks 239 -> 242 while "held").
        #
        # The seventh check is S6's: every resume from a ctx_switch() must
        # arrive still holding the scheduler lock its predecessor took. That
        # is the inverse of the canary the plan originally specified ("never
        # held across the switch"), which would have fired on correct code --
        # and unlike the race it guards against, it is observable on one hart.
        #
        # What these deliberately do NOT claim: nothing here observes two
        # harts racing, because no target can do that until phase 23's X1.
        # They prove the primitives behave correctly under the concurrency
        # this kernel has today -- preemption and yielding -- which is what
        # S2-S6 are about to build on.
        ok, log = session.send_and_expect("lockselftest\n",
                                          r"LOCK_SELFTEST_(OK|FAIL)", timeout=30.0)
        lock_ok = ok and "LOCK_SELFTEST_OK" in log
        results.append(("Cross-Hart Lock Primitives: Atomic Gate, Real Masking, ylock Re-entry (S1)",
                        lock_ok, log if not lock_ok else ""))

        # The nonce source behind that gate. On QEMU there is no hardware
        # entropy and the command says so rather than inventing a verdict --
        # SKIP is the pass here, and RANDTEST_OK/WEAK is a hardware result.
        ok, log = session.send_and_expect("randtest\n",
                                          r"RANDTEST_(OK|WEAK|SKIP)", timeout=30.0)
        rand_ok = ok and "RANDTEST_SKIP" in log
        results.append(("Entropy Source Reports Honestly With No Hardware (N1)",
                        rand_ok, log if not rand_ok else ""))

        # N2: the auth gate's pure logic -- the key-store path guard and the
        # fact that the response MAC binds nonce, identity and key. No
        # network, no peer, so it runs wherever the others do.
        ok, log = session.send_and_expect("p9authselftest\n",
                                          r"P9AUTH_SELFTEST_(OK|FAIL)", timeout=30.0)
        auth_ok = ok and "P9AUTH_SELFTEST_OK" in log
        results.append(("9P Auth Gate Path Guard And MAC Binding (N2)",
                        auth_ok, log if not auth_ok else ""))

        # I1 (plan/phase21_identity_and_authentication.md): the identity
        # record's three states, unknown-field skipping, and a
        # byte-identical round trip -- against an in-memory fake
        # block_dev_t, so like the two selftests above this needs neither
        # hardware nor a mounted filesystem. I2 wires this to a real device;
        # this is the parser and the CRC underneath it, falsified first.
        ok, log = session.send_and_expect("idstoreselftest\n",
                                          r"IDSTORE_SELFTEST_(OK|FAIL)", timeout=30.0)
        idstore_ok = ok and "IDSTORE_SELFTEST_OK" in log
        results.append(("Identity Record: States, Corruption, Unknown Fields, Round Trip (I1)",
                        idstore_ok, log if not idstore_ok else ""))

        # P2 (plan/phase24_dcf77_precision_and_ntp_server.md): the wall clock
        # keeps microseconds now. Everything here except the sub-millisecond
        # assertions would have passed on the millisecond clock it replaced,
        # which is the point -- those two are what P3b's PPS comparison needs
        # and what the old representation silently rounded to zero.
        # P3: the edge-capture ring, which the DCF pin and a GPS module's PPS
        # both feed. Portable half only -- the interrupt that fills it needs
        # RP2350, the off-by-one that would break it does not.
        ok, log = session.send_and_expect("edgecapselftest\n",
                                          r"EDGECAP_SELFTEST_(OK|FAIL)", timeout=20.0)
        ec_ok = ok and "EDGECAP_SELFTEST_OK" in log
        results.append(("Edge Capture Ring: Order, Wrap, And Which Edge Gets Dropped (P3)",
                        ec_ok, log if not ec_ok else ""))

        ok, log = session.send_and_expect("timeselftest\n",
                                          r"TIME_SELFTEST_(OK|FAIL)", timeout=20.0)
        t_ok = ok and "TIME_SELFTEST_OK" in log
        results.append(("Microsecond Wall Clock: Round Trip And Sub-Millisecond Detail (P2)",
                        t_ok, log if not t_ok else ""))

        # P5: the discipline loop. Every property here is one the loop must
        # have to be worth running at all -- it steps a wildly wrong clock
        # rather than slewing for hours, never steps a nearly-right one,
        # rejects the plausible-but-wrong frame that marginal reception
        # actually produces, gives up rejecting when the world really has
        # moved, and learns a *rate* from a run of phase measurements rather
        # than only chasing each one. Driven through the shipping entry point
        # and it restores the clock afterwards, so it is safe on a board whose
        # clock other things are using.
        ok, log = session.send_and_expect("disciplineselftest\n",
                                          r"DISCIPLINE_SELFTEST_(OK|FAIL)", timeout=20.0)
        d_ok = ok and "DISCIPLINE_SELFTEST_OK" in log
        results.append(("Clock Discipline: Step, Slew, Outlier Rejection And Rate Learning (P5)",
                        d_ok, log if not d_ok else ""))

        ok, log = session.send_and_expect("dcf77selftest\n",
                                          r"DCF77_SELFTEST_(OK|FAIL)", timeout=30.0)
        sel_ok = ok and "DCF77_SELFTEST_OK" in log
        results.append(("DCF-77 Frame Decoder Against Synthetic Frames (D1)",
                        sel_ok, log if not sel_ok else ""))

        # The timezone rules the clock renders local time with. Pure
        # arithmetic over a table of known transitions -- both sides of both
        # European switchovers, a southern-hemisphere zone whose summer spans
        # the new year, and a full-year round trip -- so it belongs on QEMU
        # for the same reason the DCF-77 decoder does. The kernel clock runs
        # on UTC (kernel/timezone.h); this is the half that turns it into
        # something a person reads.
        ok, log = session.send_and_expect("tzselftest\n",
                                          r"TZ_SELFTEST_(OK|FAIL)", timeout=30.0)
        tz_ok = ok and "TZ_SELFTEST_OK" in log
        results.append(("Timezone Rules Against Known DST Transitions",
                        tz_ok, log if not tz_ok else ""))

        # C3: the Pico-Clock-Green menu. Key events in, screen descriptions
        # out, no hardware anywhere in it -- so the whole navigation contract
        # (wrapping, long-press-is-back, the inactivity timeout, an abandoned
        # edit changing nothing, a day that stops existing when the month
        # moves under it) is checked here rather than by standing in front of
        # a clock pressing buttons.
        ok, log = session.send_and_expect("clockuiselftest\n",
                                          r"CLOCKUI_SELFTEST_(OK|FAIL)", timeout=30.0)
        ui_ok = ok and "CLOCKUI_SELFTEST_OK" in log
        results.append(("Pico-Clock-Green Menu Against Synthetic Key Presses (C3/C6)",
                        ui_ok, log if not ui_ok else ""))

        # The clock is UTC and `date` renders it: both lines, and the
        # abbreviation that says which side of a switchover we are on.
        ok, log = session.send_and_expect("date\n", r"UTC  \(TZ=", timeout=10.0)
        date_ok = ok and ("CET" in log or "CEST" in log)
        results.append(("date Shows Local Time And The UTC It Is Derived From",
                        date_ok, log if not date_ok else ""))

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

        # 5a. The C that the fixture above never contains.
        #
        # /sd0/hello.c is one function called `main()` with an empty parameter
        # list and an <lugal.h> include that resolves to a built-in string
        # without ever reading a file. All four defects this exercises lived
        # behind exactly that shape, and this suite was green throughout:
        #
        #   - `(void)` parameters halted the board: the parser took the
        #     closing paren as the parameter name, walked out of the parameter
        #     list, and spun on a cycle in `locals` until UBSan fired
        #   - two named parameters halted it the same way for a different
        #     reason (Obj::next carried the locals and params chains at once)
        #   - a second function compiled clean and returned the wrong answer:
        #     the node/object pools were reset at the top of every function,
        #     so only the last function kept an intact AST
        #   - a quoted #include read sizeof(char *) - 1 bytes of its header
        #
        # multi.h is staged into /ram0 because that is where a relative quoted
        # include is searched (user/chibicc/preprocess.c), which keeps this on
        # the normal resolution path rather than an absolute-path special
        # case. The expected string is three computed values, so a miscompile
        # shows up as different numbers rather than as silence -- silence and
        # a bare 0 were both symptoms here, and neither can be told apart from
        # "the program never ran".
        cmd_multi = ("cp /sd0/multi.h /ram0/multi.h\n"
                     "cc /sd0/multi.c /ram0/multi.elf\n"
                     "exec /ram0/multi.elf")
        ok, log = session.send_and_expect(cmd_multi, r"42-123-42", timeout=20.0)
        results.append(("chibicc: several functions, (void) and multi-arg params, a real #include",
                        ok, log if not ok else ""))

        # 5a-ii. `ed` round-trips a file it did not create.
        #
        # The ed test above appends two lines to a *new* file and asserts on
        # `1,$n` -- ed printing its own in-memory buffer. That holds whether
        # or not a byte ever reaches the disk, and it never loads an existing
        # file at all. Both halves of ed's file I/O were sized by sizeof(a
        # pointer): it read 2 bytes of any file and wrote at most 2 back, so
        # opening a real file showed its first two characters and saving
        # destroyed it. The byte count is the assertion here -- "31 bytes (3
        # lines)" cannot be satisfied by a truncated read -- and the trailing
        # `cat` runs after ed has exited, so it is the file talking rather
        # than the editor.
        session.send_and_expect(
            '(write-file "/ram0/ed_rt.txt" "alpha_one\\nbeta_two\\ngamma_three\\n")',
            r"#t", timeout=4.0)
        ok, log = session.send_and_expect(
            "ed /ram0/ed_rt.txt\n1,$p\nw\nq", r"31 bytes \(3 lines\)", timeout=5.0)
        results.append(("ed loads a whole file, not its first two bytes", ok, log if not ok else ""))
        ok, log = session.send_and_expect(
            "cat /ram0/ed_rt.txt",
            r"alpha_one[\s\S]*beta_two[\s\S]*gamma_three", timeout=4.0)
        results.append(("ed writes the whole buffer back", ok, log if not ok else ""))

        # 5a-iii. /proc/meminfo reports the mounts that actually exist, in
        # this build's own terms. The line it replaces was a fixed string
        # naming "/sd0/ (VirtIO SD)" on every target -- including RP2350,
        # which has no VirtIO at all -- and named /sd0 whether or not
        # anything was mounted there.
        ok, log = session.send_and_expect(
            "cat /proc/meminfo",
            r"Storage:.*/ram0/ \(FAT32 In-Memory RAMDisk\)", timeout=4.0)
        results.append(("/proc/meminfo storage line comes from the mount table",
                        ok, log if not ok else ""))

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
        #
        # `fen <position>` (the argument form) is here as well as bare `fen`,
        # and the two are different code paths: bare `fen` *prints* the
        # current position, the argument form *parses* one. Only the printing
        # side was covered until §1.3 of
        # plan/phase15_memory_reclamation.md routed both of this file's
        # FEN-parsing sites -- this one and `load`'s -- through chess_ui.c's
        # shared scratch Position instead of a permanent static each. `load`
        # was already exercised below; this was the parse path that was not.
        # The position used is a real mid-game one, so a parse that silently
        # produced the start position would still fail the board check.
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
            "fen r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3\n"
            "save\n"
            "load\n"
            "quit"
        )
        ok, log = session.send_and_expect(cmd_chess, r"Game loaded from", timeout=15.0)
        results.append(("Chess Console REPL: new/move+engine-reply/board/eval/moves/undo/redo/fen/save/load (J1)", ok, log if not ok else ""))

        # The argument form of `fen` specifically (§1.3). Separate assertion
        # from the sequence above rather than folded into it: "the whole REPL
        # still works" and "this one parse path still works" fail for
        # different reasons and should be readable apart.
        ok, log = session.send_and_expect(
            "(chess)\n"
            "fen r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3\n"
            "quit",
            r"Position loaded\.", timeout=15.0)
        results.append(("Chess Console: `fen <position>` parses into the shared scratch (§1.3)",
                        ok, log if not ok else ""))

        # 10b-3. SAN/PGN notation (14b).
        #
        # Separate from the search tests: this exercises *notation*, which
        # fails silently by nature -- a mis-disambiguated SAN string is still a
        # legal-looking move, and a PGN that loses a move still loads. The
        # primitive round-trips every legal move in six positions chosen for
        # the cases that break naive implementations (file and rank
        # disambiguation, castling, capture-promotion, mate suffix) and then
        # saves and reloads two whole games, requiring an identical FEN and
        # move count back. It found a real file/rank inversion when first run.
        ok, log = session.send_and_expect("(chess-san-selftest)",
                                          r"SAN Results: \d+ moves checked, 0 errors", timeout=30.0)
        results.append(("Chess SAN/PGN Notation Round-Trips (14b)", ok, log if not ok else ""))

        # 10b-4. PGN save/load/archive through the console, end to end -- named
        # saves, `new` retiring the previous game to games/, and loading it
        # back by name. The assertion is the reload, since it can only succeed
        # if the write, the archive rename and the SAN round-trip all worked.
        ok, log = session.send_and_expect(
            "(chess-console)\n"
            "level 1\n"
            "e2e4\n"
            "save keeper\n"
            "new\n"
            "load keeper\n"
            "quit",
            r"Game loaded from .*keeper\.pgn \(\d+ half-moves\)", timeout=40.0)
        results.append(("Chess PGN Save/Archive/Load By Name (14b)", ok, log if not ok else ""))

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

        # 21. Regression: a runaway/non-terminating NON-tail recursive
        # definition must be stopped by the evaluation-depth guard (A4)
        # instead of overflowing the C stack. The recursive call here is an
        # argument to `+`, not the whole result, so it is not a tail call --
        # tail-call optimization (S1, plan/phase13_lisp_engine_extensions.md)
        # does not apply, and every level still recurses through the C
        # stack/eval_depth exactly as before, tripping LISP_MAX_EVAL_DEPTH
        # quickly and cheaply. This used to be `(loop (+ n 1))` (a tail
        # call): post-TCO that shape no longer touches eval_depth at all and
        # instead runs until it exhausts the whole node/string pool for the
        # rest of the session (see test 21b below, and 21c for the
        # tail-call-optimized case this replaces testing here), which would
        # break every later test sharing this QEMU session -- not what this
        # test is meant to exercise. The trailing "(+ 5 5)" check on the same
        # session proves the shell survived and is still evaluating
        # correctly afterward, not just that it failed to print a fault
        # banner.
        cmd_depth_guard = (
            "lisp\n"
            "(define (loop n) (+ 1 (loop (+ n 1))))\n"
            "(loop 0)\n"
            "(+ 5 5)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_depth_guard, r"=> 10", timeout=5.0)
        results.append(("Lisp Recursion Depth Guard (no crash/hang on runaway non-tail recursion, A4)", ok, log if not ok else ""))

        # 21c. Tail-call optimization (S1, plan/phase13_lisp_engine_extensions.md):
        # a self-recursive call in tail position -- (loop (+ n 1)) is the
        # entire result of the `if`'s else-branch, which is itself the whole
        # lambda body -- must NOT grow eval_depth per iteration. 60
        # iterations is comfortably past the old LISP_MAX_EVAL_DEPTH=100
        # ceiling for this shape (a tail call used to cost one eval_depth
        # level exactly like a non-tail one, and this shape's extra non-tail
        # levels per iteration -- the `if` test, the operator lookup, the
        # argument evaluation -- meant it previously stalled at "Maximum
        # evaluation depth exceeded" well under n=60), while staying modest
        # against the shared node/string pool: this session has already run
        # 20 earlier tests against the same never-reclaimed pool (no GC --
        # see S0/S3 in the same plan doc) by the time this one runs, so this
        # deliberately does not push anywhere near the budget the way 21b
        # immediately below intentionally does.
        cmd_tco = (
            "lisp\n"
            "(define (loop n) (if (= n 60) n (loop (+ n 1))))\n"
            "(loop 0)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_tco, r"=> 60", timeout=8.0)
        results.append(("Lisp Tail-Call Optimization (self-recursive tail call does not grow eval_depth, S1)", ok, log if not ok else ""))

        # 21d. let* (S2, plan/phase13_lisp_engine_extensions.md): each
        # binding's initializer sees every binding before it -- the same
        # form under plain `let` would report "Unbound symbol: a", since
        # `let` evaluates every initializer against the outer scope only.
        ok, log = session.send_and_expect(
            "lisp\n(let* ((a 1) (b (+ a 1))) b)\nexit",
            r"=> 2", timeout=4.0)
        results.append(("Lisp let* Sequential Binding Visibility (S1)", ok, log if not ok else ""))

        # 21e. while (S2): a genuine multi-iteration loop. Comparison
        # operators and cons cells don't exist until S4, so this proves
        # real repeated execution the only way currently possible -- global
        # state mutated via `define` (which always rebinds global_env
        # regardless of nesting) inside the loop body, re-checked by the
        # condition on each pass, `not`/`cond`/`=` being the only tools on
        # hand. A zero-iteration case (false from the start) is folded in
        # first to prove the body genuinely doesn't run when it shouldn't.
        cmd_while = (
            "lisp\n"
            "(while #f (define should_not_exist 999))\n"
            "(define counter 0)\n"
            "(while (not (= counter 3)) "
            "(cond ((= counter 0) (define counter 1)) "
            "((= counter 1) (define counter 2)) "
            "(else (define counter 3))))\n"
            "counter\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_while, r"=> 3", timeout=5.0)
        results.append(("Lisp while Loop (zero and multi-iteration, S2)", ok, log if not ok else ""))

        # 21f. Regression found while building 21e, predating S1/S2 entirely
        # -- not fixed until now because nothing exercised it: `define`
        # always writes into the *global_env* variable itself (reassigning
        # it to a new pointer), but begin/let/let*/cond/lambda-body all
        # capture `env` once and reuse that snapshot for the rest of their
        # own sequence, so a `define` earlier in the same body used to be
        # invisible to a later form in it -- `(begin (define zzz 1) zzz)`
        # reported "Unbound symbol: zzz" before this fix. refresh_global_tail()
        # closes it for every affected form; this is one assertion per form,
        # not just `while`, since all of them shared the one root cause.
        cmd_stale_env = (
            "lisp\n"
            "(begin (define s21f_a 1) s21f_a)\n"
            "(let ((q 1)) (define s21f_b 5) s21f_b)\n"
            "(let* ((q 1)) (define s21f_c 9) s21f_c)\n"
            "(define (s21f_f) (define s21f_d 11) s21f_d)\n"
            "(s21f_f)\n"
            "(cond (#t (define s21f_e 22) s21f_e))\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_stale_env, r"=> 22", timeout=6.0)
        # The final "=> 22" alone doesn't prove every earlier form also
        # worked -- a regression isolated to just one of begin/let/let*/
        # lambda-body wouldn't prevent this test from still reaching "=> 22"
        # at the end. Every one of the 5 forms above is set up to fail as
        # "Unbound symbol: s21f_*" specifically if its own fix regressed,
        # so also require that text never appears anywhere in the session.
        no_unbound = "Unbound symbol" not in log
        ok = ok and no_unbound
        results.append(("Lisp define Visible Later In Same begin/let/let*/lambda/cond Body (S2)",
                        ok, log if not ok else ""))

        # 21g. Mark-sweep collector (S3, plan/phase13_lisp_engine_extensions.md):
        # a session that keeps generating throwaway garbage across many
        # *separate* top-level commands must survive well past what a single
        # exhaustion could ever recover from on its own, by reclaiming each
        # command's garbage at the next command's safe point (lisp_repl()'s
        # loop -- see lisp_gc_safepoint() in user/lisp/include/lisp.h).
        # Empirically, 123 churn calls exhaust QEMU's 4096-node pool from a
        # fresh boot; 140 is comfortably past that boundary. Without a
        # collector this would exhaust once at ~i=123 and never recover --
        # every later command returns () for the rest of the session (this
        # was the actual pre-S3/pre-fix behavior, confirmed live during
        # development). With one, only the handful of commands that
        # themselves cross the exhaustion boundary fail (no safe point
        # *inside* one already-executing command -- documented, not a bug);
        # every other command, including the very last one here, must
        # still produce the correct answer.
        # 60 (not the larger margin used for S1's analogous TCO test) --
        # found live: a longer version of this test (140 iterations) passed
        # in isolation but destabilized an unrelated, pre-existing test
        # later in the same shared QEMU session (23, FAT32 cluster-chain
        # accounting) by growing this session's command-history file
        # enough to shift that test's timing. 60 still reliably exhausts
        # the shared session's remaining node-pool budget by this point
        # (confirmed empirically) without that side effect.
        n_churn = 60
        churn_cmds = "".join(f"(churn {i})\n" for i in range(n_churn))
        cmd_gc = (
            "lisp\n"
            "(define (churn n) (+ (+ n 1) (+ n 2) (+ n 3) (+ n 4) (+ n 5)))\n"
            + churn_cmds
            + "exit"
        )
        ok, log = session.send_and_expect(cmd_gc, rf"=> {n_churn * 5 + 10}", timeout=15.0)
        exhausted_at_least_once = "Node pool exhausted" in log
        ok = ok and exhausted_at_least_once
        results.append(("Lisp Mark-Sweep Collector Reclaims Garbage Across Top-Level Forms (S3)",
                        ok, log if not ok else ""))

        # 21h. S4 (plan/phase13_lisp_engine_extensions.md): standard library,
        # as C primitives. One test per category from the plan, chained --
        # each `and`s its own check into the running total so a single
        # failure anywhere is visible without 30 separate round trips.
        # Each chain below ends with a command whose result cannot also
        # match earlier in the same chain -- send_and_expect() returns on
        # the *first* match of its pattern anywhere in the accumulated
        # output, so an ambiguous final pattern (e.g. "=> #t" when an
        # earlier line in the same chain also prints "=> #t") would return
        # before the later commands even finish executing, silently
        # truncating the `log` this test's own count-based assertions
        # then check. A distinct sentinel value at the end of each chain
        # sidesteps this regardless of what the "real" commands produce.
        cmd_s4_compare = (
            "lisp\n"
            "(< 1 2 3)\n"
            "(< 1 3 2)\n"
            "(> 3 2 1)\n"
            "(<= 1 1 2)\n"
            "(>= 2 2 1)\n"
            "(/= 1 2 3)\n"
            "(/= 1 2 1)\n"
            "(= 5 5 5)\n"
            "12345\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_compare, r"=> 12345", timeout=6.0)
        # Every line above except (< 1 3 2) and (/= 1 2 1) must be #t --
        # count rather than pattern-match each individually, so a single
        # wrong chained-comparison result can't hide behind the others.
        compare_correct = log.count("=> #t") == 6 and log.count("=> #f") == 2
        ok = ok and compare_correct
        results.append(("Lisp Comparison Operators <,>,<=,>=,/= Chained N-ary (S4)", ok, log if not ok else ""))

        cmd_s4_math = (
            "lisp\n"
            "(/ 20 2 5)\n"
            "(/ 5 0)\n"
            "(quotient 7 2)\n"
            "(remainder -7 2)\n"
            "(modulo -7 2)\n"
            "(abs -5)\n"
            "(min 3 1 2)\n"
            "(max 3 1 2)\n"
            "12345\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_math, r"=> 12345", timeout=6.0)
        # Division by zero must degrade to nil, not trap UBSan (this build
        # runs with -fsanitize=undefined -fno-sanitize-recover=all) or hang.
        div_by_zero_safe = "=> ()" in log
        ok = ok and div_by_zero_safe
        results.append(("Lisp Integer Math /,quotient,remainder,modulo,abs,min,max (S4)",
                        ok, log if not ok else ""))

        cmd_s4_pred = (
            "lisp\n"
            "(null? (list))\n"
            "(pair? (cons 1 2))\n"
            "(symbol? 'x)\n"
            "(string? \"x\")\n"
            "(integer? 5)\n"
            "(procedure? car)\n"
            "(zero? 0)\n"
            "(boolean? #t)\n"
            "(boolean? 5)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_pred, r"=> #f", timeout=6.0)
        no_false_positive = log.count("=> #f") == 1  # only the final (boolean? 5) should be #f
        ok = ok and no_false_positive
        results.append(("Lisp Predicates null?,pair?,symbol?,string?,integer?,procedure?,zero?,boolean? (S4)",
                        ok, log if not ok else ""))

        cmd_s4_lists = (
            "lisp\n"
            "(cons 1 2)\n"
            "(car (list 1 2 3))\n"
            "(cdr (list 1 2 3))\n"
            "(length (list 1 2 3 4))\n"
            "(append (list 1 2) (list 3 4))\n"
            "(reverse (list 1 2 3))\n"
            "(list-ref (list 10 20 30) 1)\n"
            "(nth (list 10 20 30) 2)\n"
            "(map (lambda (x) (* x x)) (list 1 2 3))\n"
            "(filter (lambda (x) (= x 2)) (list 1 2 3 2))\n"
            "(for-each (lambda (x) (display x)) (list 1 2 3))\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_lists, r"123=> \(\)", timeout=6.0)
        # (cons 1 2) must print as the dotted pair "(1 . 2)" -- the printer
        # previously had no way to show an improper pair's cdr at all (every
        # pair built anywhere else was always nil-terminated until `cons`
        # existed to build one directly), silently printing "(1)" instead.
        dotted_pair_shown = "(1 . 2)" in log
        map_correct = "(1 4 9)" in log
        filter_correct = "(2 2)" in log
        ok = ok and dotted_pair_shown and map_correct and filter_correct
        results.append(("Lisp List Processing cons,car,cdr,length,append,reverse,list-ref,map,filter,for-each (S4)",
                        ok, log if not ok else ""))

        cmd_s4_strings = (
            "lisp\n"
            "(string-append \"foo\" \"bar\" \"baz\")\n"
            "(string-length \"hello\")\n"
            "(substring \"hello world\" 6 11)\n"
            "(string->number \"42\")\n"
            "(string->number \"-7\")\n"
            "(number->string -42)\n"
            "(string=? \"abc\" \"abc\")\n"
            "(string=? \"abc\" \"xyz\")\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_strings, r"=> #f", timeout=6.0)
        strings_correct = ('"foobarbaz"' in log and '"world"' in log and
                            "=> 42" in log and "=> -7" in log and '"-42"' in log)
        ok = ok and strings_correct
        results.append(("Lisp String Processing string-append,length,substring,string<->number,string=? (S4)",
                        ok, log if not ok else ""))

        cmd_s4_apply_eval = (
            "lisp\n"
            "(apply + (list 1 2 3))\n"
            "(apply + 1 2 (list 3 4))\n"
            "(eval (list (quote +) 1 2))\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_apply_eval, r"=> 3", timeout=6.0)
        apply_correct = "=> 6" in log and "=> 10" in log
        ok = ok and apply_correct
        results.append(("Lisp apply/eval Procedure Invocation (S4)", ok, log if not ok else ""))

        # 21i. Reader fix, found while testing S4's math primitives on
        # negative numbers: a leading sign immediately followed by a digit
        # (-5, +3) was never recognized as a number literal at all -- the
        # sign-handling code for it has existed in the digit-first parsing
        # branch all along, but the *gate* deciding whether to even enter
        # that branch only checked for a leading digit, so "-5" was always
        # read as the symbol "-5" instead, predating S1-S4 entirely. Must
        # not regress the bare `-`/`+` primitives or peculiar identifiers
        # like `->foo`, which still need to read as symbols.
        cmd_s4_signed_literals = (
            "lisp\n"
            "(- 10 3)\n"
            "(+ 10 3)\n"
            "(define ->foo 99)\n"
            "->foo\n"
            "(- -5 -3)\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_s4_signed_literals, r"=> -2", timeout=6.0)
        signed_literals_correct = "=> 7" in log and "=> 13" in log and "=> 99" in log
        ok = ok and signed_literals_correct
        results.append(("Lisp Signed Number Literals -5/+3 Without Breaking -/->foo (S4)",
                        ok, log if not ok else ""))

        # 21j. Named let (S5, plan/phase13_lisp_engine_extensions.md):
        # (let name ((v init)...) body...), distinguished from plain `let`
        # by a symbol instead of a bindings list as the first argument.
        # Desugars to a self-referential closure applied via the same
        # tail-position machinery plain lambda application uses, so it
        # gets S1's TCO for free -- 60 iterations (the same modest margin
        # used for S1's analogous TCO test, chosen so this doesn't destabilize
        # an unrelated later test in the same shared session the way an
        # earlier, longer version of a different S3 test once did) is
        # comfortably past the ~20-30-iteration ceiling this shape had
        # before TCO existed. Plain `let`/`let*` are checked alongside to
        # confirm named-let's new first-argument dispatch didn't disturb
        # either.
        cmd_named_let = (
            "lisp\n"
            "(let loop ((i 0) (acc 0)) (if (= i 5) acc (loop (+ i 1) (+ acc i))))\n"
            "(let ((a 1) (b 2)) (+ a b))\n"
            "(let* ((x 1) (y (+ x 1))) y)\n"
            "(let cnt ((i 0)) (if (= i 60) i (cnt (+ i 1))))\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_named_let, r"=> 60", timeout=8.0)
        named_let_correct = "=> 10" in log and "=> 3" in log and "=> 2" in log
        ok = ok and named_let_correct
        results.append(("Lisp Named let Loop With TCO (S5)", ok, log if not ok else ""))


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
        # Ten runaway recursions rather than one -- historically because pool
        # size is a *per target* constant (RP2350's 512-768 nodes exhausted on
        # the first, these builds' 4096 needed about eight) and because each
        # individual call used to be capped by LISP_MAX_EVAL_DEPTH long before
        # it could allocate enough to exhaust 4096 nodes by itself. Since S1
        # (plan/phase13_lisp_engine_extensions.md) added tail-call
        # optimization, `(loop (+ n 1))` -- a tail call -- no longer costs
        # eval_depth at all, so a *single* call now runs until it exhausts the
        # pool on its own; the repeated calls are redundant but harmless
        # (each subsequent one degrades to nil immediately, per the abort
        # check added alongside TCO). Left as 8 rather than trimmed to 1,
        # since this is not a case that benefits from being minimal.
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
    virtio-serial chardev backed by a unix socket, connects host/p9lib's
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
                    client = p9lib.connect_unix(sock_path, timeout=2.0)
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


def test_node_identity(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """The node's own name and MAC (kernel/identity.c), as preparation for two
    boards on one segment.

    Phase 18 shipped a *constant* MAC inside the W5500 driver, so any two
    boards would have collided -- and nothing noticed, because nothing ever
    had two. This asserts the properties that failure would have violated:
    the address is locally administered (bit 1 of the first octet), carries
    the 02:4c:47 signature rather than a squatted OUI, is derived rather than
    fixed, and the name is the 9P uname the far end's key store is indexed by.

    The derived values themselves are deliberately **not** asserted: they hash
    the build host's name, so a literal here would pass on one machine and
    fail on every other."""
    import shutil
    name = "Node Identity: Derived Name, Locally-Administered MAC, 9P uname (R3)"
    arch_img = img_path.with_name(f"test_{arch_name}_node_sd.img")
    shutil.copyfile(img_path, arch_img)

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start()
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        ok, log = session.send_and_expect("cat /proc/node", r"mac source:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer: {log[-400:]}")

        # No trailing `$`: this is raw terminal output, so every line ends "\r\n"
        # and an anchored end-of-line never matches.
        m = re.search(r"^name: ([\w.-]+)", log, re.MULTILINE)
        if not m:
            return (name, False, f"no derived name in /proc/node:\n{log[-400:]}")
        node = m.group(1)
        if not node.startswith(arch_name.replace("rv", "rv")) and "-" not in node:
            return (name, False, f"the name carries no persona or suffix: {node!r}")
        if not re.search(r"^name source: derived", log, re.MULTILINE):
            return (name, False, "the name is not derived on a board that pins none")

        mm = re.search(r"^mac: ([0-9a-f:]{17})", log, re.MULTILINE)
        if not mm:
            return (name, False, f"no MAC in /proc/node:\n{log[-400:]}")
        mac = [int(b, 16) for b in mm.group(1).split(":")]
        if mac[0] & 0x01:
            return (name, False, f"{mm.group(1)} is a group address, not a unicast one")
        if not mac[0] & 0x02:
            return (name, False, f"{mm.group(1)} is not marked locally administered")
        if mac[0:3] != [0x02, 0x4C, 0x47]:
            return (name, False, f"{mm.group(1)} does not carry the 02:4c:47 signature")
        if mac[3:6] == [0, 0, 0]:
            return (name, False, "the derived suffix is all zeroes")

        if not re.search(rf"^9P uname: {re.escape(node)}", log, re.MULTILINE):
            return (name, False, "the 9P uname is not the node name")

        # A rename must take, and must NOT move the MAC: an ARP cache full of
        # a name change is a bad trade.
        ok, log = session.send_and_expect(
            'lisp\n(net-identity "renamed-01")\nexit',
            r"\[Node\] renamed-01 \(set at runtime\), mac " + re.escape(mm.group(1)),
            timeout=6.0)
        if not ok:
            return (name, False, f"rename did not take, or moved the MAC: {log[-500:]}")

        ok, log = session.send_and_expect(
            'lisp\n(net-identity "not a name!")\nexit', r"is not a usable name", timeout=6.0)
        if not ok:
            return (name, False, f"an unusable name was accepted: {log[-400:]}")

        return (name, True, f"{node}, mac {mm.group(1)}, uname matches, rename kept the MAC")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_identity_store_provisioning(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """I2, plan/phase21_identity_and_authentication.md: the identity store's
    resolution ladder (§4), against QEMU's second virtio-blk device
    (drivers/virtio_blk_id.c) as the honest analogue of a board with no
    silicon to bind to (§3.1).

    Two guests, same kernel: one boots with a disk tools/provision.py wrote
    a record onto, one boots with none. The first must report the
    provisioned name and uid with `record` as the source of both; the
    second must fall back to the derived name exactly as it always has,
    with no uid at all -- proving I2 does not change a node that was never
    given a second drive, which is every node the existing suite already
    boots."""
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
    import provision

    name = "Identity Store: Provisioned Record Resolves Over Derived Identity (I2)"
    arch_img = img_path.with_name(f"test_{arch_name}_idstore_sd.img")
    shutil.copyfile(img_path, arch_img)

    prov_name = "prov-test-node"
    prov_uid = bytes.fromhex("aabbccdd11223344")
    id_img = img_path.with_name(f"test_{arch_name}_idstore_id.img")
    # A device key too, because the host is the only place a key for a board
    # with no console can come from -- and a provisioner whose FIELD_DEVKEY
    # the kernel does not read the same way produces a board nothing can
    # authenticate to, which presents as a network fault rather than as a
    # format disagreement. `identity` prints the fingerprint and never the
    # key, so the fingerprint is what this compares.
    prov_key = bytes(range(32))
    prov_fp = hashlib.sha256(prov_key).hexdigest()[:16]
    record = provision.build_record([
        (provision.FIELD_UID, prov_uid),
        (provision.FIELD_NAME, prov_name.encode("ascii")),
        (provision.FIELD_DEVKEY, prov_key),
    ])
    id_img.write_bytes(record)

    # 1. Provisioned: the record wins over the derived identity, and says so.
    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(identity_img_path=id_img)
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"provisioned guest did not reach the shell: {log[-400:]}")

        ok, log = session.send_and_expect("cat /proc/node", r"uid source:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer: {log[-400:]}")

        # No trailing `$`: this is raw terminal output, so every line ends
        # "\r\n" and an anchored end-of-line never matches (test_node_identity
        # above hits the same thing).
        if not re.search(rf"^name: {re.escape(prov_name)}\b", log, re.MULTILINE):
            return (name, False, f"provisioned name not reported:\n{log[-500:]}")
        if not re.search(r"^name source: record\b", log, re.MULTILINE):
            return (name, False, f"name source is not 'record':\n{log[-500:]}")
        if not re.search(rf"^uid: {prov_uid.hex()}\b", log, re.MULTILINE):
            return (name, False, f"provisioned uid not reported:\n{log[-500:]}")
        if not re.search(r"^uid source: record\b", log, re.MULTILINE):
            return (name, False, f"uid source is not 'record':\n{log[-500:]}")

        # The key the host wrote is the key the board holds, compared by
        # fingerprint -- and the key itself must not appear anywhere in the
        # output, which is the other half of the promise.
        if not re.search(rf"key fingerprint: {prov_fp}\b", log):
            return (name, False,
                    f"device key from the host record not reported (want {prov_fp}):\n{log[-500:]}")
        if prov_key.hex() in log.lower():
            return (name, False, "the device key itself was printed")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()

    # 2. Unprovisioned: no second drive at all -- the same fallback every
    # other test in this suite already relies on, unaffected by I2 existing.
    session2 = QemuSession(elf_path, arch_img, arch_name)
    try:
        session2.start()
        ok, log = session2.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"unprovisioned guest did not reach the shell: {log[-400:]}")

        ok, log = session2.send_and_expect("cat /proc/node", r"uid source:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer: {log[-400:]}")

        if not re.search(r"^name source: derived\b", log, re.MULTILINE):
            return (name, False, f"name source is not 'derived' with no second drive:\n{log[-500:]}")
        if not re.search(r"^uid: none\b", log, re.MULTILINE):
            return (name, False, f"a uid was reported with no identity disk attached:\n{log[-500:]}")

        return (name, True, f"provisioned: name={prov_name!r} uid={prov_uid.hex()} (source: record); "
                             f"unprovisioned: derived name, no uid")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session2.close()


def test_grants_in_record(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """I5 + the record backend: a grant the host provisioned, honoured by the
    board, with no SD card involved.

    This is the case that made the record backend necessary. Grants lived only
    at /sd0/system/etc/auth/keys, so a board with no card -- the clock persona
    is one -- had nowhere to keep one, refused every authenticated attach
    forever, and could not be authorised by any means. `p9key` is
    this-boot-only and /flash0 is read-only and byte-identical across boards.

    And it is the case that catches the bug the backend first shipped with.
    Redirecting the *storage* left p9_grants_find() -- the reader that
    actually authorises an attach -- still reading the empty file, while
    `peers` listed the grant and `p9auth` reported keys configured. Every
    surface a person would check said yes and every attach was refused. So
    this asserts the board's own view (peers, p9auth) *and* that a grant
    written by the host tool arrives intact, rather than trusting either
    alone.
    """
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
    import provision

    name = "Grants In The Identity Record, Provisioned By The Host (I5)"
    arch_img = img_path.with_name(f"test_{arch_name}_grantrec_sd.img")
    shutil.copyfile(img_path, arch_img)

    key = bytes(range(16))
    fp = hashlib.sha256(key).hexdigest()[:16]
    blob = f"alice {key.hex()} /sd0 ro\n".encode("ascii")
    id_img = img_path.with_name(f"test_{arch_name}_grantrec_id.img")
    id_img.write_bytes(provision.build_record([
        (provision.FIELD_UID, bytes.fromhex("5566778899aabbcc")),
        (provision.FIELD_NAME, b"grantrec"),
        (provision.FIELD_GRANTS, blob),
    ]))

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(identity_img_path=id_img)
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        # 1. The board reads back what the host wrote -- name, scope and mode,
        #    with the key shown only as a fingerprint.
        ok, log = session.send_and_expect("peers", r"fingerprint", timeout=6.0)
        if not ok:
            return (name, False, f"`peers` did not answer: {log[-400:]}")
        if not re.search(rf"^alice\s+{fp}\s+/sd0\s+ro", log, re.MULTILINE):
            return (name, False,
                    f"the provisioned grant is not reported (want alice/{fp}//sd0/ro):\n{log[-500:]}")
        if key.hex() in log.lower():
            return (name, False, "the granted key itself was printed")

        # 2. And the auth gate agrees. This is the assertion that separates a
        #    grant that merely *lists* from one that would actually admit a
        #    peer: p9auth reports what p9_auth_have_keys() decides, which is
        #    the gate every Tauth passes through.
        ok, log = session.send_and_expect("p9auth", r"Keys configured", timeout=6.0)
        if not ok:
            return (name, False, f"`p9auth` did not answer: {log[-400:]}")
        if "Keys configured: yes" not in log:
            return (name, False,
                    f"a record-backed grant does not satisfy the auth gate:\n{log[-400:]}")

        # 3. Adding one on the device lands in the same place and is visible
        #    beside it -- the write path and the read path agreeing, which is
        #    what the first version of this backend got wrong.
        ok, log = session.send_and_expect(
            "peers add bob 0f0e0d0c0b0a09080706050403020100 / rw", r"granted", timeout=6.0)
        if not ok:
            return (name, False, f"`peers add` failed: {log[-400:]}")
        ok, log = session.send_and_expect("peers", r"fingerprint", timeout=6.0)
        if not ok:
            return (name, False, f"`peers` did not answer after add: {log[-400:]}")
        if not re.search(r"^alice\s", log, re.MULTILINE):
            return (name, False, f"adding a grant lost the provisioned one:\n{log[-500:]}")
        if not re.search(r"^bob\s", log, re.MULTILINE):
            return (name, False, f"the added grant is not listed:\n{log[-500:]}")

        return (name, True, "host-written grant honoured; device add joins it, neither lost")
    except Exception as e:  # noqa: BLE001
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()
        arch_img.unlink(missing_ok=True)
        id_img.unlink(missing_ok=True)


def test_identity_toolset(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """I3, plan/phase21_identity_and_authentication.md: the `identity`
    command family against a real (fresh) identity disk -- I3's own verify
    list, checked in order: `identity provision` refuses a populated store
    without `--force`; a rename does not move the MAC; and no command ever
    prints a key, only its fingerprint.

    The 9P secrecy guard (§4: "the identity store must join
    [p9_auth_path_is_secret()'s guard]") is not re-checked here -- it is
    pure string logic with no device and no server involved, already
    exercised by idstoreselftest/p9authselftest, which this suite runs
    regardless of whether any identity disk is attached."""
    import shutil
    name = "Identity Toolset: Provision-Refuses, Rename-Keeps-MAC, No-Key-Printed (I3)"
    arch_img = img_path.with_name(f"test_{arch_name}_idtool_sd.img")
    shutil.copyfile(img_path, arch_img)
    id_img = img_path.with_name(f"test_{arch_name}_idtool_id.img")
    id_img.write_bytes(b"\x00" * 4096)  # unprovisioned

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(identity_img_path=id_img)
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        # The MAC before anything touches the identity store, to compare
        # against after a persisted rename.
        ok, log = session.send_and_expect("cat /proc/node\n", r"mac source:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer: {log[-400:]}")
        mm = re.search(r"^mac: ([0-9a-f:]{17})", log, re.MULTILINE)
        if not mm:
            return (name, False, f"no MAC in /proc/node:\n{log[-400:]}")
        mac_before = mm.group(1)

        # `identity provision` on a fresh store: succeeds, uid/name become
        # 'record'-sourced.
        ok, log = session.send_and_expect("identity provision\n", r"key fingerprint:", timeout=6.0)
        if not ok or "identity: provisioned" not in log:
            return (name, False, f"first provision did not succeed:\n{log[-500:]}")

        # A second `identity provision`, no --force: refused, and the
        # refusal must not have touched anything (checked by uid staying
        # put below, via the rename step's own report).
        ok, log = session.send_and_expect("identity provision\n", r"already provisioned", timeout=6.0)
        if not ok:
            return (name, False, f"a populated store was not refused without --force:\n{log[-500:]}")

        # A rename: persists, and does not move the MAC.
        ok, log = session.send_and_expect("identity name toolset-test\n", r"renamed to", timeout=6.0)
        if not ok:
            return (name, False, f"rename did not report success:\n{log[-500:]}")

        ok, log = session.send_and_expect("cat /proc/node\n", r"mac source:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer after rename: {log[-400:]}")
        if not re.search(r"^name: toolset-test\b", log, re.MULTILINE):
            return (name, False, f"the persisted rename did not take:\n{log[-500:]}")
        if not re.search(r"^name source: record\b", log, re.MULTILINE):
            return (name, False, f"name source is not 'record' after a persisted rename:\n{log[-500:]}")
        mm2 = re.search(r"^mac: ([0-9a-f:]{17})", log, re.MULTILINE)
        if not mm2 or mm2.group(1) != mac_before:
            return (name, False, f"the rename moved the MAC: {mac_before} -> {mm2.group(1) if mm2 else '?'}")

        # A key, installed by hex: the response shows a fingerprint and
        # never the raw key. The raw hex must appear in this command's own
        # log exactly once -- the shell's echo of what was typed -- and
        # nowhere else, i.e. never in a response line.
        raw_key = "13579bdf02468ace13579bdf02468ace"
        ok, log = session.send_and_expect(f"identity key {raw_key}\n", r"key fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"key install did not report success:\n{log[-500:]}")
        if log.count(raw_key) != 1:
            return (name, False, f"the raw key appeared {log.count(raw_key)} times, expected 1 (echo only):\n{log[-500:]}")
        fp = re.search(r"^key fingerprint: ([0-9a-f]{16})", log, re.MULTILINE)
        if not fp:
            return (name, False, f"no fingerprint reported for the installed key:\n{log[-500:]}")

        # And the report command itself never prints the key either.
        ok, log = session.send_and_expect("identity\n", r"key fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"report did not answer: {log[-400:]}")
        if raw_key in log:
            return (name, False, f"the raw key leaked into the report:\n{log[-500:]}")
        if f"key fingerprint: {fp.group(1)}" not in log:
            return (name, False, f"the report's fingerprint does not match the installed key's:\n{log[-500:]}")

        return (name, True, f"provision refused without --force, rename kept mac {mac_before}, "
                             f"key fingerprint {fp.group(1)} shown and the raw key never printed")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_network_autoconfig(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """Network autoconfig from the identity record: a board with a stored
    address comes up on the network with no boot script involved.

    That last clause is the whole point, so the test asserts it rather than
    just checking a getter. Since I7a the filesystem image is byte-identical
    on every board, and an address in init.lisp would have made it per-board
    again; the address lives in the record instead, and net_task_start()
    applies it before the stack task exists. Here nothing is typed before the
    check -- no (net-config ...), no netcfg -- so a configured interface can
    only have come from the record.

    Three parts: the host-written field is read back (proving
    tools/provision.py and kernel/idstore.c agree on this field, the same way
    I2 and I6 do for theirs), the stack actually applied it at boot, and the
    on-device `netcfg` can both replace and remove it."""
    import shutil
    import sys as _sys
    _sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
    import provision

    name = "Network Autoconfig From The Identity Record, Applied Before Any Script"
    arch_img = img_path.with_name(f"test_{arch_name}_netcfg_sd.img")
    shutil.copyfile(img_path, arch_img)

    ip, mask, gw = "10.0.9.42", "255.255.255.0", "10.0.9.1"
    quads = bytes(int(o) for part in (ip, mask, gw) for o in part.split("."))
    id_img = img_path.with_name(f"test_{arch_name}_netcfg_id.img")
    id_img.write_bytes(provision.build_record([
        (provision.FIELD_UID, bytes(range(8))),
        (provision.FIELD_NAME, b"netcfg-node"),
        (provision.FIELD_IPV4, quads),
    ]))

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        # A netif has to exist for the stack to start at all; this one never
        # carries a packet, which is deliberate -- what is under test is the
        # configuration path, not the wire.
        session.start(identity_img_path=id_img, extra_qemu_args=[
            "-netdev", "user,id=n0",
            "-device", "virtio-net-device,netdev=n0",
        ])
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        # 1. The stack applied it at boot. Nothing has been typed yet.
        ok, log = session.send_and_expect("net\n", r"rx \d+ frames", timeout=6.0)
        if not ok:
            return (name, False, f"`net` did not answer: {log[-400:]}")
        if "addr unconfigured" in log:
            return (name, False,
                     f"the stored address was not applied at boot -- the interface came up "
                     f"unconfigured:\n{log[-600:]}")
        if not re.search(rf"addr {re.escape(ip)}/{re.escape(mask)} gw {re.escape(gw)}", log):
            return (name, False, f"the interface is configured, but not from the record:\n{log[-600:]}")

        # 2. The record round-trips through the device's own reports.
        ok, log = session.send_and_expect("netcfg\n", r"^address:", timeout=6.0)
        if not ok:
            return (name, False, f"`netcfg` did not answer: {log[-400:]}")
        if not re.search(rf"^address: {re.escape(ip)}\b", log, re.MULTILINE):
            return (name, False, f"netcfg does not report the stored address:\n{log[-500:]}")

        ok, log = session.send_and_expect("cat /proc/node\n", r"^ipv4:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer: {log[-400:]}")
        if not re.search(rf"^ipv4: {re.escape(ip)}/{re.escape(mask)} gw {re.escape(gw)}",
                          log, re.MULTILINE):
            return (name, False, f"/proc/node does not show the stored address:\n{log[-500:]}")

        # 3. Replacing it, and the refusals that keep an unusable one out.
        ok, log = session.send_and_expect("netcfg 10.0.9.77 255.255.0.0\n", r"^address:", timeout=6.0)
        if not ok or not re.search(r"^address: 10\.0\.9\.77\b", log, re.MULTILINE):
            return (name, False, f"netcfg did not store a replacement:\n{log[-500:]}")
        if not re.search(r"^gateway: none", log, re.MULTILINE):
            return (name, False, f"an omitted gateway should read as none:\n{log[-500:]}")

        ok, log = session.send_and_expect("netcfg 0.0.0.0 255.255.255.0\n", r"netcfg:", timeout=6.0)
        if not ok or "0.0.0.0" not in log or re.search(r"^address: 0\.0\.0\.0", log, re.MULTILINE):
            return (name, False, f"netcfg accepted 0.0.0.0 as an address:\n{log[-500:]}")
        ok, log = session.send_and_expect("netcfg 10.0.9.5 not.an.ip.addr\n", r"netcfg:", timeout=6.0)
        if not ok or "dotted quad" not in log:
            return (name, False, f"netcfg accepted a malformed mask:\n{log[-500:]}")

        # 4. Clearing really removes the field rather than zeroing it.
        ok, log = session.send_and_expect("netcfg clear\n", r"netcfg: cleared", timeout=6.0)
        if not ok:
            return (name, False, f"netcfg clear did not answer: {log[-400:]}")
        ok, log = session.send_and_expect("netcfg\n", r"address:", timeout=6.0)
        if not ok or "none stored" not in log:
            return (name, False, f"the address survived a clear:\n{log[-500:]}")
        ok, log = session.send_and_expect("cat /proc/node\n", r"^ipv4:", timeout=6.0)
        if not ok or not re.search(r"^ipv4: none", log, re.MULTILINE):
            return (name, False, f"/proc/node still shows an address after clear:\n{log[-500:]}")

        return (name, True, "")
    except Exception as e:
        return (name, False, str(e))
    finally:
        session.close()
        arch_img.unlink(missing_ok=True)
        id_img.unlink(missing_ok=True)


def test_wlan_credential_roundtrip(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """I6, plan/phase21_identity_and_authentication.md §5.3: "on the device,
    the credential round-trips and is never printed." (The host-side half of
    I6's verify -- "a known passphrase and SSID derive the PSK the standard
    gives" -- is test_host_wpa2_psk_derivation() below, no QEMU involved.)

    Two halves. First: a disk `tools/provision.py` wrote a WLAN credential
    onto is attached at boot, and the device's own report shows the same
    SSID and a PSK fingerprint that matches a fingerprint computed
    independently here on the host from the same PSK bytes -- proving the
    two implementations of the record format (kernel/idstore.c and
    provision.py's own from-scratch writer) agree on this field the same
    way I2's test already proved they agree on uid/name. Second: the
    on-device `wlan <ssid> <psk-hex>` command installs a *different*
    credential at runtime, and the raw PSK hex appears exactly once in
    that exchange -- the shell's own echo -- never in any response,
    matching I3's identity-key test and I5's peers-add test."""
    import hashlib
    import shutil
    import sys as _sys
    _sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
    import provision

    name = "WLAN Credential: Host<->Device Format Agreement, Round Trip, Never Printed (I6)"
    arch_img = img_path.with_name(f"test_{arch_name}_wlan_sd.img")
    shutil.copyfile(img_path, arch_img)

    prov_ssid = "provisioned-net"
    prov_psk = provision.derive_wpa2_psk(prov_ssid, "a provisioned passphrase")
    prov_fp = hashlib.sha256(prov_psk).digest()[:8].hex()
    id_img = img_path.with_name(f"test_{arch_name}_wlan_id.img")
    id_img.write_bytes(provision.build_record([
        (provision.FIELD_WLAN_SSID, prov_ssid.encode("utf-8")),
        (provision.FIELD_WLAN_PSK, prov_psk),
    ]))

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(identity_img_path=id_img)
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        # 1. The provisioned credential, read back exactly as provision.py wrote it.
        ok, log = session.send_and_expect("wlan\n", r"psk fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"wlan report did not answer: {log[-400:]}")
        if not re.search(rf"^ssid: {re.escape(prov_ssid)}\b", log, re.MULTILINE):
            return (name, False, f"the provisioned ssid was not reported:\n{log[-500:]}")
        if not re.search(rf"^psk fingerprint: {prov_fp}\b", log, re.MULTILINE):
            return (name, False, f"the provisioned psk's fingerprint does not match "
                                 f"the host-computed one ({prov_fp}):\n{log[-500:]}")

        ok, log = session.send_and_expect("cat /proc/node\n", r"wlan psk fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"/proc/node did not answer: {log[-400:]}")
        if not re.search(rf"^wlan ssid: {re.escape(prov_ssid)}\b", log, re.MULTILINE):
            return (name, False, f"/proc/node does not show the provisioned ssid:\n{log[-500:]}")
        if not re.search(rf"^wlan psk fingerprint: {prov_fp}\b", log, re.MULTILINE):
            return (name, False, f"/proc/node's fingerprint does not match:\n{log[-500:]}")

        # 2. A fresh credential installed at runtime: round-trips, and the
        # raw hex appears exactly once (the shell's own echo of the command).
        new_ssid = "runtime-net"
        # Built, not hand-typed: a 32-byte pattern is easy to get one hex
        # digit short by hand, and a wrong-length string would fail this
        # test for the wrong reason (its own typo, not the code under test).
        new_psk_hex = bytes(range(0x50, 0x70)).hex()
        # Waits for the *last* line the command produces, not the first.
        #
        # This expected "credential installed" and then asserted on the psk
        # fingerprint, which the shell prints afterwards (cmd_wlan calls
        # wlan_print_report() below its confirmation). send_and_expect()
        # returns on the first read in which its pattern matches, so anything
        # printed after that is in the log only if it happened to land in the
        # same 20 ms chunk. It usually did; under host load it did not, and the
        # test failed roughly one run in four with a log ending exactly at
        # "credential installed" -- which reads as a firmware fault and is a
        # test racing itself. Found 2026-09-04 after it had been dismissed as
        # an unexplained flake several times across a session.
        new_fp = hashlib.sha256(bytes.fromhex(new_psk_hex)).digest()[:8].hex()
        ok, log = session.send_and_expect(f"wlan {new_ssid} {new_psk_hex}",
                                          rf"psk fingerprint: {new_fp}", timeout=6.0)
        if not ok:
            return (name, False, f"installing a new credential failed: {log[-500:]}")
        if "credential installed" not in log:
            return (name, False, f"no confirmation of the install:\n{log[-500:]}")
        if log.count(new_psk_hex) != 1:
            return (name, False, f"the raw psk appeared {log.count(new_psk_hex)} times, expected 1 (echo only):\n{log[-500:]}")
        if not re.search(rf"^psk fingerprint: {new_fp}\b", log, re.MULTILINE):
            return (name, False, f"the new credential's fingerprint does not match:\n{log[-500:]}")

        # And the old ssid/psk are gone -- this is a replacement (§5.3: "one
        # network"), not an addition.
        ok, log = session.send_and_expect("wlan\n", r"psk fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"wlan report did not answer after install: {log[-400:]}")
        if new_psk_hex in log:
            return (name, False, f"the raw psk leaked into the report:\n{log[-500:]}")
        if not re.search(rf"^ssid: {re.escape(new_ssid)}\b", log, re.MULTILINE):
            return (name, False, f"the report does not show the new ssid:\n{log[-500:]}")
        if re.search(rf"^ssid: {re.escape(prov_ssid)}\b", log, re.MULTILINE):
            return (name, False, f"the old provisioned ssid is still reported:\n{log[-500:]}")

        return (name, True, f"provisioned credential ({prov_ssid}, fp {prov_fp}) read back correctly; "
                            f"runtime install ({new_ssid}, fp {new_fp}) round-tripped, raw psk never printed")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_host_wpa2_psk_derivation() -> tuple[bool, str]:
    """I6's host-side verify point: "a known passphrase and SSID derive
    the PSK the standard gives." No QEMU -- tools/provision.py's
    derive_wpa2_psk() runs entirely on the host, so this is exactly
    test_host_fat32_image() above's shape (a plain host-side check), not
    a QEMU integration test."""
    import sys as _sys
    _sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
    import provision

    failures = provision._wpa2_selftest()
    return (failures == 0, "" if failures == 0 else f"{failures} check(s) failed -- see the printed detail above")


def test_netif_virtio_net(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """R1, plan/phase19_ip_stack_and_ethernet.md: the netif_t seam and its
    first implementation, verified against a packet-level peer.

    The guest gets a virtio-net device whose far end is a plain UDP socket
    (QEMU's `dgram` backend), so `tests/netpeer.py` sees every frame the guest
    transmits as one datagram and can inject any frame it likes. That is the
    whole point of choosing this backend over slirp: a peer that can only
    speak correct protocols cannot test an implementation of one, and R2/R3
    will need to send truncated headers, bad checksums and out-of-order
    segments.

    Four things are checked, in the order a bring-up asks them:
      1. the device is found and its MAC comes from config space, not from a
         fallback -- QEMU's default is 52:54:00:12:34:56
      2. `net txtest 3` puts three frames on the wire, and each is compared
         **byte for byte** against a locally rebuilt expectation rather than
         pattern-matched; a two-byte virtio header misjudgement (see
         drivers/virtio_net.c's feature-negotiation note) would shift every
         one of them and is exactly what this catches
      3. a frame injected by the peer is received and parsed correctly
      4. the interface counters agree with what the peer independently
         counted -- the assertion that makes the first three mean something
    """
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import netpeer

    name = "Netif Seam + VirtIO-Net Driver Against A Packet-Level Peer (R1)"
    arch_img = img_path.with_name(f"test_{arch_name}_netif_sd.img")
    shutil.copyfile(img_path, arch_img)

    peer = netpeer.NetPeer()
    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=peer.qemu_args())

        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        # 1. the driver found the device and read its MAC from config space
        # The line names the node first, then the interface: identity is what
        # an operator looking at two boards actually needs.
        ok, log = session.send_and_expect(
            "net", r"[\w.-]+: virtio-net, mac ([0-9a-f:]{17}), link up", timeout=5.0)
        if not ok:
            return (name, False, f"`net` did not report the interface: {log[-600:]}")
        m = re.search(r"virtio-net, mac ([0-9a-f:]{17})", log)
        mac_text = m.group(1)
        if mac_text != "52:54:00:12:34:56":
            return (name, False, f"MAC {mac_text} is not QEMU's default -- config space not read?")
        guest_mac = bytes(int(b, 16) for b in mac_text.split(":"))

        # 2. three frames out, byte-exact
        peer.clear()
        ok, log = session.send_and_expect("net txtest 3", r"net: 3/3 test frames sent", timeout=5.0)
        if not ok:
            return (name, False, f"txtest did not report three frames sent: {log[-600:]}")

        got = peer.wait_for(3, timeout=5.0)
        if len(got) != 3:
            return (name, False, f"peer received {len(got)} frames, expected 3")
        for i, frame in enumerate(got):
            want = netpeer.expected_test_frame(guest_mac, i)
            if frame != want:
                return (name, False,
                        f"frame {i} differs.\n  got  {frame.hex()}\n  want {want.hex()}")

        # 3. one frame in, parsed correctly
        peer_mac = b"\x02\x00\x00\x00\x00\x42"
        inbound = netpeer.eth_frame(netpeer.BROADCAST, peer_mac,
                                    netpeer.ETHERTYPE_TEST, b"HELLO-FROM-PEER")
        peer.send(inbound)
        # `net rxtest` reports what the *stack* did not claim (net/ip.h's raw
        # sink), so the frame is injected first and drained second -- polling
        # the interface directly would lose every race against `netsrv`.
        ok, log = session.send_and_expect(
            "net rxtest 1",
            r"net: rx 60 bytes, 02:00:00:00:00:42 -> ff:ff:ff:ff:ff:ff, type 0x88b5", timeout=8.0)
        if not ok:
            return (name, False, f"the injected frame was not received as sent: {log[-600:]}")

        # 4. the counters agree with what the peer counted
        ok, log = session.send_and_expect("net", r"rx 1 frames, 60 bytes, 0 dropped", timeout=5.0)
        if not ok:
            return (name, False, f"rx counters disagree with the peer: {log[-600:]}")
        if not re.search(r"tx 3 frames, 180 bytes, 0 errors", log):
            return (name, False, f"tx counters disagree with the peer: {log[-600:]}")

        return (name, True, f"mac {mac_text}, 3 frames out byte-exact, 1 in, counters agree")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()
        peer.close()


def test_ntp_server(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """P6: the board answering as a time server.

    These targets have no radio, so the clock is never disciplined and the
    server can only answer "unsynchronised" -- which is the case most worth
    pinning down. §4's design principle is that a server advertising a
    confidence it cannot justify is worse than one admitting it has none,
    because clients believe stratum numbers; a target with nothing to be
    confident about is where that principle is checkable without hardware.

    So this asserts the honest-refusal path byte for byte: LI 3, stratum 16,
    and the origin timestamp echoed back -- the field a client uses to know the
    reply is its own, and getting it wrong makes every reply untrackable
    regardless of how good the time in it is.
    """
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import netpeer

    name = "NTP Server: Answers, And Admits It Is Unsynchronised (P6)"
    arch_img = img_path.with_name(f"test_{arch_name}_ntpsrv_sd.img")
    shutil.copyfile(img_path, arch_img)

    GUEST_IP = bytes([192, 168, 77, 2])
    PEER_IP = bytes([192, 168, 77, 1])
    PEER_MAC = b"\x02\x00\x00\x00\x00\x42"
    GUEST_MAC = bytes.fromhex("525400123456")

    peer = netpeer.NetPeer()
    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=peer.qemu_args())
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        ok, log = session.send_and_expect(
            'lisp\n(net-config "192.168.77.2" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.77\.2/255\.255\.255\.0", timeout=6.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")

        # A version 4 client request, with a transmit timestamp we can look for
        # coming back in the origin field.
        req = bytearray(48)
        req[0] = (0 << 6) | (4 << 3) | 3          # LI 0, VN 4, mode 3 = client
        req[2] = 6
        MARK = bytes.fromhex("DEADBEEFCAFEBABE")
        req[40:48] = MARK

        peer.clear()
        peer.send(netpeer.eth_frame(
            GUEST_MAC, PEER_MAC, netpeer.ETHERTYPE_IPV4,
            netpeer.ipv4_packet(PEER_IP, GUEST_IP, 17,
                                netpeer.udp_datagram(PEER_IP, GUEST_IP, 50123, 123, bytes(req)))))

        reply = None
        for f in peer.wait_for(1, timeout=6.0):
            try:
                _d, _s, etype, payload = netpeer.parse_eth(f)
                if etype != netpeer.ETHERTYPE_IPV4:
                    continue
                ip = netpeer.parse_ipv4(payload)
                if ip is None or ip["proto"] != 17:
                    continue
                dgram = ip["payload"]
                if len(dgram) < 8 or int.from_bytes(dgram[0:2], "big") != 123:
                    continue
                reply = dgram[8:]
            except Exception:
                continue
        if reply is None or len(reply) < 48:
            return (name, False, "no NTP reply from the guest on port 123")

        li = (reply[0] >> 6) & 0x3
        vn = (reply[0] >> 3) & 0x7
        mode = reply[0] & 0x7
        problems = []
        if mode != 4:
            problems.append(f"mode {mode}, expected 4 (server)")
        if vn != 4:
            problems.append(f"version {vn} echoed back, expected 4")
        if li != 3:
            problems.append(f"leap indicator {li}, expected 3 (unsynchronised)")
        if reply[1] != 16:
            problems.append(f"stratum {reply[1]}, expected 16 while unsynchronised")
        if reply[24:32] != MARK:
            problems.append("origin timestamp was not the request's transmit stamp")
        if reply[3] != (-20) & 0xFF:
            problems.append(f"precision {reply[3]:#x}, expected 0xec (2^-20 s)")
        if problems:
            return (name, False, "; ".join(problems))
        return (name, True, "")
    finally:
        session.close()
        peer.close()
        arch_img.unlink(missing_ok=True)


def test_ip_stack(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """R2, plan/phase19_ip_stack_and_ethernet.md: ARP, IPv4, ICMP and UDP,
    against a peer that can build anything.

    The four things that work are checked by their replies, byte for byte; the
    four things that must *not* work are checked by their counters, which is
    the whole reason /proc/net keeps them apart. "The network does not work"
    is not a diagnosis and a single drop total cannot become one -- phase 18
    spent days learning that, and this test is what the lesson buys.

    What is deliberately not asserted: timing. A localhost UDP socket says
    nothing useful about latency, and pretending otherwise would make this
    test flaky on a loaded machine for no information.
    """
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import netpeer

    name = "IP Stack: ARP, ICMP, UDP And Four Kinds Of Malformed (R2)"
    arch_img = img_path.with_name(f"test_{arch_name}_ipstack_sd.img")
    shutil.copyfile(img_path, arch_img)

    GUEST_IP = bytes([192, 168, 77, 2])
    PEER_IP = bytes([192, 168, 77, 1])
    PEER_MAC = b"\x02\x00\x00\x00\x00\x42"
    GUEST_MAC = bytes.fromhex("525400123456")

    peer = netpeer.NetPeer()
    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=peer.qemu_args())
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        ok, log = session.send_and_expect(
            'lisp\n(net-config "192.168.77.2" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.77\.2/255\.255\.255\.0", timeout=6.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")

        ok, log = session.send_and_expect("net udpecho 7", r"net: echoing UDP on port 7", timeout=5.0)
        if not ok:
            return (name, False, f"could not bind the echo port: {log[-500:]}")

        def ipv4_to_guest(proto: int, payload: bytes, **kw: object) -> bytes:
            return netpeer.eth_frame(
                GUEST_MAC, PEER_MAC, netpeer.ETHERTYPE_IPV4,
                netpeer.ipv4_packet(PEER_IP, GUEST_IP, proto, payload, **kw))  # type: ignore[arg-type]

        # --- 1. ARP: a request for our address must be answered ---
        peer.clear()
        peer.send(netpeer.eth_frame(
            netpeer.BROADCAST, PEER_MAC, netpeer.ETHERTYPE_ARP,
            netpeer.arp_packet(1, PEER_MAC, PEER_IP, bytes(6), GUEST_IP)))
        frames = peer.wait_for(1, timeout=5.0)
        if not frames:
            return (name, False, "no ARP reply")
        _dst, _src, etype, payload = netpeer.parse_eth(frames[0])
        if etype != netpeer.ETHERTYPE_ARP:
            return (name, False, f"expected ARP, got ethertype 0x{etype:04x}")
        arp = netpeer.parse_arp(payload)
        if arp["op"] != 2 or arp["sender_ip"] != GUEST_IP or arp["target_ip"] != PEER_IP:
            return (name, False, f"malformed ARP reply: {arp}")
        if arp["sender_mac"] != GUEST_MAC:
            return (name, False, f"ARP reply carries {netpeer.mac_str(arp['sender_mac'])}")

        # --- 2. ICMP: an echo request comes back with everything echoed ---
        peer.clear()
        body = b"ping-payload-0123456789"
        peer.send(ipv4_to_guest(netpeer.IP_PROTO_ICMP, netpeer.icmp_echo(0x1234, 1, body)))
        frames = peer.wait_for(1, timeout=5.0)
        if not frames:
            return (name, False, "no ICMP echo reply")
        _d, _s, _t, ip = netpeer.parse_eth(frames[0])
        icmp = ip[20:]
        if icmp[0] != 0 or icmp[1] != 0:
            return (name, False, f"expected echo reply, got type {icmp[0]} code {icmp[1]}")
        if netpeer.ip_checksum(icmp) != 0:
            return (name, False, "echo reply checksum is wrong")
        if icmp[4:8] != b"\x12\x34\x00\x01" or icmp[8:] != body:
            return (name, False, f"identifier/sequence/payload not echoed: {icmp[4:].hex()}")

        # --- 3. UDP: the echo service returns the same bytes ---
        peer.clear()
        msg = b"udp-hello"
        peer.send(ipv4_to_guest(netpeer.IP_PROTO_UDP,
                                netpeer.udp_datagram(PEER_IP, GUEST_IP, 4000, 7, msg)))
        frames = peer.wait_for(1, timeout=5.0)
        if not frames:
            return (name, False, "no UDP echo")
        _d, _s, _t, ip = netpeer.parse_eth(frames[0])
        udp = ip[20:]
        if int.from_bytes(udp[0:2], "big") != 7 or int.from_bytes(udp[2:4], "big") != 4000:
            return (name, False, f"UDP ports wrong: {udp[0:4].hex()}")
        if udp[8:8 + len(msg)] != msg:
            return (name, False, f"UDP payload not echoed: {udp[8:].hex()}")
        pseudo = GUEST_IP + PEER_IP + bytes([0, netpeer.IP_PROTO_UDP]) + udp[4:6]
        if netpeer.ip_checksum(pseudo + udp) != 0:
            return (name, False, "UDP checksum on the reply is wrong")

        # --- 4. UDP to a port nobody bound: ICMP 3/3, quoting the original ---
        peer.clear()
        closed = netpeer.udp_datagram(PEER_IP, GUEST_IP, 4000, 9999, b"nobody-home")
        peer.send(ipv4_to_guest(netpeer.IP_PROTO_UDP, closed))
        frames = peer.wait_for(1, timeout=5.0)
        if not frames:
            return (name, False, "no ICMP port-unreachable")
        _d, _s, _t, ip = netpeer.parse_eth(frames[0])
        icmp = ip[20:]
        if icmp[0] != 3 or icmp[1] != 3:
            return (name, False, f"expected ICMP 3/3, got {icmp[0]}/{icmp[1]}")
        # The quote must carry the original UDP ports, or the far end cannot
        # attribute the refusal to a socket.
        quoted_udp = icmp[8 + 20:8 + 20 + 8]
        if int.from_bytes(quoted_udp[2:4], "big") != 9999:
            return (name, False, f"the quoted datagram is not the one we sent: {icmp[8:].hex()}")

        # --- 5. four malformed frames, each counted as its own kind ---
        peer.clear()
        good_icmp = netpeer.icmp_echo(0x1234, 2, b"x" * 16)
        peer.send(ipv4_to_guest(netpeer.IP_PROTO_ICMP, good_icmp, bad_checksum=True))
        peer.send(ipv4_to_guest(netpeer.IP_PROTO_ICMP, good_icmp, flags_frag=0x2000))
        peer.send(netpeer.eth_frame(GUEST_MAC, PEER_MAC, netpeer.ETHERTYPE_IPV4,
                                    b"\x45\x00\x00\x30"))
        # 47 is GRE: something this stack genuinely does not implement, and
        # will not. It used to be TCP, until R3 implemented it.
        peer.send(ipv4_to_guest(47, b"\x00" * 20))
        time.sleep(1.0)

        ok, log = session.send_and_expect("cat /proc/net", r"drop: .*no-port", timeout=8.0)
        if not ok:
            return (name, False, f"/proc/net did not answer: {log[-500:]}")
        m = re.search(r"drop: (\d+) not-for-us, (\d+) short, (\d+) checksum, (\d+) fragment,\s*"
                      r"(\d+) proto, (\d+) no-route, (\d+) no-port", log)
        if not m:
            return (name, False, f"could not read the drop counters:\n{log[-700:]}")
        short, checksum, fragment, proto, no_port = (int(m.group(i)) for i in (2, 3, 4, 5, 7))
        problems = []
        if checksum != 1: problems.append(f"checksum={checksum} (want 1)")
        if fragment != 1: problems.append(f"fragment={fragment} (want 1)")
        if short != 1: problems.append(f"short={short} (want 1)")
        if proto != 1: problems.append(f"proto={proto} (want 1)")
        if no_port != 1: problems.append(f"no-port={no_port} (want 1)")
        if problems:
            return (name, False, "drop counters: " + ", ".join(problems))

        if not re.search(r"192\.168\.77\.1 02:00:00:00:00:42", log):
            return (name, False, "the peer is not in the ARP cache")

        return (name, True, "arp/icmp/udp answered byte-exact; five drop kinds counted apart")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()
        peer.close()


def test_ntp_client(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """R6, plan/phase19_ip_stack_and_ethernet.md: the SNTP client, against a
    server this test *is*.

    The point is the arithmetic, which is why this runs on QEMU rather than
    against a real server: NTP's epoch is 1900 and everything else's is 1970,
    its timestamps are 32.32 fixed point, and the offset is a signed
    difference of four of them. Every one of those is a place to be wrong by a
    constant -- and a constant error is exactly what a test against a
    *correct* server cannot see, because a clock that is already right stays
    right however the arithmetic is wrong. So the server here answers with an
    instant this test chose, years away from anything the board could be
    holding, and the board has to name it back.

    Three things are asserted, in the order they can fail:
      1. the request is a well-formed v4 client packet, from an ephemeral
         port, carrying a transmit timestamp a reply can be matched against,
      2. a correct reply moves the clock to the instant the server named,
      3. a reply whose origin timestamp does *not* echo the request is
         refused -- the one check standing between this client and an
         off-path forgery.

    The peer runs on its own thread rather than between console reads. The
    board sends its request and waits three seconds for an answer, so a
    single-threaded test would have to interleave log-tailing with frame
    handling, and QemuSession's reader seeks past whatever arrived while it
    was not looking -- which is a race, not a test.
    """
    import shutil
    import struct
    import threading
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import netpeer

    name = "NTP Client: Epoch, Fixed Point, And A Reply That Lies (R6)"
    arch_img = img_path.with_name(f"test_{arch_name}_ntp_sd.img")
    shutil.copyfile(img_path, arch_img)

    GUEST_IP = bytes([192, 168, 77, 2])
    PEER_IP = bytes([192, 168, 77, 1])
    PEER_MAC = b"\x02\x00\x00\x00\x00\x42"
    GUEST_MAC = bytes.fromhex("525400123456")
    NTP_EPOCH_OFFSET = 2208988800

    # 2031-06-15 12:34:56.500 UTC, deliberately years from the base epoch
    # kernel/time.c compiles in, so "the clock did not move" and "the clock
    # moved to the right instant" cannot be confused for one another.
    SERVER_UNIX_MS = 1939293296500

    def to_ntp(unix_ms: int) -> bytes:
        sec, ms = divmod(unix_ms, 1000)
        return struct.pack(">II", sec + NTP_EPOCH_OFFSET, (ms << 32) // 1000)

    def ntp_reply(origin: bytes) -> bytes:
        """A stratum-1 server's answer. `origin` is what goes in the origin
        field -- the request's own transmit timestamp for the honest case, and
        deliberately something else for the forgery."""
        pkt = bytearray(48)
        pkt[0] = (0 << 6) | (4 << 3) | 4      # LI 0, version 4, mode 4 (server)
        pkt[1] = 1                            # stratum 1: a reference clock, like the one this is pretending to be
        pkt[2] = 6
        pkt[3] = (-20) & 0xFF
        pkt[12:16] = b"GPS\x00"
        pkt[16:24] = to_ntp(SERVER_UNIX_MS)   # reference
        pkt[24:32] = origin
        pkt[32:40] = to_ntp(SERVER_UNIX_MS)   # receive
        pkt[40:48] = to_ntp(SERVER_UNIX_MS)   # transmit
        return bytes(pkt)

    peer = netpeer.NetPeer()
    session = QemuSession(elf_path, arch_img, arch_name)

    seen: dict[str, object] = {}
    forge = threading.Event()
    stop = threading.Event()

    def responder() -> None:
        """Gateway and time server both. ARP is answered because the board has
        to resolve the gateway before it can ask it anything, and letting that
        fail would put this test's timeout on the wrong protocol."""
        # NetPeer.received() returns everything it has ever captured and never
        # forgets, so this walks forward through it by index. Re-reading the
        # whole list each pass -- the obvious first version -- makes the
        # responder answer every *previous* request again on every poll, and
        # each answer to a closed port draws an ICMP unreachable that is itself
        # captured: the list grows quadratically and a pass eventually takes
        # longer than the board's three-second timeout. That failed once in
        # ten runs, which is the worst possible frequency for a test.
        handled = 0
        while not stop.is_set():
            frames = peer.received()
            fresh, handled = frames[handled:], len(frames)
            if not fresh:
                time.sleep(0.02)
                continue
            for f in fresh:
                try:
                    _d, _s, etype, payload = netpeer.parse_eth(f)
                    if etype == netpeer.ETHERTYPE_ARP:
                        arp = netpeer.parse_arp(payload)
                        if arp["op"] == 1 and arp["target_ip"] == PEER_IP:
                            peer.send(netpeer.eth_frame(
                                GUEST_MAC, PEER_MAC, netpeer.ETHERTYPE_ARP,
                                netpeer.arp_packet(2, PEER_MAC, PEER_IP, GUEST_MAC, GUEST_IP)))
                        continue
                    if etype != netpeer.ETHERTYPE_IPV4:
                        continue
                    ip = netpeer.parse_ipv4(payload)
                    if ip["proto"] != netpeer.IP_PROTO_UDP:
                        continue
                    udp = ip["payload"]
                    if int.from_bytes(udp[2:4], "big") != 123:
                        continue
                    sport = int.from_bytes(udp[0:2], "big")
                    req = udp[8:]
                    seen["req"] = req
                    seen["sport"] = sport
                    seen["count"] = int(seen.get("count", 0)) + 1  # type: ignore[arg-type]
                    origin = to_ntp(SERVER_UNIX_MS - 4000) if forge.is_set() else req[40:48]
                    peer.send(netpeer.eth_frame(
                        GUEST_MAC, PEER_MAC, netpeer.ETHERTYPE_IPV4,
                        netpeer.ipv4_packet(
                            PEER_IP, GUEST_IP, netpeer.IP_PROTO_UDP,
                            netpeer.udp_datagram(PEER_IP, GUEST_IP, 123, sport, ntp_reply(origin)))))
                except Exception:  # noqa: BLE001,S110 -- a malformed frame is not this test's subject
                    continue

    thread = threading.Thread(target=responder, daemon=True)
    try:
        session.start(extra_qemu_args=peer.qemu_args())
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        ok, log = session.send_and_expect(
            'lisp\n(net-config "192.168.77.2" "255.255.255.0" "192.168.77.1")\nexit',
            r"\[Net\] 192\.168\.77\.2/255\.255\.255\.0", timeout=6.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")

        thread.start()

        # --- 1 & 2. An honest server: the clock lands on the instant it named ---
        ok, log = session.send_and_expect("ntp", r"clock set  : 2031-06-15 12:34:5", timeout=15.0)
        if not ok:
            return (name, False, f"the clock was not set from the reply: {log[-700:]}")
        if "stratum 1 (GPS)" not in log:
            return (name, False, f"stratum/refid not reported: {log[-400:]}")

        # The offset is 1775 days -- 153,362,096,500 ms, which does not fit in
        # a 32-bit `long`, and `long` is 32 bits on RV32 and on RP2350. This
        # assertion exists because the first version of this test checked only
        # that the clock was set, and that passed on RV64 while the same build
        # on real RP2350 hardware printed a truncated, confidently-signed
        # -231194430 ms beside a clock it had just set correctly. Asserting the
        # *reported* figure is what makes a 64-bit value printed through a
        # 32-bit path a test failure rather than a hardware surprise.
        if "offset     : +1775 d " not in log:
            return (name, False,
                    "offset misreported -- expected '+1775 d ...' (153362096500 ms). "
                    "A 32-bit truncation prints -1256726156 instead. "
                    f"Got: {log[-500:]}")

        req = seen.get("req")
        sport = seen.get("sport")
        if not isinstance(req, (bytes, bytearray)) or len(req) < 48:
            return (name, False, f"never saw a 48-byte request (got {req!r})")
        if (req[0] & 7) != 3:
            return (name, False, f"request mode is {req[0] & 7}, not 3 (client)")
        if ((req[0] >> 3) & 7) != 4:
            return (name, False, f"request version is {(req[0] >> 3) & 7}, not 4")
        if req[40:48] == bytes(8):
            return (name, False, "request carries no transmit timestamp, so no reply could be matched to it")
        if not isinstance(sport, int) or not (49152 <= sport <= 65535):
            return (name, False, f"source port {sport} is outside the ephemeral range")

        # The clock is a *state*, so it would read 2031 after a sync that got
        # the sign of the offset backwards and then corrected itself. Ask the
        # shell separately, through the path a person would use.
        ok, log2 = session.send_and_expect("date", r"2031-06-15 .*UTC", timeout=6.0)
        if not ok:
            return (name, False, f"`date` does not agree with the sync: {log2[-400:]}")

        # --- 3. A reply that does not echo our request is refused ---
        forge.set()
        ok, log = session.send_and_expect(
            "ntp", r"ntp: the reply was not an answer to our question", timeout=15.0)
        if not ok:
            return (name, False,
                    f"forged origin not refused (requests seen: {seen.get('count')}, "
                    f"forge={forge.is_set()}): {log[-700:]}")

        return (name, True,
                "request well-formed from an ephemeral port; clock set to the server's "
                "instant; forged origin refused")
    except Exception as exc:  # noqa: BLE001
        return (name, False, f"exception: {exc}")
    finally:
        stop.set()
        if thread.is_alive():
            thread.join(timeout=2.0)
        session.close()
        peer.close()
        arch_img.unlink(missing_ok=True)


def test_tcp_stream(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """Q0, plan/phase26_mqtt_and_environment_sensors.md: a TCP connection
    carrying bytes rather than 9P frames.

    Until Q0 the only thing that could speak over a connection was 9P, because
    the framing assumption lived in link_poll()/link_recv_frame() -- so an
    MQTT client (Q1) needed a second *view* of a connection, not a second
    transport. This proves the view: 64 KB out and 64 KB back, in order, byte
    for byte, through a real socket on the host.

    **The peer is an ordinary host echo server, not a packet-level peer.**
    tests/netpeer.py exists for what a socket cannot do (a RST mid-stream, a
    lost segment) and test_tcp_under_impairment already uses it for that. What
    is under test here is the opposite: that a long, boring, correct
    conversation stays correct, which a real TCP stack on the other end is the
    honest way to check. QEMU's slirp puts the host at 10.0.2.2, so the guest
    dials a listener on the host's loopback with no forwarding rule needed.

    The pattern is position-dependent (see echotest_byte() in kernel/shell.c),
    so a run of bytes that is duplicated, dropped or reordered fails the
    comparison -- not only a corrupted one. The guest does the comparing, and
    reports the first offset that disagreed.
    """
    import threading

    name = "TCP Byte Stream: 64 KB Round Trip Through A Host Echo Server"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    srv.settimeout(30.0)

    seen = {"bytes": 0, "eof": False, "error": None}

    def echo() -> None:
        """Echoes until the peer closes. A graceful FIN from the guest shows up
        here as a clean EOF; a reset shows up as ECONNRESET, which is the
        difference tcp_stream_close() is asserted on below."""
        try:
            conn, _ = srv.accept()
            conn.settimeout(30.0)
            with conn:
                while True:
                    b = conn.recv(4096)
                    if not b:
                        seen["eof"] = True
                        break
                    seen["bytes"] += len(b)
                    conn.sendall(b)
        except Exception as exc:  # noqa: BLE001 -- reported, not raised, from a thread
            seen["error"] = repr(exc)

    thread = threading.Thread(target=echo, daemon=True)
    thread.start()

    session = QemuSession(elf_path, img_path, arch_name)
    try:
        session.start(extra_qemu_args=[
            "-netdev", "user,id=n0",
            "-device", "virtio-net-device,netdev=n0",
        ])
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        # 10.0.2.15/24 with 10.0.2.2 as the gateway is slirp's own layout; the
        # gateway address is also the host, which is what makes the echo
        # server reachable without a hostfwd rule.
        ok, log = session.send_and_expect(
            'lisp\n(net-config "10.0.2.15" "255.255.255.0" "10.0.2.2")\nexit',
            r"10\.0\.2\.15", timeout=8.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")

        ok, log = session.send_and_expect(
            f"net echotest 10.0.2.2 {port} 65536\n",
            r"echotest (ok,|failed|MISMATCH)", timeout=45.0)
        if not ok:
            return (name, False,
                    f"no verdict from `net echotest` in 45 s -- the stream stalled "
                    f"(host saw {seen['bytes']} bytes):\n{log[-700:]}")
        if "MISMATCH" in log:
            return (name, False, f"the echoed bytes are not the bytes sent:\n{log[-500:]}")
        if "echotest failed" in log:
            return (name, False, f"the stream did not complete:\n{log[-500:]}")

        # The guest says it round-tripped 64 KB; the host has to agree, or the
        # guest compared something it generated against itself.
        thread.join(timeout=10.0)
        if seen["bytes"] != 65536:
            return (name, False,
                    f"the guest reported success but the host echoed {seen['bytes']} "
                    f"bytes, not 65536 (thread error: {seen['error']})")
        if not seen["eof"]:
            return (name, False,
                    f"the connection did not close gracefully -- the host never saw EOF "
                    f"(error: {seen['error']}). tcp_stream_close() must send a FIN, not a reset.")

        # /proc/net has to name it as a stream, which is what distinguishes a
        # connection the 9P server must never be offered from one it serves.
        ok, log = session.send_and_expect("net\n", r"arp cache", timeout=8.0)
        if not ok:
            return (name, False, f"`net` did not answer after the transfer: {log[-400:]}")
        if "stream" not in log:
            return (name, False,
                    f"the connection is not reported as a stream, so a p9_link_t face "
                    f"may have been installed on it:\n{log[-600:]}")

        return (name, True, "64 KB echoed byte-for-byte, closed with a FIN, reported as a stream")
    finally:
        session.close()
        srv.close()


def test_tcp_state_machine(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """R3, plan/phase19_ip_stack_and_ethernet.md: the TCP state machine, driven
    by a client with no stack under it.

    tests/netpeer.py's TCPDriver builds every segment by hand, which is the
    only way to reach the states that matter: a segment out of order, a reset
    mid-stream, a half-close. A correct-by-construction socket cannot produce
    any of them, which is why the end-to-end test below is not a substitute
    for this one.

    Six cases:
      1. handshake -- SYN gets SYN|ACK with our sequence acknowledged and a
         window that matches the receive buffer
      2. a 9P Tversion in one segment is answered with a well-formed Rversion
      3. an out-of-order segment is dropped and answered with a duplicate ACK
         (not silence -- §2 wants the peer retransmitting now, not after its
         own RTO)
      4. the retransmission of that same segment, now in order, is accepted
      5. a half-close (our FIN) is acknowledged and answered with theirs
      6. a SYN to a port nobody is listening on gets a RST
    """
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import netpeer

    name = "TCP State Machine Against A Hand-Built Client (R3)"
    arch_img = img_path.with_name(f"test_{arch_name}_tcp_sd.img")
    shutil.copyfile(img_path, arch_img)

    GUEST_IP = bytes([192, 168, 77, 2])
    PEER_IP = bytes([192, 168, 77, 1])
    PEER_MAC = b"\x02\x00\x00\x00\x00\x42"
    GUEST_MAC = bytes.fromhex("525400123456")

    def tversion(msize: int = 4096) -> bytes:
        ver = b"9P2000"
        body = msize.to_bytes(4, "little") + len(ver).to_bytes(2, "little") + ver
        msg = bytes([100]) + b"\xff\xff" + body       # Tversion, NOTAG
        return (len(msg) + 4).to_bytes(4, "little") + msg

    peer = netpeer.NetPeer()
    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=peer.qemu_args())
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")
        ok, log = session.send_and_expect(
            'lisp\n(net-config "192.168.77.2" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.77\.2", timeout=6.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")

        tcp = netpeer.TCPDriver(peer, guest_mac=GUEST_MAC, peer_mac=PEER_MAC,
                                guest_ip=GUEST_IP, peer_ip=PEER_IP)

        # 1. handshake
        sa = tcp.handshake()
        if sa["ack"] != 1001:
            return (name, False, f"SYN|ACK acknowledges {sa['ack']}, expected 1001")
        if sa["window"] == 0:
            return (name, False, "SYN|ACK advertises a zero window")

        # 2. a real 9P exchange over it
        tcp.send(netpeer.TCP_ACK | netpeer.TCP_PSH, tversion())
        segs = tcp.segments(timeout=5.0)
        data = tcp.absorb(segs)
        if len(data) < 7:
            return (name, False, f"no Rversion came back (got {len(data)} bytes)")
        if data[4] != 101:
            return (name, False, f"expected Rversion (101), got message type {data[4]}")
        declared = int.from_bytes(data[0:4], "little")
        if declared != len(data):
            return (name, False, f"Rversion declares {declared} bytes, {len(data)} arrived")
        tcp.send(netpeer.TCP_ACK)

        # 3. out of order: sent 100 bytes ahead of what is expected
        before = tcp.snd
        tcp.send_raw(tcp.snd + 100, tcp.rcv, netpeer.TCP_ACK | netpeer.TCP_PSH, tversion())
        segs = tcp.segments(timeout=3.0)
        if not segs:
            return (name, False, "an out-of-order segment got silence, not a duplicate ACK")
        dup = segs[-1]
        if dup["ack"] != before:
            return (name, False,
                    f"duplicate ACK acknowledges {dup['ack']}, expected {before} "
                    "(the stack accepted data it should have dropped)")
        if dup["data"]:
            return (name, False, "the duplicate ACK carried data")

        # 4. the same segment, now in order, is accepted
        tcp.send(netpeer.TCP_ACK | netpeer.TCP_PSH, tversion())
        segs = tcp.segments(timeout=5.0)
        data = tcp.absorb(segs)
        if len(data) < 7 or data[4] != 101:
            return (name, False, "the in-order retransmission was not accepted")
        tcp.send(netpeer.TCP_ACK)

        # 5. half-close: our FIN must be acknowledged, and answered
        tcp.send(netpeer.TCP_FIN | netpeer.TCP_ACK)
        segs = tcp.segments(timeout=5.0, count=1)
        if not segs:
            return (name, False, "the FIN was not answered")
        saw_ack = any(s["ack"] == tcp.snd for s in segs)
        saw_fin = any(s["flags"] & netpeer.TCP_FIN for s in segs)
        if not saw_ack:
            return (name, False, f"our FIN was not acknowledged: "
                                 f"{[netpeer.flags_str(s['flags']) for s in segs]}")
        if not saw_fin:
            return (name, False, "the peer never sent its own FIN (CLOSE_WAIT is stuck)")

        # 6. a SYN to a closed port is reset, not ignored
        stray = netpeer.TCPDriver(peer, guest_mac=GUEST_MAC, peer_mac=PEER_MAC,
                                  guest_ip=GUEST_IP, peer_ip=PEER_IP,
                                  sport=41000, dport=9999)
        stray.send(netpeer.TCP_SYN, mss=1460)
        segs = stray.segments(timeout=3.0)
        if not any(s["flags"] & netpeer.TCP_RST for s in segs):
            return (name, False, "a SYN to a closed port was not reset")

        return (name, True, "handshake, 9P, duplicate ACK, retransmit, half-close, stray RST")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()
        peer.close()


def test_tcp_under_impairment(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """R3, plan/phase19_ip_stack_and_ethernet.md: the recovery paths, which no
    clean link ever runs.

    This stack's riskiest simplifications -- no reassembly, a send window with
    no congestion control, a fixed RTO with backoff, window accounting done by
    hand -- all live in code that only executes when something goes wrong.
    Both of the other TCP oracles run over loopback, where nothing does: the
    packet-level peer delivers everything in order and libslirp does not lose
    frames. So the retransmission and window paths had **zero coverage** until
    this test, which is a poor position from which to meet a real wire.

    Deterministic, not random. A fuzzer that drops 5% of frames finds these
    bugs eventually and reproduces them never; a client that drops exactly the
    ACK it means to drop fails the same way every time. tests/netpeer.py's
    TCPDriver builds every segment by hand, so it can simply decline to
    acknowledge.

    Five cases:
      1. an unacknowledged reply is retransmitted, byte-identical, at the same
         sequence number -- and stops once acknowledged
      2. a duplicated request is acknowledged twice but delivered once
      3. the advertised receive window shrinks as a partial frame accumulates
         and reopens when the frame completes
      4. a peer advertising a one-byte window gets its reply one byte at a
         time, in order, and complete
      5. a reset mid-stream frees the connection slot for the next client
    """
    import shutil
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import netpeer

    name = "TCP Recovery Paths: Loss, Duplication, Window Squeeze, Reset (R3)"
    arch_img = img_path.with_name(f"test_{arch_name}_impair_sd.img")
    shutil.copyfile(img_path, arch_img)

    GUEST_IP = bytes([192, 168, 77, 2])
    PEER_IP = bytes([192, 168, 77, 1])
    PEER_MAC = b"\x02\x00\x00\x00\x00\x42"
    GUEST_MAC = bytes.fromhex("525400123456")

    def tversion(msize: int = 4096) -> bytes:
        ver = b"9P2000"
        body = msize.to_bytes(4, "little") + len(ver).to_bytes(2, "little") + ver
        msg = bytes([100]) + b"\xff\xff" + body
        return (len(msg) + 4).to_bytes(4, "little") + msg

    peer = netpeer.NetPeer()
    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=peer.qemu_args())
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")
        ok, log = session.send_and_expect(
            'lisp\n(net-config "192.168.77.2" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.77\.2", timeout=6.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")

        def dial(sport: int) -> "netpeer.TCPDriver":
            d = netpeer.TCPDriver(peer, guest_mac=GUEST_MAC, peer_mac=PEER_MAC,
                                  guest_ip=GUEST_IP, peer_ip=PEER_IP, sport=sport)
            d.handshake()
            return d

        def drop(d: "netpeer.TCPDriver") -> None:
            """Reset, because there are only two connection slots and this test
            needs six. A reset frees one immediately; a graceful close would
            leave it in TIME_WAIT and the next dial would be refused -- which
            is exactly what happened the first time this test was run, and is
            a fair warning about how little headroom two slots leave."""
            d.send(netpeer.TCP_RST)
            time.sleep(0.3)

        # --- 1. a reply we never acknowledge must come back, unchanged ---
        tcp = dial(40100)
        tcp.send(netpeer.TCP_ACK | netpeer.TCP_PSH, tversion())
        segs = [s for s in tcp.segments(timeout=5.0) if s["data"]]
        if not segs:
            return (name, False, "no reply to acknowledge in the first place")
        first = segs[0]
        # Deliberately no ACK. The RTO is 300 ms with exponential backoff, so
        # two seconds is several attempts' worth without being a long wait.
        time.sleep(2.0)
        again = [s for s in tcp.segments(timeout=1.0) if s["data"]]
        if not again:
            return (name, False, "an unacknowledged reply was never retransmitted")
        rx = again[0]
        if rx["seq"] != first["seq"] or rx["data"] != first["data"]:
            return (name, False,
                    "the retransmission is not the same bytes at the same sequence:\n"
                    f"  first  seq={first['seq']} {bytes(first['data']).hex()}\n"
                    f"  resent seq={rx['seq']} {bytes(rx['data']).hex()}")
        # Acknowledge it, and it must stop.
        tcp.rcv = first["seq"] + len(first["data"])
        tcp.send(netpeer.TCP_ACK)
        time.sleep(1.5)
        stale = [s for s in tcp.segments(timeout=1.0) if s["data"]]
        if stale:
            return (name, False, f"still retransmitting {len(stale)} segments after an ACK")

        drop(tcp)
        # --- 2. a duplicated request is delivered once ---
        peer.clear()
        tcp2 = dial(40101)
        req = tversion()
        at = tcp2.snd
        tcp2.send_raw(at, tcp2.rcv, netpeer.TCP_ACK | netpeer.TCP_PSH, req)
        tcp2.send_raw(at, tcp2.rcv, netpeer.TCP_ACK | netpeer.TCP_PSH, req)   # same seq, again
        tcp2.snd = at + len(req)
        segs = tcp2.segments(timeout=5.0)
        replies = [s for s in segs if s["data"]]
        if len(replies) != 1:
            return (name, False,
                    f"a duplicated request produced {len(replies)} replies, expected 1 "
                    "(the stack processed the same bytes twice)")
        acks = {s["ack"] for s in segs}
        if any(a > at + len(req) for a in acks):
            return (name, False, f"the stack acknowledged past the data it was sent: {acks}")

        drop(tcp2)
        # --- 3. the receive window shrinks on a partial frame and reopens ---
        peer.clear()
        tcp3 = dial(40102)
        full = tversion()
        tcp3.send(netpeer.TCP_ACK | netpeer.TCP_PSH, full[:-1])    # one byte short
        segs = tcp3.segments(timeout=4.0)
        if not segs:
            return (name, False, "a partial frame was not acknowledged")
        squeezed = segs[-1]["window"]
        if squeezed >= 65535 or squeezed == 0:
            return (name, False, f"window after a partial frame is {squeezed}, expected a shrink")
        tcp3.send(netpeer.TCP_ACK | netpeer.TCP_PSH, full[-1:])    # complete it
        segs = tcp3.segments(timeout=5.0)
        replies = [s for s in segs if s["data"]]
        if not replies:
            return (name, False, "the completed frame was not answered")
        reopened = max(s["window"] for s in segs)
        if reopened <= squeezed:
            return (name, False,
                    f"the window did not reopen after the frame was taken "
                    f"({squeezed} -> {reopened}); a peer that filled it would stall forever")

        drop(tcp3)
        # --- 4. a one-byte window must be honoured, byte by byte ---
        peer.clear()
        tcp4 = dial(40103)
        tcp4.send_raw(tcp4.snd, tcp4.rcv, netpeer.TCP_ACK | netpeer.TCP_PSH,
                      tversion(), window=1)
        tcp4.snd += len(tversion())
        collected = b""
        expect_seq = None
        for _ in range(40):
            segs = [s for s in tcp4.segments(timeout=1.0) if s["data"]]
            for s in segs:
                if len(s["data"]) != 1:
                    return (name, False,
                            f"a one-byte window got a {len(s['data'])}-byte segment")
                if expect_seq is not None and s["seq"] != expect_seq:
                    continue                      # a retransmission; ignore
                collected += s["data"]
                expect_seq = s["seq"] + 1
                tcp4.rcv = expect_seq
                tcp4.send_raw(tcp4.snd, tcp4.rcv, netpeer.TCP_ACK, b"", window=1)
            if len(collected) >= 19:
                break
        if len(collected) < 7 or collected[4] != 101:
            return (name, False,
                    f"the byte-at-a-time reply did not reassemble: {collected.hex()}")
        declared = int.from_bytes(collected[0:4], "little")
        if declared != len(collected):
            return (name, False,
                    f"reassembled {len(collected)} bytes, the message declares {declared}")

        drop(tcp4)
        # --- 5. a reset mid-stream must free the slot ---
        peer.clear()
        tcp5 = dial(40104)
        tcp5.send(netpeer.TCP_RST)
        time.sleep(0.5)
        ok, log = session.send_and_expect("cat /proc/net", r"tcp: listening", timeout=8.0)
        if ok and re.search(r"192\.168\.77\.1:40104", log):
            return (name, False, f"a reset connection is still in the table: {log[-400:]}")
        tcp6 = dial(40105)            # the slot must be dialable again
        tcp6.send(netpeer.TCP_ACK | netpeer.TCP_PSH, tversion())
        replies = [s for s in tcp6.segments(timeout=5.0) if s["data"]]
        if not replies:
            return (name, False, "the slot freed by a reset could not be reused")
        drop(tcp6)

        return (name, True, "retransmit identical, duplicate delivered once, window "
                            "shrank and reopened, one-byte window honoured, reset freed the slot")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()
        peer.close()


def test_9p_over_own_tcp(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """R3's headline: a real 9P session over a TCP stack we wrote, through a
    real host socket, with authentication.

    QEMU's slirp backend gives the guest a virtual LAN (the fixed 10.0.2.15/24
    with a 10.0.2.2 gateway -- static, which suits §7's no-DHCP rule) and
    gives the host a forwarded port into it. Nothing on the host side is new:
    p9lib's connect_tcp(), the Tauth exchange and the framing are phase 18's,
    unchanged. What changed is underneath -- the guest now terminates the TCP
    itself instead of being handed a byte stream by a chardev.

    **Which TCP is on the other end, precisely.** `hostfwd` does not forward
    packets: libslirp *terminates* the host's connection and originates a
    separate one to the guest. So the host-to-QEMU leg is Linux's TCP, and the
    leg this stack actually speaks is **libslirp's** -- a userspace stack
    descended from 4.4BSD-Lite. A good, genuinely independent oracle, and not
    the same claim as "tested against Linux", which nothing here does yet.

    Both halves of the auth gate are checked, because `auth_required` is set
    on this link by net/tcp.c and a network that accepts anonymous attaches
    would be a regression phase 18 exists to prevent."""
    import shutil
    import socket as _socket
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host" / "p9lib" / "src"))
    import p9lib

    name = "9P Over Our Own TCP, Authenticated, Through A Host Socket (R3)"
    arch_img = img_path.with_name(f"test_{arch_name}_9ptcp_sd.img")
    shutil.copyfile(img_path, arch_img)
    key = bytes(range(16))

    probe = _socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=[
            "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-10.0.2.15:564",
            "-device", "virtio-net-device,netdev=n0",
        ])
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")
        ok, log = session.send_and_expect(
            'lisp\n(net-config "10.0.2.15" "255.255.255.0" "10.0.2.2")\nexit',
            r"\[Net\] 10\.0\.2\.15", timeout=6.0)
        if not ok:
            return (name, False, f"(net-config) did not take: {log[-500:]}")
        ok, log = session.send_and_expect(f"p9key {key.hex()}",
                                          r"console key set \(16 bytes\)", timeout=5.0)
        if not ok:
            return (name, False, f"the key was not accepted: {log[-500:]}")

        # An attach with no key must be refused -- the link sets auth_required.
        try:
            anon = p9lib.connect_tcp("127.0.0.1", port, timeout=15.0)
            try:
                anon.version()
                anon.attach(1)
                return (name, False, "an unauthenticated attach succeeded over the network")
            except Exception:
                pass
            finally:
                anon.close()
        except OSError as e:
            return (name, False, f"could not reach the guest at all: {e}")

        client = p9lib.connect_tcp("127.0.0.1", port, timeout=15.0)
        with p9lib.Session(client, key=key) as sess:
            version = sess.read("/proc/version")
            if b"LugalOS" not in version:
                return (name, False, f"/proc/version reads {version[:60]!r}")
            names = {st.name for st in sess.listdir("/")}
            if not {"proc", "sd0"} <= names:
                return (name, False, f"ls / is missing mounts: {sorted(names)}")
            # Bigger than one segment and bigger than one msize: this is the
            # part a single-frame transport never exercised.
            init = sess.read("/sd0/system/etc/init.lisp")
            want = expected_init_lisp()
            if len(init) != len(want):
                return (name, False, f"init.lisp came back {len(init)} bytes, expected {len(want)}")

        ok, log = session.send_and_expect("cat /proc/net", r"tcp: listening on 564", timeout=8.0)
        if not ok:
            return (name, False, f"/proc/net did not answer: {log[-400:]}")
        m = re.search(r"drop: (\d+) not-for-us, (\d+) short, (\d+) checksum, (\d+) fragment,\s*"
                      r"(\d+) proto, (\d+) no-route, (\d+) no-port", log)
        if m and any(int(g) for g in m.groups()):
            return (name, False, f"a clean session should drop nothing: {m.group(0)}")

        return (name, True, f"{len(init)} bytes read over our own TCP; anonymous attach refused")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_9p_auth_gate(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """The Tauth/afid gate, end to end, over a **TCP** chardev (N2,
    plan/phase18_networking_and_auth.md).

    TCP on purpose, and the guest does not speak it. The chardev terminates
    the TCP and hands the guest an ordinary virtio-console byte stream, so
    what this exercises is the *host* half (p9lib.connect_tcp(), the auth
    exchange, the framing) -- real code that would otherwise only ever run
    against hardware.

    Phase 19's R3 gives the guest its own TCP, at which point this test keeps
    working unchanged and a second one joins it that goes through the guest's
    stack (plan/phase19_ip_stack_and_ethernet.md §4). Until then, everything
    below the byte stream stays untested here, which is the honest boundary
    and was the whole reason §4 specifies a packet-level peer.

    Six cases, each of which has been a real bug in somebody's auth code:
      1. attach with no Tauth at all, on a link that requires it   -> refused
      2. the correct key                                           -> works
      3. a wrong key                                               -> refused
      4. a response replayed against a fresh nonce                 -> refused
      5. a key set for one uname, attached as another              -> refused
      6. the same link with the requirement turned off             -> works
    """
    import shutil
    import tempfile
    import socket as _socket
    import hmac as _hmac
    import hashlib as _hashlib
    import p9lib
    from p9lib.client import P9Client, P9Error, Session, NOFID

    NAME = "9P Tauth/afid Gate Over TCP: Six Refusal And Acceptance Cases (N2)"
    KEY = bytes(range(1, 17))            # 16 bytes, hex below
    KEY_HEX = KEY.hex()

    arch_img = img_path.with_name(f"test_{arch_name}_9pauth_sd.img")
    shutil.copyfile(img_path, arch_img)

    # An ephemeral port, released before QEMU binds it. The race is real but
    # tiny, and the alternative (a fixed port) collides with a previous run
    # that has not finished dying yet -- which is the failure this file's
    # own retry logic exists to avoid elsewhere.
    _s = _socket.socket()
    _s.bind(("127.0.0.1", 0))
    port = _s.getsockname()[1]
    _s.close()

    def dial() -> P9Client:
        last = ""
        for _ in range(25):
            try:
                return p9lib.connect_tcp("127.0.0.1", port, timeout=3.0)
            except (ConnectionRefusedError, OSError) as e:
                last = str(e)
                time.sleep(0.2)
        raise P9Error(f"could not connect to 127.0.0.1:{port}: {last}")

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=[
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=p9c",
            "-chardev", f"socket,id=p9c,host=127.0.0.1,port={port},server=on,wait=off",
        ])
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        if not ok:
            return (NAME, False, log)

        # Install a key for this boot and arm the link. Both from the console,
        # which is the trusted side by the same argument that lets the local
        # channel skip authentication entirely.
        ok, log = session.send_and_expect(f"p9key {KEY_HEX}\n", r"console key set", timeout=5.0)
        if not ok:
            return (NAME, False, f"p9key did not take: {log}")
        ok, log = session.send_and_expect("p9auth vconsole on\n", r"REQUIRES authentication", timeout=5.0)
        if not ok:
            return (NAME, False, f"p9auth did not take: {log}")

        failures = []

        # 1. No Tauth at all.
        c = dial()
        try:
            c.version()
            try:
                c.attach(1, aname="", uname="lugal", afid=NOFID)
                failures.append("unauthenticated attach SUCCEEDED on a link that requires auth")
            except P9Error as e:
                if "auth" not in str(e).lower():
                    failures.append(f"refused, but not for authentication: {e}")
        finally:
            c.close()

        # 2. The real thing, through the same Session a user would use.
        c = dial()
        try:
            sess = Session(c, key=KEY)
            # The Session's own read, not client.cat(): cat() opens a fresh
            # connection (Tversion resets all server fid state), which would
            # throw away the afid this Session just authenticated.
            data = sess.read("/proc/version")
            if b"LugalOS" not in data:
                failures.append(f"authenticated read returned {data[:60]!r}")
        except P9Error as e:
            failures.append(f"correct key was refused: {e}")
        finally:
            c.close()

        # 3. A wrong key.
        c = dial()
        try:
            c.version()
            try:
                c.authenticate(0, b"not-the-key")
                failures.append("a wrong key was accepted")
            except P9Error:
                pass
        finally:
            c.close()

        # 4. Replay: capture a valid response, then present it against the
        #    *next* nonce. This is the case a fixed challenge would pass.
        c = dial()
        try:
            c.version()
            c.auth(0, aname="", uname="lugal")
            nonce1 = c.read(0, 0, 32)
            mac1 = _hmac.new(KEY, nonce1 + b"lugal", _hashlib.sha256).digest()
            c.write(0, 0, mac1)          # legitimate, and now spent
            c.clunk(0)

            c.auth(0, aname="", uname="lugal")
            nonce2 = c.read(0, 0, 32)
            if nonce2 == nonce1:
                failures.append("the server reused a nonce across two Tauths")
            try:
                c.write(0, 0, mac1)      # the old response, new challenge
                failures.append("a replayed response was accepted")
            except P9Error:
                pass
        finally:
            c.close()

        # 5. Authenticated as one identity, attaching as another.
        c = dial()
        try:
            c.version()
            c.authenticate(0, KEY, uname="lugal")
            try:
                c.attach(1, aname="", uname="someone-else", afid=0)
                failures.append("an afid authenticated for 'lugal' attached as 'someone-else'")
            except P9Error:
                pass
        finally:
            c.close()

        # 6. Requirement off: the same link goes back to attaching freely,
        #    which is what every other test in this file depends on.
        ok, log = session.send_and_expect("p9auth vconsole off\n",
                                          r"does not require authentication", timeout=5.0)
        if not ok:
            failures.append(f"could not turn the requirement back off: {log}")
        else:
            c = dial()
            try:
                Session(c)               # no key at all
            except P9Error as e:
                failures.append(f"unauthenticated attach refused after p9auth off: {e}")
            finally:
                c.close()

        return (NAME, not failures, "\n".join(failures))
    except Exception as e:
        return (NAME, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_identity_grants_scope(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """I5, plan/phase21_identity_and_authentication.md: §5.2's grants,
    exercised end to end over the same TCP-bridged virtio-console setup
    test_9p_auth_gate() uses just above -- real code driving the raw
    protocol, not our own client asking for what it always asks for
    (fs/p9_link.c's in-kernel client only ever requests aname=""). Three of
    §8's four verify points for I5:

      - a peer granted /ram0 cannot attach at "/"
      - a peer granted `ro` is refused a Twrite and told why
      - a removed peer is refused immediately (the very next Tauth)

    The fourth (eight entries fill, the ninth is rejected) is
    test_identity_grants_cap() below -- a `peers add` property with no
    attach involved, so it needs neither p9lib nor this setup."""
    import shutil
    import socket as _socket
    import p9lib
    from p9lib.client import P9Client, P9Error, Session

    NAME = "Grants: Scope Restricts Aname, Ro Refuses Twrite, Removal Is Immediate (I5)"
    KEY_RW = bytes(range(1, 17)).hex()
    KEY_RO = bytes(range(17, 33)).hex()

    arch_img = img_path.with_name(f"test_{arch_name}_grants_sd.img")
    shutil.copyfile(img_path, arch_img)

    _s = _socket.socket()
    _s.bind(("127.0.0.1", 0))
    port = _s.getsockname()[1]
    _s.close()

    def dial() -> P9Client:
        last = ""
        for _ in range(25):
            try:
                return p9lib.connect_tcp("127.0.0.1", port, timeout=3.0)
            except (ConnectionRefusedError, OSError) as e:
                last = str(e)
                time.sleep(0.2)
        raise P9Error(f"could not connect to 127.0.0.1:{port}: {last}")

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start(extra_qemu_args=[
            "-device", "virtio-serial-device",
            "-device", "virtconsole,chardev=p9c",
            "-chardev", f"socket,id=p9c,host=127.0.0.1,port={port},server=on,wait=off",
        ])
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
        if not ok:
            return (NAME, False, log)

        ok, log = session.send_and_expect('lisp\n(format "/sd0")\nexit', r"=> #t", timeout=6.0)
        if not ok:
            return (NAME, False, f"could not format /sd0: {log}")

        ok, log = session.send_and_expect(f"peers add rw-peer {KEY_RW} /ram0 rw", r"granted", timeout=5.0)
        if not ok:
            return (NAME, False, f"grant for rw-peer did not take: {log}")
        ok, log = session.send_and_expect(f"peers add ro-peer {KEY_RO} /ram0 ro", r"granted", timeout=5.0)
        if not ok:
            return (NAME, False, f"grant for ro-peer did not take: {log}")

        ok, log = session.send_and_expect("p9auth vconsole on\n", r"REQUIRES authentication", timeout=5.0)
        if not ok:
            return (NAME, False, f"p9auth did not take: {log}")

        failures = []

        # 1a. rw-peer, granted /ram0, cannot attach at "/".
        c = dial()
        try:
            c.version()
            try:
                c.authenticate(0, bytes.fromhex(KEY_RW), aname="/", uname="rw-peer")
                c.attach(1, aname="/", uname="rw-peer", afid=0)
                failures.append("rw-peer, granted /ram0, attached at / anyway")
            except P9Error as e:
                if "not granted" not in str(e).lower():
                    failures.append(f"refused at /, but not for scope: {e}")
        finally:
            c.close()

        # 1b. ...and can attach, and write, at exactly what it was granted.
        c = dial()
        try:
            sess = Session(c, aname="/ram0", uname="rw-peer", key=bytes.fromhex(KEY_RW))
            # A path relative to the session's own attached root (aname
            # "/ram0"), not "/ram0/..." again -- the attach already rooted
            # this session there.
            n = sess.write("/grants_test.txt", b"hello from rw-peer")
            if n <= 0:
                failures.append(f"rw-peer could not write inside its granted subtree: wrote {n} bytes")
        except P9Error as e:
            failures.append(f"rw-peer was refused inside its own granted subtree: {e}")
        finally:
            c.close()

        # 2. ro-peer, granted /ram0 ro: attaches fine, refused a write and
        #    told why (Session.write() reaches Tcreate for a file that
        #    doesn't exist yet, which is where the read-only check lives).
        c = dial()
        try:
            sess = Session(c, aname="/ram0", uname="ro-peer", key=bytes.fromhex(KEY_RO))
            try:
                sess.write("/ro_should_fail.txt", b"should not land")
                failures.append("ro-peer's write was accepted")
            except P9Error as e:
                if "read-only" not in str(e).lower():
                    failures.append(f"ro-peer's write was refused, but not for read-only: {e}")
        except P9Error as e:
            failures.append(f"ro-peer could not even attach at its granted subtree: {e}")
        finally:
            c.close()

        # 3. Remove ro-peer; the very next Tauth for it is refused --
        #    nothing to invalidate, since nothing here was ever cached.
        ok, log = session.send_and_expect("peers remove ro-peer", r"removed", timeout=5.0)
        if not ok:
            failures.append(f"peers remove did not report success: {log}")
        c = dial()
        try:
            c.version()
            try:
                c.authenticate(0, bytes.fromhex(KEY_RO), aname="/ram0", uname="ro-peer")
                failures.append("a removed peer's key was still accepted")
            except P9Error:
                pass
        finally:
            c.close()

        return (NAME, not failures, "\n".join(failures))
    except Exception as e:
        return (NAME, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_identity_grants_cap(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """I5's fourth verify point, standalone: it needs neither p9lib nor a
    live attach, just `peers add` run nine times. Eight distinct names
    fill the table; the ninth is refused with a message (not silently
    dropped, not silently overwriting the oldest); updating one of the
    eight already there still works at 8/8, since that is a replacement,
    not a new slot (fs/9p.c's p9_grants_add() docstring)."""
    import shutil
    name = "Grants List Bounded To Eight Entries, Ninth Refused With A Message (I5)"
    arch_img = img_path.with_name(f"test_{arch_name}_grantscap_sd.img")
    shutil.copyfile(img_path, arch_img)

    session = QemuSession(elf_path, arch_img, arch_name)
    try:
        session.start()
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"guest did not reach the shell: {log[-400:]}")

        ok, log = session.send_and_expect('lisp\n(format "/sd0")\nexit', r"=> #t", timeout=6.0)
        if not ok:
            return (name, False, f"could not format /sd0: {log[-400:]}")

        for i in range(8):
            key = f"{i:02x}" * 16
            ok, log = session.send_and_expect(f"peers add peer{i} {key}", r"granted", timeout=6.0)
            if not ok:
                return (name, False, f"entry {i} (of 8) was refused: {log[-500:]}")

        ok, log = session.send_and_expect("peers\n", r"peer7", timeout=6.0)
        if not ok:
            return (name, False, f"peers list does not show the 8th entry: {log[-500:]}")

        key9 = "ff" * 16
        ok, log = session.send_and_expect(f"peers add peer8 {key9}", r"full", timeout=6.0)
        if not ok:
            return (name, False, f"a 9th distinct entry was not refused with a 'full' message: {log[-500:]}")

        # Updating an EXISTING entry must still work at 8/8: it replaces a
        # slot rather than claiming a new one.
        ok, log = session.send_and_expect(f"peers add peer0 {key9}", r"granted", timeout=6.0)
        if not ok:
            return (name, False, f"updating an existing entry at 8/8 was refused: {log[-500:]}")

        return (name, True, "8 entries filled; a 9th distinct name refused with a message; "
                            "updating an existing entry at 8/8 still works")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session.close()


def test_9p_iounit(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """An Ropen's iounit must fit inside the negotiated msize.

    fs/9p.c wrote a hardcoded `4096` here, described in a comment as "no
    restriction beyond the negotiated msize". It was a restriction, and the
    number could not be honoured: Rread frames its payload behind
    size[4] type[1] tag[2] count[4], so a 4096-byte read against a 4096 msize
    needs 4107 bytes on the wire. A conforming client that believed the
    advertised iounit and issued a read that size would be asking for a reply
    the connection cannot carry. It is now derived from what Tversion actually
    agreed (§2.3, plan/phase15_memory_reclamation.md).

    Asserted as the protocol invariant rather than against a fixed number, so
    it holds on every target: RP2350 negotiates 2048 and the QEMU builds 4096,
    and both must satisfy `0 < iounit <= msize - IOHDRSZ`. The old hardcoded
    value fails it on both.
    """
    import shutil
    import tempfile
    import p9lib
    from p9lib.client import IOHDRSZ

    name = "9P Ropen iounit fits the negotiated msize (protocol invariant)"
    arch_img = img_path.with_name(f"test_{arch_name}_9p_iounit_sd.img")
    shutil.copyfile(img_path, arch_img)

    with tempfile.TemporaryDirectory() as tmpdir:
        sock_path = str(Path(tmpdir) / "p9iou.sock")
        session = QemuSession(elf_path, arch_img, arch_name)
        try:
            session.start(extra_qemu_args=[
                "-device", "virtio-serial-device",
                "-device", "virtconsole,chardev=p9c",
                "-chardev", f"socket,id=p9c,path={sock_path},server=on,wait=off",
            ])
            ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
            if not ok:
                return (name, False, log)

            last_err = ""
            for _ in range(20):
                try:
                    client = p9lib.connect_unix(sock_path, timeout=2.0)
                    break
                except (FileNotFoundError, ConnectionRefusedError, OSError) as e:
                    last_err = str(e)
                    time.sleep(0.2)
            else:
                return (name, False, f"could not connect to {sock_path}: {last_err}")

            try:
                msize = client.version()
                client.attach(0)
                client.walk(0, 1, ["proc", "version"])
                client.open(1)
                iounit = client.last_iounit
                client.clunk(1)
            finally:
                client.close()

            limit = msize - IOHDRSZ
            ok = 0 < iounit <= limit
            detail = f"msize={msize} iounit={iounit} limit={limit}"
            return (name, ok, "" if ok else f"iounit outside the msize: {detail}")
        except Exception as e:
            return (name, False, str(e))
        finally:
            session.close()


def test_9p_crud_via_p9lib(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """A6: exercises host/p9lib's Session (the "product-p9lib" file-utility
    layer, not just the low-level P9Client the A3 test above uses) through a
    full write/read/truncate/stat/mkdir/remove cycle over the same
    virtio-console link as test_9p_virtio_link. This is the regression test
    that would have caught fs/fat32.c's filename_to_83() dropping the
    extension of any 9+ character base name (e.g. "host_test.txt" and
    "host_test_dir" both silently collapsed to the bare 8.3 name
    "HOST_TES", so creating one made vfs_mkdir()'s fat32_find_file() see a
    false "already exists" for the other) -- every prior 9P test only ever
    read pre-existing short 8.3-named files, never wrote or mkdir'd a
    long name of its own. Also guards against a second bug found the same
    way, later, by host/fuse-p9: Session leaking WORK_FID on a partial-walk
    stat() failure (see the inline comment below) -- fixed in
    p9lib/client.py's _walk_or_raise()."""
    import shutil
    import tempfile
    import p9lib

    name = "9P Read/Write/Mkdir CRUD Cycle Via host/p9lib Session (A6, external client)"
    arch_img = img_path.with_name(f"test_{arch_name}_9p_crud_sd.img")
    shutil.copyfile(img_path, arch_img)

    with tempfile.TemporaryDirectory() as tmpdir:
        sock_path = str(Path(tmpdir) / "p9crud.sock")
        session = QemuSession(elf_path, arch_img, arch_name)
        try:
            session.start(extra_qemu_args=[
                "-device", "virtio-serial-device",
                "-device", "virtconsole,chardev=p9c",
                "-chardev", f"socket,id=p9c,path={sock_path},server=on,wait=off",
            ])
            ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=5.0)
            if not ok:
                return (name, False, log)

            client = None
            last_err = ""
            for _ in range(20):
                try:
                    client = p9lib.connect_unix(sock_path, timeout=2.0)
                    break
                except (FileNotFoundError, ConnectionRefusedError, OSError) as e:
                    last_err = str(e)
                    time.sleep(0.2)
            if client is None:
                return (name, False, f"could not connect to {sock_path}: {last_err}")

            with p9lib.Session(client, aname="/") as sess:
                # Regression check for a real bug host/fuse-p9 (phase14
                # 14a-follow-on) found: stat'ing a path whose LAST
                # component doesn't exist yet (a common check-before-create
                # pattern -- FUSE's getattr() does exactly this before
                # every create()) is a *partial* Twalk (the parent
                # resolves, the leaf doesn't). fs/9p.c leaves WORK_FID
                # bound to the parent in that case, and Session used to
                # never clunk it before raising, permanently wedging the
                # shared fid ("walk: newfid already in use") for every
                # call after it. Confirm the Session survives this and is
                # still fully usable afterward.
                try:
                    sess.stat("/sd0/does_not_exist_yet.txt")
                    return (name, False, "stat of a missing path should have raised P9Error")
                except p9lib.P9Error:
                    pass

                n = sess.write("/sd0/host_test.txt", b"hello from product-p9lib\n")
                if n != len(b"hello from product-p9lib\n"):
                    return (name, False, f"write returned {n} bytes")

                back = sess.read("/sd0/host_test.txt")
                if back != b"hello from product-p9lib\n":
                    return (name, False, f"round-trip mismatch: {back!r}")

                sess.write("/sd0/host_test.txt", b"short\n")
                back2 = sess.read("/sd0/host_test.txt")
                if back2 != b"short\n":
                    return (name, False, f"truncate-on-overwrite failed: {back2!r}")

                # Tstat's name comes back as the requested path's own last
                # component (fs/9p.c's p9_handle_tstat resolves it from the
                # fid's tracked path, not a FAT32 8.3 round-trip), unlike a
                # directory *listing*, which reflects the on-disk 8.3 name
                # via fat83_to_display_name() -- see the mkdir check below.
                st = sess.stat("/sd0/host_test.txt")
                if st.is_dir or st.length != len(b"short\n") or st.name != "host_test.txt":
                    return (name, False,
                             f"stat mismatch: name={st.name!r} length={st.length} is_dir={st.is_dir}")

                # filename_to_83()'s bug dropped the extension of any 9+
                # char base name, so the on-disk 8.3 name (as reported by a
                # directory listing) would show up as bare "HOST_TES" here
                # instead of "HOST_TES.TXT".
                entries = sess.listdir("/sd0")
                by_name = {e.name: e for e in entries}
                if "HOST_TES.TXT" not in by_name:
                    return (name, False, f"expected on-disk name HOST_TES.TXT, got: {sorted(by_name)}")

                # A long base name (>8 chars) sharing host_test.txt's first 8
                # characters -- the collision filename_to_83()'s bug caused.
                sess.mkdir("/sd0/host_test_dir")
                st_dir = sess.stat("/sd0/host_test_dir")
                if not st_dir.is_dir:
                    return (name, False, "mkdir'd entry is not reported as a directory")

                sess.write("/sd0/host_test_dir/nested.txt", b"nested\n")
                nested = sess.read("/sd0/host_test_dir/nested.txt")
                if nested != b"nested\n":
                    return (name, False, f"nested write/read mismatch: {nested!r}")

                # FAT32 keeps "." and ".." as real on-disk entries in every
                # subdirectory (never in a volume root, which is why /sd0
                # above shows no sign of this), and fs/9p.c used to stream
                # them straight onto the wire. A 9P2000 directory read
                # carries contents only -- the parent is reached by walking
                # the name "..", not by finding it in a listing -- so this
                # made every client's tree walk self-referential: fuse-p9
                # showed "." and ".." twice in `ls -la` of any subdirectory
                # (once from here, once from the pair FUSE adds itself), and
                # a recursive p9lib walk would never terminate. Found by
                # mounting a real board over TCP, 2026-09-01.
                sub = [e.name for e in sess.listdir("/sd0/host_test_dir")]
                if "." in sub or ".." in sub:
                    return (name, False,
                             f"subdirectory listing leaks FAT32's own dot entries: {sub}")
                if "NESTED.TXT" not in sub:
                    return (name, False, f"subdirectory listing lost its real entry: {sub}")

                sess.remove("/sd0/host_test_dir/nested.txt")
                sess.remove("/sd0/host_test_dir")
                sess.remove("/sd0/host_test.txt")

            return (name, True, "")
        except Exception as e:
            return (name, False, str(e))
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


def test_9p_between_nodes_over_tcp(rv64_elf: Path, rv32_elf: Path,
                                   img64: Path, img32: Path) -> tuple[str, bool, str]:
    """R3b, plan/phase19_ip_stack_and_ethernet.md: two LugalOS nodes on one
    virtual Ethernet segment, one mounting the other's namespace over TCP that
    both of them implement.

    QEMU's `-netdev socket,listen=` / `connect=` pair makes a **layer-2** link
    between the two guests -- a virtual segment, so ARP is real and both sides
    need addresses on one subnet. That is the netdev-layer twin of the chardev
    bridge test_9p_multinode_heterogeneous() already uses, so the orchestration
    here is that test's, one layer down. The two MACs are set explicitly:
    QEMU gives every virtio-net the same default, and two hosts sharing a MAC
    on one segment is a debugging afternoon nobody needs.

    **What this proves that the slirp test does not:** the client half of the
    state machine. SYN_SENT is never entered without an active open, and
    FIN_WAIT_1/FIN_WAIT_2/CLOSING/TIME_WAIT are only reachable from the side
    that closes first -- four of ten states that no server-only test can
    touch. It also exercises the in-kernel client's Tauth exchange, which
    phase 18 never built because until R3b no node could dial another.

    **And what it does not prove.** Our stack agreeing with our stack is a
    weaker oracle than our stack agreeing with something we did not write: a
    symmetric misunderstanding passes here and fails nowhere. That is why this
    does not replace test_9p_over_own_tcp() or test_tcp_state_machine() (a peer
    that can send what no correct stack would). It is an integration test, and
    it is labelled as one."""
    import shutil
    import socket as _socket

    name = "Two Nodes, One Segment: 9P Over Our Own TCP Both Ways (R3b)"
    probe = _socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    key = "000102030405060708090a0b0c0d0e0f"
    marker = "GREETINGS_FROM_NODE_B_OVER_IP"

    img_b = img64.with_name("test_tcpnode_b_sd.img")
    img_a = img32.with_name("test_tcpnode_a_sd.img")
    shutil.copyfile(img64, img_b)
    shutil.copyfile(img32, img_a)

    session_b = QemuSession(rv64_elf, img_b, "rv64")
    session_a = QemuSession(rv32_elf, img_a, "rv32")
    try:
        # Node B: the server. It holds the listening end of the virtual cable
        # *and* the 9P listener, but those are independent -- one is QEMU's
        # wiring, the other is ours.
        session_b.start(extra_qemu_args=[
            "-netdev", f"socket,id=n0,listen=127.0.0.1:{port}",
            "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:56",
        ])
        ok, log = session_b.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"Node B (RV64) did not boot: {log[-400:]}")

        ok, log = session_b.send_and_expect(
            'lisp\n(net-config "192.168.99.2" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.99\.2", timeout=6.0)
        if not ok:
            return (name, False, f"Node B refused its address: {log[-400:]}")
        ok, log = session_b.send_and_expect(f"p9key {key}", r"console key set", timeout=5.0)
        if not ok:
            return (name, False, f"Node B refused the key: {log[-400:]}")
        ok, log = session_b.send_and_expect(
            f'lisp\n(write-file "/ram0/hello.txt" "{marker}")\nexit', r"=> #t", timeout=5.0)
        if not ok:
            return (name, False, f"Node B could not write its marker: {log[-400:]}")

        # Node A: the client, on the dialling end of both cables.
        session_a.start(extra_qemu_args=[
            "-netdev", f"socket,id=n0,connect=127.0.0.1:{port}",
            "-device", "virtio-net-device,netdev=n0,mac=52:54:00:12:34:57",
        ])
        ok, log = session_a.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"Node A (RV32) did not boot: {log[-400:]}")
        ok, log = session_a.send_and_expect(
            'lisp\n(net-config "192.168.99.3" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.99\.3", timeout=6.0)
        if not ok:
            return (name, False, f"Node A refused its address: {log[-400:]}")

        # Without a key, the attach must be refused -- B's TCP link sets
        # auth_required, and a node that cannot authenticate must not get in.
        ok, log = session_a.send_and_expect(
            'lisp\n(net-mount "peer" "192.168.99.2" 564)\nexit',
            r"connected, but /peer could not be mounted", timeout=12.0)
        if not ok:
            return (name, False,
                    "an unauthenticated node mounted an auth-requiring peer:\n"
                    f"{log[-600:]}")

        ok, log = session_a.send_and_expect(f"p9key {key}", r"console key set", timeout=5.0)
        if not ok:
            return (name, False, f"Node A refused the key: {log[-400:]}")

        ok, log = session_a.send_and_expect(
            'lisp\n(net-mount "peer" "192.168.99.2" 564)\nexit',
            r"\[Net\] /peer mounted from 192\.168\.99\.2:564", timeout=12.0)
        if not ok:
            return (name, False, f"the authenticated mount failed: {log[-600:]}")

        ok, log = session_a.send_and_expect("cat /peer/ram0/hello.txt",
                                            re.escape(marker), timeout=8.0)
        if not ok:
            return (name, False, f"could not read B's file through the mount: {log[-500:]}")

        ok, log = session_a.send_and_expect("cat /peer/proc/version",
                                            expected_version(), timeout=8.0)
        if not ok:
            return (name, False, f"B's /proc/version did not come through: {log[-500:]}")

        # Both ends must agree about what happened, and neither may have
        # dropped anything on a conversation that worked.
        # **Two** accepted, and that is the interesting number: the refused
        # attempt got a perfectly good TCP connection and was turned away by
        # the auth gate above it, which is where a refusal belongs. The count
        # of *open* connections is deliberately not asserted -- the refused
        # one may or may not have left TIME_WAIT yet.
        ok, log = session_b.send_and_expect("cat /proc/net",
                                            r"tcp: listening on 564, \d+ open, 2 accepted", timeout=8.0)
        if not ok:
            return (name, False, f"Node B does not report two accepted connections: {log[-500:]}")
        if not re.search(r"192\.168\.99\.3:\d+ -> :564 ESTABLISHED", log):
            return (name, False, f"Node B does not show A as established: {log[-500:]}")

        ok, log = session_a.send_and_expect("cat /proc/net",
                                            r"192\.168\.99\.2:564 -> :\d+ ESTABLISHED", timeout=8.0)
        if not ok:
            return (name, False, f"Node A does not show its outbound connection: {log[-500:]}")

        # Unmounting closes it, gracefully -- which is the only thing in this
        # system that walks TCP's active-close path (FIN_WAIT_1 -> FIN_WAIT_2
        # -> TIME_WAIT). Asserted by the slot being free again: with two in
        # total, a connection that outlived its mount would make a node
        # undialable until it rebooted.
        ok, log = session_a.send_and_expect('lisp\n(unmount "peer")\nexit', r"=> #t", timeout=6.0)
        if not ok:
            return (name, False, f"unmount refused: {log[-400:]}")
        for _ in range(10):
            ok, log = session_a.send_and_expect("cat /proc/net", r"tcp: listening", timeout=8.0)
            if ok and not re.search(r"192\.168\.99\.2:564 -> :\d+ (ESTABLISHED|FIN_WAIT|CLOSING)", log):
                break
            time.sleep(0.5)
        else:
            return (name, False, f"the connection outlived its mount: {log[-500:]}")

        return (name, True, "rv32 mounted rv64's namespace over TCP, authenticated; "
                            "anonymous attempt refused; unmount closed the connection")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session_a.close()
        session_b.close()


def test_identity_record_auth(rv64_elf: Path, rv32_elf: Path,
                              img64: Path, img32: Path) -> tuple[str, bool, str]:
    """I4, plan/phase21_identity_and_authentication.md: "a node with a key
    only in its identity record authenticates to a peer" -- the same shape
    as test_9p_between_nodes_over_tcp() (R3b) above, deliberately, except
    neither node ever calls `p9key` (the console override) and neither has
    an SD-card key *file*. Node A's only key is what `identity key <hex>`
    (I3) wrote into its identity record -- fs/9p.c's p9_auth_own_key()
    (I5) must find it there when A proves itself as a client.

    Updated for I5: the earlier version of this test also gave Node B the
    *same* key in its own record and relied on p9_auth_key_for() (the
    server-side verifier) falling back to that record for any uname -- a
    real mechanism at I4, and exactly the conflation §1.2 says I5 must
    retire (a node's own key answering "who may attach to me" defeats
    §5.2's whole point: anyone who knows the segment's shared key would
    walk straight past every grant). Server-side verification is now the
    grants list's job, unconditionally -- so B grants A's key explicitly,
    with `peers add`, the same command an operator would use. `p9auth`
    (no args) confirms p9_auth_have_keys() sees that grant."""
    import shutil
    import socket as _socket

    name = "Identity Record As The Only Auth Key, Peer To Peer (I4)"
    key = "a1b2c3d4e5f60718293a4b5c6d7e8f90"

    # The same probe-a-free-port approach test_9p_between_nodes_over_tcp()
    # uses, on its own port so the two tests cannot collide if ever run
    # concurrently.
    probe = _socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    img_b = img64.with_name("test_idauth_b_sd.img")
    img_a = img32.with_name("test_idauth_a_sd.img")
    shutil.copyfile(img64, img_b)
    shutil.copyfile(img32, img_a)
    id_b = img64.with_name("test_idauth_b_id.img")
    id_a = img32.with_name("test_idauth_a_id.img")
    id_b.write_bytes(b"\x00" * 4096)
    id_a.write_bytes(b"\x00" * 4096)

    marker = "GREETINGS_FROM_NODE_B_VIA_RECORD_KEY"

    session_b = QemuSession(rv64_elf, img_b, "rv64")
    session_a = QemuSession(rv32_elf, img_a, "rv32")
    try:
        session_b.start(identity_img_path=id_b, extra_qemu_args=[
            "-netdev", f"socket,id=n0,listen=127.0.0.1:{port}",
            "-device", "virtio-net-device,netdev=n0,mac=52:54:00:aa:bb:01",
        ])
        ok, log = session_b.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"Node B (RV64) did not boot: {log[-400:]}")

        ok, log = session_b.send_and_expect(
            'lisp\n(net-config "192.168.98.2" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.98\.2", timeout=6.0)
        if not ok:
            return (name, False, f"Node B refused its address: {log[-400:]}")

        ok, log = session_b.send_and_expect(f"identity key {key}", r"key fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"Node B's identity key install failed: {log[-400:]}")

        # I5: B's own record key proves B to others (untouched by this
        # test); B must separately GRANT A's key to accept A's attach.
        # Wildcard, since A's derived name varies by build/architecture and
        # this test is about the grant mechanism, not name matching.
        ok, log = session_b.send_and_expect(
            'lisp\n(format "/sd0")\nexit', r"=> #t", timeout=6.0)
        if not ok:
            return (name, False, f"Node B could not format /sd0: {log[-400:]}")
        ok, log = session_b.send_and_expect(f"peers add * {key}", r"granted", timeout=6.0)
        if not ok:
            return (name, False, f"Node B's grant for A did not take: {log[-400:]}")

        ok, log = session_b.send_and_expect("p9auth", r"Keys configured: yes", timeout=5.0)
        if not ok:
            return (name, False, f"p9auth does not see the grant: {log[-500:]}")

        ok, log = session_b.send_and_expect(
            f'lisp\n(write-file "/ram0/hello.txt" "{marker}")\nexit', r"=> #t", timeout=5.0)
        if not ok:
            return (name, False, f"Node B could not write its marker: {log[-400:]}")

        session_a.start(identity_img_path=id_a, extra_qemu_args=[
            "-netdev", f"socket,id=n0,connect=127.0.0.1:{port}",
            "-device", "virtio-net-device,netdev=n0,mac=52:54:00:aa:bb:02",
        ])
        ok, log = session_a.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        if not ok:
            return (name, False, f"Node A (RV32) did not boot: {log[-400:]}")
        ok, log = session_a.send_and_expect(
            'lisp\n(net-config "192.168.98.3" "255.255.255.0")\nexit',
            r"\[Net\] 192\.168\.98\.3", timeout=6.0)
        if not ok:
            return (name, False, f"Node A refused its address: {log[-400:]}")

        ok, log = session_a.send_and_expect(f"identity key {key}", r"key fingerprint:", timeout=6.0)
        if not ok:
            return (name, False, f"Node A's identity key install failed: {log[-400:]}")

        # Neither node's raw key ever appears anywhere but this one line of
        # input echo -- the same "no command prints a key" property I3's own
        # test checks, exercised here against the path that actually
        # authenticates a peer rather than just a self-test.
        if log.count(key) != 1:
            return (name, False, f"the raw key appeared {log.count(key)} times installing it on A")

        ok, log = session_a.send_and_expect(
            'lisp\n(net-mount "peer" "192.168.98.2" 564)\nexit',
            r"\[Net\] /peer mounted from 192\.168\.98\.2:564", timeout=12.0)
        if not ok:
            return (name, False,
                    f"the record-key-only mount failed: {log[-600:]}")

        ok, log = session_a.send_and_expect("cat /peer/ram0/hello.txt",
                                            re.escape(marker), timeout=8.0)
        if not ok:
            return (name, False, f"could not read B's file through the mount: {log[-500:]}")

        return (name, True, "A proved itself via its identity record alone (no p9key); "
                            "B accepted it via an explicit grant (I5)")
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")
    finally:
        session_a.close()
        session_b.close()


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



def test_smp_two_harts(elf_path: Path, img_path: Path) -> list[tuple[str, bool, str]]:
    """X1, plan/phase23_multicore_scheduling.md: a genuinely two-hart kernel.

    Its own target, from its own build (`cmake --preset rv64-smp`), because
    CONFIG_ENABLE_SMP is off everywhere else on purpose -- every board persona
    boots on one hart and a regression in second-hart bring-up must not be
    able to touch that. So this is skipped rather than failed when that build
    is absent.

    Three claims, in increasing order of what they cost to establish:

      1. A second hart reaches the scheduler at all. /proc/cpuinfo counts
         harts that got as far as sched_secondary_init(), so "2" is reported
         by code running on hart 1.
      2. Two tasks run concurrently and the lock holds. smptest increments a
         shared counter through a spinlock from four workers and asserts no
         lost updates, AND that the work executed on more than one hart -- the
         part that distinguishes concurrency from the interleaving a single
         preemptive hart already does. It also increments an unlocked counter
         and reports its loss, which is what proves there was real contention
         rather than an exactness that came for free.
      3. Phase 22's scheduler hand-off survives real contention. lockselftest's
         seventh check (S6) counts resumes that arrived without the lock their
         predecessor should have handed over. On one hart that check can only
         catch a coding mistake; here it is finally exposed to the race it was
         written for.
    """
    out: list[tuple[str, bool, str]] = []
    session = QemuSession(elf_path, img_path, "rv64-smp")
    try:
        session.start(extra_qemu_args=["-smp", "2"])
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=8.0)
        out.append(("SMP: two-hart kernel boots to a shell (X1)", ok, log if not ok else ""))

        # A hart that owns no task says so, instead of claiming to be task 0.
        #
        # secondary_main() printk()s from the window between the reset vector
        # and sched_secondary_init(), where this hart has no task slot. Until
        # the identity fix, sched_current_pid() answered 0 there -- naming the
        # boot task, which is running on the *other* hart at that instant. The
        # consequences needed a race to show (printk_lock() taking the
        # re-entrant path through a lock hart 0 held; task_block() blocking the
        # shell), but the cause does not: the pid is printed, so `pid -1` and
        # `pid 0` separate fixed from broken in the boot log with no race to
        # reproduce. Falsified by reverting only that one initialiser, which
        # turns this line back into `pid 0`.
        #
        # It also establishes that printk() works at all from a task-less
        # hart, which is what makes second-core bring-up debuggable -- X3 got
        # its core running only by giving it no printk whatsoever.
        ok = "[SMP] hart 1: in the kernel, no task yet (pid -1)" in log
        out.append(("SMP: a hart with no task reports no pid, not task 0's (identity)",
                    ok, log if not ok else ""))

        # A longer window than the same read gets on a single-hart target: two
        # harts boot noisily and this command can land while the tail of that
        # is still streaming, which cost an intermittent failure at 5 s on a
        # kernel that reports the right number every time when asked
        # (verified: 5 consecutive reads, no interleaving).
        ok, log = session.send_and_expect("cat /proc/cpuinfo", r"harts_online:\s*2", timeout=12.0)
        out.append(("SMP: a second hart reached the scheduler (X1)", ok, log if not ok else ""))

        ok, log = session.send_and_expect("smptest", r"SMP_SELFTEST_(OK|FAIL)", timeout=60.0)
        ok = ok and "SMP_SELFTEST_OK" in log
        out.append(("SMP: no lost updates through the lock, on two harts (X1)", ok,
                    log if not ok else ""))

        ok, log = session.send_and_expect("lockselftest", r"LOCK_SELFTEST_(OK|FAIL)", timeout=45.0)
        ok = ok and "LOCK_SELFTEST_OK" in log
        out.append(("SMP: the scheduler lock hand-off holds under real contention (S6/X1)",
                    ok, log if not ok else ""))

        # X2: per-task isolation, with the domain activated on a hart that is
        # not the primary.
        #
        # "It still passes" is not enough on its own here. Driver tasks are
        # pinned to hart 0, so a run could satisfy every isolation check
        # while never once installing a restricted domain anywhere else --
        # and would then prove nothing about the claim §1 makes. So this
        # asserts the fault AND the evidence: /proc/cpuinfo counts
        # restricted-domain activations per hart, and a non-zero count for
        # hart 1 means the isolation being checked was installed by code
        # running there.
        ok, log = session.send_and_expect(
            "exec /flash0/system/bin/uisolate.elf\nps", r"uprog\s+killed", timeout=30.0)
        out.append(("SMP: an out-of-domain store still faults, on a two-hart kernel (X2)",
                    ok, log if not ok else ""))

        ok, log = session.send_and_expect("cat /proc/cpuinfo",
                                          r"domains_hart1:\s*[1-9]", timeout=12.0)
        out.append(("SMP: restricted domains were actually activated on hart 1 (X2)",
                    ok, log if not ok else ""))

        # ---- X5: the isolation and fault suite, on a machine that is
        # actually using both cores at the moment each fault lands.
        #
        # Everything above this point would pass on a kernel where the second
        # hart booted and then idled forever. That is the gap X5 exists to
        # close, and it takes two separate mechanisms, because two different
        # things could be untrue:
        #
        #   `smpload start` puts four yielding tasks on the ready queue and
        #   keeps a per-hart progress counter. The trap handler snapshots
        #   those counters *before* killing a faulting task, so `smpload stop`
        #   can assert that a hart other than the one taking the fault had
        #   made progress before the fault and made more after it -- a claim
        #   about simultaneity, which no pair of passing tests can establish.
        #
        #   `isolationtest <hart>` pins the probe, so "a domain fault is
        #   handled correctly on a hart that is not the primary" stops being
        #   a lucky draw. Unpinned, the probe lands wherever the ready queue
        #   sends it; on a quiet machine that is usually the shell's own hart,
        #   and a green run would have proved nothing about hart 1.
        ok, log = session.send_and_expect("smpload start", r"SMPLOAD_STARTED tasks=[1-9]",
                                          timeout=30.0)
        out.append(("SMP: a background load is running on every hart (X5)",
                    ok, log if not ok else ""))

        # The pinned half, both directions. Running it on hart 0 as well is
        # not symmetry for its own sake: it is what shows the pin is a real
        # constraint rather than a label, since hart 0 is the busy one and an
        # unpinned probe would rarely choose it.
        for hart in (1, 0):
            ok, log = session.send_and_expect(
                f"isolationtest {hart}",
                r"ISOLATED \(kernel memory untouched\)", timeout=25.0)
            if ok and f"ran in U-mode on hart {hart}" not in log:
                ok = False   # it faulted, but not where we asked -- see above
            if "NOT the hart it was pinned to" in log:
                ok = False
            out.append((f"SMP: a domain fault is contained when taken on hart {hart} (X5)",
                        ok, log if not ok else ""))

        # The syscall boundary, and an ordinary U-mode round trip, under the
        # same load and on the non-primary hart -- the two halves of phase
        # 12's suite that are about the kernel *not* faulting. A confused
        # deputy check that only ever ran on hart 0 says nothing about the
        # copy-in path a task on hart 1 uses.
        ok, log = session.send_and_expect("deputytest 1",
                                          r"DEPUTY_REFUSED.*OWNBUF_OK.*UNTOUCHED",
                                          timeout=25.0)
        ok = ok and "ran in U-mode on hart 1" in log
        out.append(("SMP: the syscall boundary rejects a foreign pointer, on hart 1 (X5)",
                    ok, log if not ok else ""))

        ok, log = session.send_and_expect("usertest 1",
                                          r"UMODE_OK.*cause: 8 \(U-mode.*ended cleanly",
                                          timeout=25.0)
        ok = ok and "ran in U-mode on hart 1" in log
        out.append(("SMP: a U-mode task syscalls back cleanly from hart 1 (X5)",
                    ok, log if not ok else ""))

        # A loaded ELF, not a kernel-linked probe: the same distinction B6
        # drew when uisolate.elf was added, now under load. Left unpinned on
        # purpose -- the loader has no pinning API and should not need one,
        # so this is the case where the scheduler chooses.
        ok, log = session.send_and_expect(
            "exec /flash0/system/bin/uisolate.elf\nps",
            r"uprog\s+killed", timeout=30.0)
        if "UISO_NOT_ISOLATED" in log:
            ok = False
        out.append(("SMP: a loaded ELF is still confined, and still reported killed (X5)",
                    ok, log if not ok else ""))

        # The verdict. Reads the counters before stopping the load, so
        # "the other hart kept going after the fault" cannot be satisfied by
        # progress made between the fault and the word `stop`.
        ok, log = session.send_and_expect("smpload stop", r"SMPLOAD_(OK|FAIL)", timeout=45.0)
        ok = ok and "SMPLOAD_OK" in log
        out.append(("SMP: another hart was mid-task at the instant of each fault (X5)",
                    ok, log if not ok else ""))

        # And the machine is still usable afterwards. Every fault above was
        # survivable in isolation; this asks whether five of them, taken on
        # two harts with a load running, left a shell that still answers --
        # which is the actual claim "the fault suite passes on two cores"
        # is making.
        ok, log = session.send_and_expect("cat /proc/cpuinfo", r"harts_online:\s*2",
                                          timeout=15.0)
        out.append(("SMP: both harts still scheduling after the fault suite (X5)",
                    ok, log if not ok else ""))

        # ---- X8: the second core doing visible work, checked against a
        # published answer rather than against itself.
        #
        # perft's node counts are exact and externally documented, and
        # perft.c's table already carries them. So splitting the root move
        # list across two harts is verified by the same assertion that
        # verifies the single-core path: any mistake in the split -- a
        # dropped root move, a shared ply index, two workers on one board --
        # changes the count, and the count is checked against the table.
        # Nothing else in this tree validates a concurrency change that
        # cleanly, which is why X8 leads with perft rather than the search.
        #
        # Both core counts are run. One core is the pre-X8 path and must
        # still be right; two cores must produce the identical answer AND
        # report that it really used two, so a silent fallback to one cannot
        # pass for a working split.
        ok, log = session.send_and_expect("(perft 3 1)",
                                          r"PERFT Results: \d+ passed depths, 0 errors "
                                          r"\(cores used: 1, \d+ ms\)", timeout=120.0)
        out.append(("SMP: perft is still exact on one core (X8)", ok, log if not ok else ""))

        ok, log = session.send_and_expect("(perft 3 2)",
                                          r"PERFT Results: \d+ passed depths, 0 errors "
                                          r"\(cores used: 2, \d+ ms\)", timeout=120.0)
        out.append(("SMP: perft split across two harts gives the same exact counts (X8)",
                    ok, log if not ok else ""))

        # X8b: the search's second core, asserted on what it did rather than
        # on how long it took.
        #
        # A Lazy SMP helper contributes only through the shared transposition
        # table and its own result is discarded, so there is no output that is
        # *supposed* to change -- and the timing that does change is exactly
        # the kind of wall-clock assertion this suite has been bitten by
        # before. What is checkable is that the helper ran at all: it reports
        # its own node count, kept separate from the primary's precisely so
        # this question stays answerable. Zero means it never started, which
        # is what a silent fallback to one core looks like.
        ok, log = session.send_and_expect("(chess-selftest 1)",
                                          r"cores requested 1, harts online 2, "
                                          r"helper nodes 0", timeout=60.0)
        out.append(("SMP: chess on one core runs no helper (X8b)", ok, log if not ok else ""))

        ok, log = session.send_and_expect("(chess-selftest 2)",
                                          r"cores requested 2, harts online 2, "
                                          r"helper nodes [1-9]\d+", timeout=60.0)
        out.append(("SMP: chess on two cores really searches on the second (X8b)",
                    ok, log if not ok else ""))
    except Exception as e:  # noqa: BLE001
        out.append(("SMP two-hart target", False, str(e)))
    finally:
        session.close()
    return out


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

    # I6, plan/phase21_identity_and_authentication.md: the host half of the
    # WLAN PBKDF2-HMAC-SHA1 derivation, no QEMU involved -- same shape as
    # the FAT32 inspection just above.
    ok, info = test_host_wpa2_psk_derivation()
    total_tests += 1
    name = "WPA2 PSK Derivation Against The IEEE 802.11i Worked Example (I6)"
    if ok:
        passed_tests += 1
        print(f"  [PASS] {name}")
    else:
        print(f"  [FAIL] {name}\n    Log Output:\n{info}")

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
        _run_single(test_9p_crud_via_p9lib(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_9p_iounit(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_9p_auth_gate(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_identity_grants_scope(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_identity_grants_cap(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_node_identity(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_identity_store_provisioning(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_identity_toolset(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_grants_in_record(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_wlan_credential_roundtrip(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_network_autoconfig(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_netif_virtio_net(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_ip_stack(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_ntp_server(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_ntp_client(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_tcp_state_machine(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_tcp_stream(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_tcp_under_impairment(rv64_elf, img_for("rv64"), "rv64"))
        _run_single(test_9p_over_own_tcp(rv64_elf, img_for("rv64"), "rv64"))
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
        _run_single(test_9p_crud_via_p9lib(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_iounit(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_auth_gate(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_identity_grants_scope(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_identity_grants_cap(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_node_identity(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_identity_store_provisioning(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_identity_toolset(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_grants_in_record(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_wlan_credential_roundtrip(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_network_autoconfig(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_netif_virtio_net(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_ip_stack(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_ntp_server(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_ntp_client(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_tcp_state_machine(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_tcp_stream(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_tcp_under_impairment(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_over_own_tcp(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_uart_slip_link(rv32_elf, img_for("rv32"), "rv32"))
        _run_single(test_9p_uart_demux_shared_wire(rv32_elf, img_for("rv32"), "rv32"))
    else:
        print(f"\n[!] RV32 binary not found at '{rv32_elf}'. Skipping RV32 tests.")

    # 4. A4/T2: multi-node heterogeneous interconnect
    if rv64_elf.exists() and rv32_elf.exists():
        print("\n[Target: Multi-Node RV32 <-> RV64 Heterogeneous Interconnect]")
        _run_single(test_9p_multinode_heterogeneous(rv64_elf, rv32_elf, img_for("rv64"), img_for("rv32")))
        _run_single(test_9p_remote_mount(rv64_elf, rv32_elf, img_for("rv64"), img_for("rv32")))
        _run_single(test_9p_between_nodes_over_tcp(rv64_elf, rv32_elf, img_for("rv64"), img_for("rv32")))
        _run_single(test_identity_record_auth(rv64_elf, rv32_elf, img_for("rv64"), img_for("rv32")))
    else:
        print("\n[!] RV64 and/or RV32 binary not found. Skipping multi-node test.")

    # 5. X1: the two-hart target. Built by `cmake --preset rv64-smp`; absent
    # from a default checkout, and skipped rather than failed when so.
    smp_elf = build_dir / "rv64-smp" / "lugalos.elf"
    if smp_elf.exists():
        print("\n[Target: RV64 SMP -- two harts]")
        for r in test_smp_two_harts(smp_elf, build_dir / "rv64-smp" / "lugalos_sd.img"):
            _run_single(r)
    else:
        print("\n[i] build/rv64-smp not present; skipping the two-hart target "
              "(cmake --preset rv64-smp && cmake --build --preset rv64-smp)")

    duration = time.time() - start_time
    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed_tests} / {total_tests} Tests PASSED ({duration:.2f}s)")
    print("======================================================================\n")

    return 0 if (passed_tests == total_tests and total_tests > 0) else 1


if __name__ == "__main__":
    sys.exit(main())
