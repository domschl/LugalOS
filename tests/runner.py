#!/usr/bin/env python3
"""Automated Test Runner & Inspection Framework for LugalOS Microkernel Operating System."""

from __future__ import annotations

import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path


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
        self.process: subprocess.Popen[str] | None = None
        self.output_queue: queue.Queue[str] = queue.Queue()
        self._reader_thread: threading.Thread | None = None
        self._running: bool = False

    def _reader(self) -> None:
        if not self.process or not self.process.stdout:
            return
        while self._running:
            try:
                char = self.process.stdout.read(1)
                if not char:
                    break
                self.output_queue.put(char)
            except Exception:
                break

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

        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._running = True
        self._reader_thread = threading.Thread(target=self._reader, daemon=True)
        self._reader_thread.start()

    def _drain(self) -> None:
        """Discard any output left over from a previous call. Without this, a
        pattern from the *next* test's own echoed command (or trailing output
        the previous test never consumed) can bleed in and produce a false
        PASS before the new command has even been sent."""
        while True:
            try:
                self.output_queue.get_nowait()
            except queue.Empty:
                break

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
        if not self.process or not self.process.stdin:
            return False, "Process not running"

        self._drain()

        if command:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()

        accumulated: list[str] = []
        start_time = time.time()
        regex = re.compile(expected_pattern, re.MULTILINE | re.DOTALL)

        while time.time() - start_time < timeout:
            try:
                char = self.output_queue.get(timeout=0.05)
                accumulated.append(char)
                text = "".join(accumulated)

                if any(marker in text for marker in self.FAULT_MARKERS):
                    return False, text

                visible = self._strip_echo(text, command)
                if regex.search(visible):
                    return True, text
            except queue.Empty:
                pass

        return False, "".join(accumulated)

    def close(self) -> None:
        self._running = False
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=1.0)
            except Exception:
                self.process.kill()
            self.process = None


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
        ok, log = session.send_and_expect("cat /proc/version", r"LugalOS v0\.5\.0", timeout=3.0)

        results.append(("/proc/version Metrics", ok, log if not ok else ""))

        ok, log = session.send_and_expect("cat /proc/ps", r"vfs_server", timeout=3.0)
        results.append(("/proc/ps Task Listing", ok, log if not ok else ""))

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
        ok, log = session.send_and_expect(cmd_cc, r"returned: 0", timeout=5.0)
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
            "(p9-cat \"/sd0/system/init.lisp\")\n"
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
            "(load \"/sd0/system/stdlib.lisp\")\n"
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
        # "kernel_idle" only appears in real /proc/ps content (A1) -- this also
        # exercises that (ps)/(meminfo)/(version) now read real /proc byte
        # streams via the VFS handle API instead of the old printk-side-effect
        # path (which this pattern used to accidentally pass through, via a
        # generic directory listing containing the word "synthetic").
        ok, log = session.send_and_expect(cmd_lisp_vfs, r"kernel_idle", timeout=5.0)
        results.append(("Lisp Microkernel VFS Primitives (mkdir, write, cp, cat, rm, ps, meminfo)", ok, log if not ok else ""))


        # 10. Lisp Compiler & Native Binary Primitives (cc, exec)
        cmd_lisp_cc = (
            "(cc \"/sd0/hello.c\" \"/sd0/hello_lisp.elf\")\n"
            "(exec \"/sd0/hello_lisp.elf\")"
        )
        ok, log = session.send_and_expect(cmd_lisp_cc, r"returned: 0", timeout=5.0)
        results.append(("Lisp Compiler & Binary Exec Primitives (cc, exec)", ok, log if not ok else ""))

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


    finally:
        session.close()

    return results


def test_9p_virtio_link(elf_path: Path, img_path: Path, arch_name: str) -> tuple[str, bool, str]:
    """A3: proves the 9P server is reachable over a real, separate wire
    (drivers/virtio_console.c) by an external host process -- not any of
    LugalOS's own in-kernel 9P client code (drivers/loopback_net.c /
    drivers/uart_net.c). Boots its own dedicated QEMU instance with a
    virtio-serial chardev backed by a unix socket, connects tests/p9lib.py's
    independent Python 9P client to it, and reads a real pre-existing file
    (/sd0/system/init.lisp) end to end: Tversion, Tattach("/"), a
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
                data = client.cat("/sd0/system/init.lisp")
            finally:
                client.close()

            ok = b"LugalOS System Initialization Script" in data and len(data) == 515
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
            data = client.cat("/sd0/system/init.lisp")

            ok = b"LugalOS System Initialization Script" in data and len(data) == 515
            log = "" if ok else f"unexpected content ({len(data)} bytes): {data[:120]!r}"
            return (name, ok, log)
        except Exception as e:
            return (name, False, str(e))
        finally:
            if raw_sock:
                raw_sock.close()
            session.close()


def test_9p_multinode_heterogeneous(rv64_elf: Path, rv32_elf: Path, img_path: Path) -> tuple[str, bool, str]:
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

    img_b = img_path.with_name("test_multinode_b_sd.img")
    img_a = img_path.with_name("test_multinode_a_sd.img")
    shutil.copyfile(img_path, img_b)
    shutil.copyfile(img_path, img_a)

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

        cmd = 'lisp\n(p9-remote-cat "/ram0/multinode_marker.txt")\nexit'
        ok, log = session_a.send_and_expect(cmd, re.escape(marker), timeout=5.0)
        return (name, ok, log if not ok else "")
    except Exception as e:
        return (name, False, str(e))
    finally:
        session_a.close()
        session_b.close()


def test_9p_remote_mount(rv64_elf: Path, rv32_elf: Path, img_path: Path) -> tuple[str, bool, str]:
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

    img_b = img_path.with_name("test_mount_b_sd.img")
    img_a = img_path.with_name("test_mount_a_sd.img")
    shutil.copyfile(img_path, img_b)
    shutil.copyfile(img_path, img_a)

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

        cmd = 'lisp\n(mount-remote "netb")\nexit'
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
    img_path = build_dir / "lugalos_sd.img"

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
    ok, info = test_host_fat32_image(img_path)
    total_tests += 1
    if ok:
        passed_tests += 1
        print(f"  [PASS] {info}")
    else:
        print(f"  [FAIL] {info}")

    # 2. RV64 Target
    if rv64_elf.exists():
        print("\n[Target: RV64 Sv39 MMU Virtual Memory]")
        rv64_results = test_qemu_architecture(rv64_elf, img_path, "rv64")
        for name, ok, log in rv64_results:
            total_tests += 1
            if ok:
                passed_tests += 1
                print(f"  [PASS] {name}")
            else:
                print(f"  [FAIL] {name}\n    Log Output:\n{log}")

        _run_single(test_9p_virtio_link(rv64_elf, img_path, "rv64"))
        _run_single(test_9p_uart_slip_link(rv64_elf, img_path, "rv64"))
    else:
        print(f"\n[!] RV64 binary not found at '{rv64_elf}'. Skipping RV64 tests.")

    # 3. RV32 Target
    if rv32_elf.exists():
        print("\n[Target: RV32 NOMMU Microcontroller]")
        rv32_results = test_qemu_architecture(rv32_elf, img_path, "rv32")
        for name, ok, log in rv32_results:
            total_tests += 1
            if ok:
                passed_tests += 1
                print(f"  [PASS] {name}")
            else:
                print(f"  [FAIL] {name}\n    Log Output:\n{log}")

        _run_single(test_9p_virtio_link(rv32_elf, img_path, "rv32"))
        _run_single(test_9p_uart_slip_link(rv32_elf, img_path, "rv32"))
    else:
        print(f"\n[!] RV32 binary not found at '{rv32_elf}'. Skipping RV32 tests.")

    # 4. A4/T2: multi-node heterogeneous interconnect
    if rv64_elf.exists() and rv32_elf.exists():
        print("\n[Target: Multi-Node RV32 <-> RV64 Heterogeneous Interconnect]")
        _run_single(test_9p_multinode_heterogeneous(rv64_elf, rv32_elf, img_path))
        _run_single(test_9p_remote_mount(rv64_elf, rv32_elf, img_path))
    else:
        print("\n[!] RV64 and/or RV32 binary not found. Skipping multi-node test.")

    duration = time.time() - start_time
    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed_tests} / {total_tests} Tests PASSED ({duration:.2f}s)")
    print("======================================================================\n")

    return 0 if (passed_tests == total_tests and total_tests > 0) else 1


if __name__ == "__main__":
    sys.exit(main())
