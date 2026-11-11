#!/usr/bin/env python3
"""precommit.py -- the checks every commit passes, whatever its tier
(doc/dev/testing.md): cheap, and they catch most cross-platform breakage.

  1. every preset configured in build/ builds, with no warning (the P4's
     known objcopy "empty loadable segment" note aside);
  2. no preset's static RAM grew past its baseline (tools/sizereport.py);
  3. the on-device help matches doc/ (tools/gendocs.py --check), and the
     generated icon and font tables match their sources;
  4. every static reached from more than one task has its ownership verdict
     (tools/racescan.py --check).

    python3 tools/precommit.py            # all of it
    python3 tools/precommit.py --no-race  # without the race scan (about a minute)

Exit status 0 when all pass. The tests that go with a change are separate:
tests/quick.py and the board suites' --only for tiers 0-2, tests/runner.py
for tier 3.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NM = "/usr/bin/riscv64-elf-nm"
KNOWN = ("empty loadable segment",)


def run(cmd: list[str], timeout: int = 900) -> tuple[int, str]:
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def presets() -> list[str]:
    """The build directories with a ninja file, but not the analysis ones."""
    return sorted(p.parent.name for p in (ROOT / "build").glob("*/build.ninja")
                  if not p.parent.name.startswith(("racescan", "host", "spike")))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--no-race", action="store_true", help="skip tools/racescan.py --check")
    args = ap.parse_args()
    bad: list[str] = []
    t0 = time.time()

    for p in presets():
        rc, out = run(["ninja", "-C", f"build/{p}"])
        warn = [l for l in out.splitlines()
                if ("warning:" in l or "error:" in l or "FAILED" in l) and not any(k in l for k in KNOWN)]
        if rc != 0 or warn:
            bad.append(f"build {p}: " + ("\n  ".join(warn[:5]) or out[-800:]))
    print(f"precommit: {len(presets())} presets built ({time.time() - t0:.0f} s)")

    rc, out = run([sys.executable, "tools/sizereport.py", "--all", "--check", "--nm", NM])
    if rc != 0:
        bad.append("static RAM: " + "\n  ".join(l for l in out.splitlines() if "GREW" in l or "FAIL" in l))

    for cmd in (["tools/gendocs.py", "--check"], ["tools/gen_icons.py", "--check"],
                ["tools/gen_serif_faces.py", "--check"]):
        rc, out = run([sys.executable, *cmd])
        if rc != 0:
            bad.append(f"{cmd[0]} --check: {out.strip()[-400:]}")

    if not args.no_race:
        rc, out = run([sys.executable, "tools/racescan.py", "--check"])
        if rc != 0:
            bad.append("racescan: " + out.strip()[-600:])

    for b in bad:
        print(f"FAIL {b}")
    print(f"precommit: {'FAIL' if bad else 'PASS'} ({time.time() - t0:.0f} s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
