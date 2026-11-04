#!/usr/bin/env python3
"""No direct calls to a driver's serve() outside the driver-task framework.

A serve callback is reached through driver_task_call() and friends, so that a
preset can fold the endpoint (drivers/driver_task.c) without any caller
noticing. A direct call would bypass the endpoint's claim and its copies, and
would quietly stop working the day the endpoint is folded or unfolded
(doc/history/adr/008_folded_endpoints.md, guardrail 2 of plan/phase66).

Checks, over every C file outside third_party/:
  * a function named in a `.serve = NAME` initializer appears nowhere else
    in its file but its definition (and prototypes) and that initializer;
  * `.serve(` / `->serve(` is called only in drivers/driver_task.c.

    python3 tools/servecheck.py          # report; exit 1 on a finding
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRAMEWORK = ROOT / "drivers" / "driver_task.c"
SKIP = ("third_party", "build", "tools/host_tools")

INIT = re.compile(r"\.serve\s*=\s*([A-Za-z_]\w*)")
INDIRECT = re.compile(r"(?:\.|->)serve\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def c_files() -> list[Path]:
    out = []
    for p in ROOT.rglob("*.c"):
        rel = p.relative_to(ROOT).as_posix()
        if not any(rel.startswith(s) for s in SKIP):
            out.append(p)
    return sorted(out)


def check() -> list[str]:
    findings = []
    for path in c_files():
        text = strip_comments(path.read_text(errors="replace"))
        rel = path.relative_to(ROOT).as_posix()
        lines = text.splitlines()
        if path != FRAMEWORK:
            for i, l in enumerate(lines, 1):
                if INDIRECT.search(l):
                    findings.append(f"{rel}:{i}: calls a serve callback outside drivers/driver_task.c")
        for name in sorted(set(INIT.findall(text))):
            use = re.compile(r"\b" + re.escape(name) + r"\b")
            # A definition or prototype: the name directly after a return type, then '('.
            decl = re.compile(r"^\s*(?:static\s+)?(?:inline\s+)?(?:const\s+)?\w[\w\s\*]*\b"
                              + re.escape(name) + r"\s*\(")
            for i, l in enumerate(lines, 1):
                if not use.search(l):
                    continue
                if INIT.search(l) and INIT.search(l).group(1) == name:
                    continue
                if decl.match(l):
                    continue
                findings.append(f"{rel}:{i}: '{name}' is a serve callback; reach it through driver_task_call()")
    return findings


def main() -> int:
    findings = check()
    for f in findings:
        print(f)
    n = sum(1 for p in c_files() for _ in INIT.finditer(p.read_text(errors="replace")))
    print(f"servecheck: {n} serve callbacks, {len(findings)} direct call(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
