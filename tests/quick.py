#!/usr/bin/env python3
"""quick.py -- tiers 0 and 1 of the test plan (doc/dev/testing.md): one QEMU
target, only what the change touches.

    python3 tests/quick.py --list                    # the groups and what they cover
    python3 tests/quick.py settings reader           # those groups on rv32
    python3 tests/quick.py --target rv64 screen      # on another target
    python3 tests/quick.py --run "vtselftest" --run "settings view"
                                                     # commands, their output printed
    python3 tests/quick.py --build settings          # ninja the target first

Groups are tests/qemu_groups.py's, the same functions tests/runner.py (tier 3)
runs, so a quick pass and the full pass check the same thing. The target's
SD image is copied first; the build tree is never written to. Exit status 0
when every check passed.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))

import qemu_groups  # noqa: E402
from runner import QemuSession  # noqa: E402

# target -> QEMU architecture (the build directory is the target's name)
TARGETS = {"rv32": "rv32", "rv64": "rv64", "rv32-fold": "rv32"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("groups", nargs="*", help="groups to run (--list shows them)")
    ap.add_argument("--target", default="rv32", choices=sorted(TARGETS), help="QEMU target (default rv32)")
    ap.add_argument("--run", action="append", default=[], metavar="CMD",
                    help="a shell command to send, its output printed (repeatable; after the groups)")
    ap.add_argument("--build", action="store_true", help="ninja -C build/<target> first")
    ap.add_argument("--list", action="store_true", help="list the groups and exit")
    args = ap.parse_args()

    if args.list:
        for name, (what, checks) in qemu_groups.GROUPS.items():
            print(f"{name:10} {len(checks):2} checks  {what}")
        return 0
    unknown = [g for g in args.groups if g not in qemu_groups.GROUPS]
    if unknown:
        ap.error(f"no group {', '.join(unknown)} (--list shows them)")
    if not args.groups and not args.run:
        ap.error("name a group, or --run a command")

    build = ROOT / "build" / args.target
    if args.build:
        r = subprocess.run(["ninja", "-C", str(build)], capture_output=True, text=True)
        warn = [l for l in r.stdout.splitlines() if ("warning:" in l or "error" in l) and "empty loadable segment" not in l]
        if r.returncode != 0 or warn:
            print("\n".join(warn) or r.stdout[-2000:])
            return 1
    elf, img = build / "lugalos.elf", build / "lugalos_sd.img"
    if not elf.exists() or not img.exists():
        print(f"quick: {elf} or {img} missing -- build the target first (--build)")
        return 1

    t0 = time.time()
    total = passed = 0
    with tempfile.TemporaryDirectory(prefix="lugalos-quick-") as tmp:
        copy = Path(tmp) / "sd.img"
        shutil.copyfile(img, copy)
        session = QemuSession(elf, copy, TARGETS[args.target])
        session.start()
        try:
            ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=10.0)
            if not ok:
                print(f"quick: {args.target} did not reach the shell:\n{log[-2000:]}")
                return 1
            for g in args.groups:
                what, checks = qemu_groups.GROUPS[g]
                print(f"[{g}] {what}")
                for check in checks:
                    name, ok, log = check(session)
                    total += 1
                    passed += ok
                    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + ("" if ok else f"\n{log}"))
            for cmd in args.run:
                _, out = session.send_and_expect(cmd, r"lsh> ", timeout=15.0)
                print(f"### {cmd}\n{out}")
        finally:
            session.close()
    if total:
        print(f"quick: {passed} / {total} passed on {args.target} ({time.time() - t0:.0f} s)")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
