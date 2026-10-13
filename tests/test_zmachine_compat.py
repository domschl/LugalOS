#!/usr/bin/env python3
"""tests/test_zmachine_compat.py — Z-machine & Auto-Mapper Compatibility Verification

Verifies Z-machine story files against the LugalOS freestanding interpreter
core (zork-host) and verifies auto-mapper room identification.

Usage:
    python3 tests/test_zmachine_compat.py
    python3 tests/test_zmachine_compat.py [path/to/story.z3 ...]
    python3 tests/test_zmachine_compat.py --dir user/zmachine/additional_games
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
ZMACHINE_DIR = REPO_ROOT / "user" / "zmachine"
ZORK_HOST = ZMACHINE_DIR / "zork-host"


def ensure_zork_host() -> bool:
    """Build zork-host if not already present."""
    if not ZORK_HOST.exists():
        print(f"Building zork-host in {ZMACHINE_DIR}...")
        res = subprocess.run(["make", "-C", str(ZMACHINE_DIR)], capture_output=True, text=True)
        if res.returncode != 0:
            print("ERROR: Failed to build zork-host:", file=sys.stderr)
            print(res.stderr, file=sys.stderr)
            return False
    return True


def verify_story(path: Path) -> dict[str, str]:
    """Run zork-host --compat on a story file and parse results."""
    res = subprocess.run(
        [str(ZORK_HOST), "--compat", str(path)],
        capture_output=True,
        text=True,
    )
    info: dict[str, str] = {
        "file": path.name,
        "path": str(path),
        "version": "?",
        "release": "-",
        "serial": "-",
        "size": f"{path.stat().st_size // 1024} KB",
        "room_name": "-",
        "mapper": "UNKNOWN",
        "result": "FAIL",
    }
    for line in res.stdout.splitlines():
        line = line.strip()
        if not line.startswith("[compat] "):
            continue
        parts = line[len("[compat] "):].split(":", 1)
        if len(parts) != 2:
            continue
        k = parts[0].strip()
        v = parts[1].strip()
        if k == "version":
            info["version"] = v
        elif k == "release":
            info["release"] = v
        elif k == "serial":
            info["serial"] = v
        elif k == "size":
            info["size"] = v
        elif k == "room_name":
            info["room_name"] = v.strip('"')
        elif k == "auto-mapper":
            info["mapper"] = v
        elif k == "result":
            info["result"] = v

    if res.returncode != 0 and info["result"] == "FAIL":
        if "UNSUPPORTED" in res.stdout or "unsupported" in res.stderr:
            info["result"] = "UNSUPPORTED"
    return info


def find_default_stories() -> list[Path]:
    """Find story files to verify."""
    candidates: list[Path] = []
    # 1. additional_games
    add_dir = ZMACHINE_DIR / "additional_games"
    if add_dir.exists():
        candidates.extend(sorted(add_dir.glob("*.z[1-8]")))
    # 2. z-data
    zdata_dir = ZMACHINE_DIR / "z-data"
    if zdata_dir.exists():
        candidates.extend(sorted(zdata_dir.glob("*.z[1-8]")))
    # 3. MIT games in user/zmachine/games/
    for sub in (ZMACHINE_DIR / "games").glob("*"):
        if sub.is_dir():
            candidates.extend(sorted(sub.glob("*.z[1-8]")))
    return candidates


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify Z-machine stories against LugalOS interpreter and mapper."
    )
    parser.add_argument("stories", nargs="*", type=Path, help="Specific story files to check")
    parser.add_argument("--dir", type=Path, help="Directory containing stories to check")
    args = parser.parse_args()

    if not ensure_zork_host():
        return 1

    targets: list[Path] = []
    if args.stories:
        targets = args.stories
    elif args.dir:
        targets = sorted(args.dir.glob("*.z[1-8]"))
    else:
        targets = find_default_stories()

    if not targets:
        print("No story files found to verify.")
        print(f"Fetch games first using: {ZMACHINE_DIR}/fetch-games.sh")
        return 0

    print("=" * 80)
    print("           LugalOS Z-Machine & Auto-Mapper Compatibility Report")
    print("=" * 80)
    print(f"{'Story':<18} {'Spec':<5} {'Rel':<4} {'Serial':<8} {'Size':<10} {'First Room':<20} {'Mapper':<11} {'Status'}")
    print("-" * 80)

    all_passed = True
    for p in targets:
        if not p.is_file():
            continue
        info = verify_story(p)
        room = info["room_name"]
        if len(room) > 19:
            room = room[:17] + ".."
        status_color = info["result"]
        print(f"{info['file']:<18} {info['version']:<5} {info['release']:<4} {info['serial']:<8} {info['size']:<10} {room:<20} {info['mapper']:<11} {status_color}")
        if info["result"] not in ("PASS", "UNSUPPORTED"):
            all_passed = False

    print("=" * 80)
    return 0 if all_passed else 2


if __name__ == "__main__":
    sys.exit(main())
