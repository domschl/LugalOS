#!/usr/bin/env python3
"""Run LugalOS on QEMU, execute a command or Lisp expression, capture a screenshot,
and retrieve it via 9P virtio-console, converting it to PNG.

Allows fast GUI iteration without requiring physical hardware.

Usage:
    python3 tools/qemu_screenshot.py --cmd "(chess-board-selftest)" --out board.png
    python3 tools/qemu_screenshot.py --commands "chess" "level 1" "e2e4" --out chess.png
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tests"))
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))

import runner  # noqa: E402
import p9lib  # noqa: E402


def capture_qemu_screenshot(
    commands: list[str],
    out_path: Path,
    arch: str = "rv64",
    elf_path: Path | None = None,
    img_path: Path | None = None,
) -> bool:
    if elf_path is None:
        elf_path = REPO_ROOT / "build" / arch / "lugalos.elf"
    if img_path is None:
        img_path = REPO_ROOT / "build" / arch / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Error: ELF not found at {elf_path}. Run `ninja -C build/{arch}` first.", file=sys.stderr)
        return False
    if not img_path.exists():
        print(f"Error: SD image not found at {img_path}.", file=sys.stderr)
        return False

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_dir = Path(tmpdir)
        temp_img = tmp_dir / "qemu_sd.img"
        shutil.copyfile(img_path, temp_img)

        sock_path = str(tmp_dir / "p9c.sock")
        session = runner.QemuSession(elf_path, temp_img, arch)

        try:
            print(f"Starting QEMU ({arch})...")
            session.start(extra_qemu_args=[
                "-device", "virtio-serial-device",
                "-device", "virtconsole,chardev=p9c",
                "-chardev", f"socket,id=p9c,path={sock_path},server=on,wait=off",
            ])

            ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=10.0)
            if not ok:
                print(f"Error: failed to boot shell:\n{log}", file=sys.stderr)
                return False

            client = None
            for _ in range(25):
                try:
                    client = p9lib.connect_unix(sock_path, timeout=2.0)
                    break
                except (FileNotFoundError, ConnectionRefusedError, OSError):
                    time.sleep(0.2)

            if client is None:
                print("Error: could not connect to 9P virtio socket.", file=sys.stderr)
                return False

            for cmd in commands:
                print(f"Sending: {cmd}")
                ok, log = session.send_and_expect(cmd, r"(lsh>|chess>|=>)", timeout=30.0)
                if not ok:
                    print(f"Warning: command {cmd!r} timed out or unexpected reply:\n{log}")

            shot_pbm = "/sd0/shot_qemu.pbm"
            print(f"Taking screenshot to {shot_pbm}...")
            ok, log = session.send_and_expect(f'(screenshot "{shot_pbm}")', r"=>", timeout=15.0)

            # Retrieve screenshot via 9P
            print("Retrieving screenshot via 9P...")
            pbm_data = client.cat(shot_pbm)
            client.close()

            # Save PBM and convert to PNG
            tmp_pbm = tmp_dir / "shot.pbm"
            tmp_pbm.write_bytes(pbm_data)

            im = Image.open(tmp_pbm)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            im.save(out_path)
            print(f"Successfully saved screenshot to {out_path} ({im.width}x{im.height})")
            return True

        finally:
            session.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture LugalOS screenshot on QEMU via 9P.")
    parser.add_argument("--cmd", type=str, help="Single command to run before screenshot")
    parser.add_argument("--commands", nargs="+", help="Multiple commands to run in sequence")
    parser.add_argument("--out", type=Path, default=Path("screenshot.png"), help="Output PNG file path")
    parser.add_argument("--arch", type=str, default="rv64", choices=["rv64", "rv32"], help="Architecture target")
    parser.add_argument("--elf", type=Path, help="Explicit path to lugalos.elf")
    parser.add_argument("--img", type=Path, help="Explicit path to lugalos_sd.img")

    args = parser.parse_args()

    commands = []
    if args.cmd:
        commands.append(args.cmd)
    if args.commands:
        commands.extend(args.commands)
    if not commands:
        commands = ["(chess-board-selftest)"]

    success = capture_qemu_screenshot(
        commands=commands,
        out_path=args.out,
        arch=args.arch,
        elf_path=args.elf,
        img_path=args.img,
    )
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
