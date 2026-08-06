#!/usr/bin/env python3
"""Automated Test Runner & Inspection Framework for LugalOS Microkernel Operating System."""

from __future__ import annotations

import os
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path


class QemuSession:
    """Manages an interactive QEMU session for testing LugalOS serial I/O."""

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

    def start(self) -> None:

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

    def send_and_expect(self, command: str, expected_pattern: str, timeout: float = 4.0) -> tuple[bool, str]:
        if not self.process or not self.process.stdin:
            return False, "Process not running"
        
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
                if regex.search(text):
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
        ok, log = session.send_and_expect("cat /proc/version", r"LugalOS v0\.4\.0", timeout=3.0)

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
        cmd_9p = (
            "write /srv/loopback_9p 9P_VFS_WRITE_OK\n"
            "cat /srv/loopback_9p\n"
            "lisp\n"
            "(9p-loopback \"9P_LISP_RPC_PASSED\")\n"
            "exit"
        )
        ok, log = session.send_and_expect(cmd_9p, r"9P_LISP_RPC_PASSED", timeout=4.0)
        results.append(("9P2000 Protocol & Loopback Transport (Phase 1)", ok, log if not ok else ""))


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
        ok, log = session.send_and_expect(cmd_lisp_vfs, r"synthetic", timeout=5.0)
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


    finally:
        session.close()

    return results






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
    else:
        print(f"\n[!] RV32 binary not found at '{rv32_elf}'. Skipping RV32 tests.")

    duration = time.time() - start_time
    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed_tests} / {total_tests} Tests PASSED ({duration:.2f}s)")
    print("======================================================================\n")

    return 0 if (passed_tests == total_tests and total_tests > 0) else 1


if __name__ == "__main__":
    sys.exit(main())
