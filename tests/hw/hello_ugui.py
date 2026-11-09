"""The `ui-` sample (76.2, tools/sd_root/demos/hello_ugui.lisp) on a board.

The sample is copied to /sd0/demos over 9P, opened, and driven through the
board's own input paths at the places its window reports (`ui`): the mouse
(`ms`), a finger where the board has touch (`ts tap`), and keys for the
focused view (`key`). Between inputs the program handles what came
((hu-pump)), and its state ((hu-state)) must be what the inputs mean. A
`q` ends it, and its window must be gone.

A suite passes `with_9p(fn)` (fn(session) with a p9lib.Session attached at
"/") and `console(line)` (one shell line's output), as for longnames.py.
"""

import re
from collections.abc import Callable
from pathlib import Path
from typing import Any, TypeVar

T = TypeVar("T")

SAMPLE = Path(__file__).resolve().parents[2] / "tools" / "sd_root" / "demos" / "hello_ugui.lisp"
HIT = re.compile(r"hit (\d+\.\d+) at (-?\d+) (-?\d+) size (\d+) (\d+)")


def _hits(report: str) -> dict[str, tuple[int, int, int, int]]:
    return {m.group(1): (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
            for m in HIT.finditer(report)}


def _at(h: tuple[int, int, int, int], fx: float = 0.5) -> str:
    return f"{h[0] + int(h[2] * fx)} {h[1] + h[3] // 2}"


def check(with_9p: Callable[[Callable[[Any], T]], T], console: Callable[[str], str]) -> tuple[bool, str]:
    def copy(sess: Any) -> None:
        try:
            sess.mkdir("/sd0/demos")
        except Exception:
            pass
        sess.write("/sd0/demos/hello_ugui.lisp", SAMPLE.read_bytes())

    with_9p(copy)
    out = console('(load "/sd0/demos/hello_ugui.lisp")')
    if "=> #t" not in out:
        return False, f"the sample did not load:\n{out[-300:]}"
    out = console("(hu-start)")
    if "=> #t" not in out:
        return False, f"no window:\n{out[-300:]}"
    report = console("ui")
    hits = _hits(report)
    for k in ("1.0", "4.0", "5.0", "7.0", "8.2"):
        if k not in hits:
            return False, f"widget {k} missing:\n{report[-600:]}"
    ms_ok = "ms: pointer at" in console("ms")
    touch = "Unknown command" not in console("ts") and "Unbound" not in console("ts")

    steps: list[str] = []
    if ms_ok:
        steps += [f"ms click {_at(hits['1.0'])}", f"ms click {_at(hits['4.0'], 0.75)}",
                  f"ms click {_at(hits['5.0'])}", f"ms click {_at(hits['8.2'])}", f"ms click {_at(hits['7.0'])}"]
    if touch:
        steps += [f"ts tap {_at(hits['1.0'])}"]
    steps += ['key "Ada" enter']
    for s in steps:
        console(s)
    console("(hu-pump)")
    state = console("(hu-state)")
    want = [r"greeting=Hello, Ada!"]
    if ms_ok:
        want += [r"level=7\d", r"size=2", r"picked=Cherry"]
    count = (1 if ms_ok else 0) + (1 if touch else 0)
    want += [rf"count={count}\b"]
    missing = [w for w in want if not re.search(w, state)]
    if missing:
        return False, f"state {missing} not reached:\n{state[-300:]}"

    console("key tab tab tab enter")
    console("(hu-pump)")
    state2 = console("(hu-state)")
    if "picked=Apple" not in state2:
        return False, f"keys alone did not pick the first row:\n{state2[-300:]}"
    console("key q")
    out = console("(hu-pump)")
    if "=> #f" not in out or "closed" not in console("ui"):
        return False, f"`q` did not end it:\n{out[-300:]}"
    how = ", ".join(x for x, on in (("mouse", ms_ok), ("touch", touch), ("keys", True)) if on)
    m = re.search(r'count=[^"]*', state)
    return True, f"opened, driven by {how}; {m.group(0) if m else state[-120:]}"
