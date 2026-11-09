"""Long file names on a board's FAT volume (76.1), for every hardware suite.

Over 9P (the path `p9sync.py` takes): names with Latin-1 letters, mixed case,
names sharing their first letters (their aliases must differ), a lower-case
8.3 name, and a lisp file beside its editor-style backup; each written, listed
exactly as written, read back, and found again in another case. On the
console: `ls` shows the long names, `mv` renames, and `fsck` finds nothing
worse than before (a card in use for months may hold older damage).
Everything is removed again.

A suite passes two callables: `with_9p(fn)` runs `fn(session)` with a
p9lib.Session attached at "/" and returns its result (on a P4 the session and
the console share one wire, so 9P is only up inside it), and `console(line)`
runs one shell line and returns its output.
"""

import re
from collections.abc import Callable
from typing import Any, TypeVar

T = TypeVar("T")

NAMES: list[str] = [
    "Ein längerer Dateiname mit Umlauten äöü.md",
    "chapter one of a long book.md",
    "chapter two of a long book.md",
    "chapter three of a long book.md",
    "chapter four of a long book.md",
    "chapter five of a long book.md",
    "notes.txt",
    "Mixed Case Name.Txt",
    "lugalos.lisp",
    "lugalos.lis~",
]
DIR_NAME = "Long Names 76.1"
RENAMED = "Chapter 1.md"


def _other_case(name: str) -> str:
    """Upper case for the ASCII and Latin-1 letters the board folds."""
    return "".join(c.upper() if ord(c) < 0x100 and c not in "ßÿ" else c for c in name)


_FSCK = re.compile(r"(\d+) cross-linked, (\d+) broken chains, (\d+) wrong sizes, (\d+) lost clusters")


def _fsck(console: Callable[[str], str], root: str) -> tuple[int, ...] | str:
    """fsck's four problem counts, or why there are none."""
    out = console(f"fsck {root}")
    if "for the cluster map" in out:   # a large card on a board without the memory for its map
        return "no memory for fsck here"
    m = _FSCK.search(out)
    return tuple(int(x) for x in m.groups()) if m else f"no fsck result:\n{out[-400:]}"


def _body(i: int) -> bytes:
    return f"{i}: {NAMES[i]}\n".encode()


def check(with_9p: Callable[[Callable[[Any], T]], T], console: Callable[[str], str],
          root: str = "/sd0") -> tuple[bool, str]:
    d = f"{root}/{DIR_NAME}"

    def cleanup(sess: Any) -> None:
        for n in NAMES + [RENAMED]:
            try:
                sess.remove(f"{d}/{n}")
            except Exception:
                pass
        try:
            sess.remove(d)
        except Exception:
            pass

    def write_and_read(sess: Any) -> str:
        cleanup(sess)
        sess.mkdir(d)
        for i, n in enumerate(NAMES):
            sess.write(f"{d}/{n}", _body(i))
        listed = {e.name for e in sess.listdir(d)}
        missing = [n for n in NAMES if n not in listed]
        if missing:
            return f"not listed as written: {missing}; listed: {sorted(listed)}"
        for i, n in enumerate(NAMES):
            back = sess.read(f"{d}/{n}")
            if back != _body(i):
                return f"{n!r} reads back {back!r}"
        other = f"{root}/{_other_case(DIR_NAME)}/{_other_case(NAMES[0])}"
        if sess.read(other) != _body(0):
            return f"{other!r} (another case) did not open {NAMES[0]!r}"
        return ""

    def after_rename(sess: Any) -> str:
        listed = {e.name for e in sess.listdir(d)}
        if RENAMED not in listed or NAMES[1] in listed:
            return f"`mv` did not rename: {sorted(listed)}"
        if sess.read(f"{d}/{RENAMED}") != _body(1):
            return "the renamed file lost its content"
        return ""

    before = _fsck(console, root)
    try:
        err = with_9p(write_and_read)
        if err:
            return False, err
        out = console(f'ls "{d}"')
        if "chapter three of a long book.md" not in out:
            return False, f"`ls` does not show the long names:\n{out[-400:]}"
        out = console(f'mv "{d}/{NAMES[1]}" "{d}/{RENAMED}"')
        err = with_9p(after_rename)
        if err:
            return False, f"{err}\n{out[-200:]}"
        after = _fsck(console, root)
        done = f"{len(NAMES)} long names written, listed, read, renamed"
        if isinstance(after, str) or isinstance(before, str):
            return (not isinstance(after, str) or "memory" in after), f"{done}; {after if isinstance(after, str) else before}"
        if any(a > b for a, b in zip(after, before)):
            return False, f"fsck found more after the test: {before} -> {after} (cross, broken, size, lost)"
        return True, f"{done}; fsck {'clean' if not any(after) else f'unchanged {after}'}"
    finally:
        try:
            with_9p(cleanup)
        except Exception:
            pass
