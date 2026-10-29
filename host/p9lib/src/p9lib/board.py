"""Which board is on the other end? The node answers without a key (`/proc/board`, kernel/boardinfo.h):
preset, board, hardware, chip, version, build, name, uid. A tool that is about to write to a board
says which one it found, and a destructive one takes `--expect` and refuses on a mismatch -- with
several boards on the desk, the wrong port is the likeliest mistake."""

from __future__ import annotations

import sys

from .client import NOFID, P9Client, P9Error

_FIELDS = ("preset", "board", "hardware", "chip", "version", "build", "tainted", "name", "uid")


def read_board(client: P9Client) -> dict[str, str]:
    """The node's /proc/board as a dict, or {} for a node that predates it. Uses the one attach that
    needs no key, so it works before a Session is established; the server forgets this connection
    state at the next Tversion, which Session sends."""
    try:
        client.version()
        client.attach(1, aname="/proc/board", afid=NOFID)
        client.walk(1, 2, [])
        client.open(2)
        text = client.read(2, 0, 512).decode(errors="replace")
        client.clunk(2)
        client.clunk(1)
    except P9Error:
        return {}
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def board_line(info: dict[str, str]) -> str:
    """One line a person can compare against the board in front of them."""
    if not info:
        return "an unidentified node (no /proc/board: older firmware)"
    return (f'{info.get("name", "?")} -- {info.get("hardware", "?")}, {info.get("chip", "?")}, '
            f'preset {info.get("preset", "?")} v{info.get("version", "?")} build {info.get("build", "?")}, '
            f'uid {info.get("uid", "?")}')


def matches(info: dict[str, str], expect: str) -> bool:
    """`expect` names the board: its name, its uid (any prefix of four digits or more), its
    preset or its board file. Case does not matter."""
    e = expect.strip().lower()
    if not e or not info:
        return False
    if e in (info.get("name", "").lower(), info.get("preset", "").lower(), info.get("board", "").lower()):
        return True
    uid = info.get("uid", "").lower()
    return len(e) >= 4 and uid.startswith(e) and uid != "none"


def identify(client: P9Client, expect: str | None = None, destructive: bool = False, quiet: bool = False,
             out=None) -> dict[str, str]:
    """Reads and prints the board line (to stderr, so it never mixes with a command's output).
    With `expect` set, a node that does not match -- or cannot say -- is refused: SystemExit.
    Without it, a destructive action only warns when the node is unidentified."""
    out = out or sys.stderr
    info = read_board(client)
    if not quiet:
        print(f"target: {board_line(info)}", file=out)
    if expect:
        if not matches(info, expect):
            raise SystemExit(f"refusing: this is not the board you expected ({expect!r}):\n  {board_line(info)}")
    elif destructive and not info:
        print("warning: this node cannot say which board it is; pass --expect once it is updated", file=out)
    return info


def add_expect_argument(parser, what: str = "write to") -> None:
    parser.add_argument("--expect", metavar="NAME_OR_UID",
                        help=f"refuse to {what} any board but this one: its name, uid (four digits or more), "
                             "preset or board id, as `board` prints them")
