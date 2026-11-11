"""QEMU test groups: checks written once, run by both test tiers.

`tests/quick.py` (tiers 0-1, doc/dev/testing.md) boots one QEMU target and
runs only the groups named for the area that changed; `tests/runner.py`
(tier 3) calls the same functions inside its full pass. A check that lives
here therefore cannot drift between the quick and the full run.

Each check takes a started `QemuSession` (tests/runner.py) at the shell
prompt and returns `(name, ok, log)` results, as the runner collects them.
Groups name lists of checks; a check may sit in more than one group.

Not everything the runner checks is here yet: an area's inline checks move
into a function when the area is next worked on.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from typing import Any

Result = tuple[str, bool, str]
Session = Any               # tests/runner.QemuSession (not imported: runner imports this module)


# --- Self-tests: one shell command, one verdict line ----------------------------

# command -> (result name, the line it ends with, the marker of a pass, timeout s)
SELFTESTS: dict[str, tuple[str, str, str, float]] = {
    "vtselftest": ("Screen Terminal Emulator Against A RAM Grid (36.6 vtselftest)",
                   r"VTTERM_SELFTEST_(OK|FAIL)[^\n]*\n", "VTTERM_SELFTEST_OK", 10.0),
    "usbselftest": ("USB CRC5/CRC16 Against Known Packets (36.7 usbselftest)",
                    r"USB_SELFTEST_(OK|FAIL)[^\n]*\n", "USB_SELFTEST_OK", 10.0),
    "usbkbdselftest": ("USB Descriptors And Keyboard Reports (36.8 usbkbdselftest)",
                       r"USBKBD_SELFTEST_(OK|FAIL)[^\n]*\n", "USBKBD_SELFTEST_OK", 10.0),
    "uguiselftest": ("Touch Toolkit Hit-Boxes And Drawing (71.0 uguiselftest)",
                     r"uguiselftest: (PASS|FAIL)[^\n]*\n", "uguiselftest: PASS", 10.0),
    "apps check": ("Application Registry Sorted And Consistent (71.5 apps check)",
                   r"apps check: (PASS|FAIL)[^\n]*\n", "apps check: PASS", 10.0),
    "icons check": ("Built-in Icons Sorted And Complete (71.5 icons check)",
                    r"icons check: (PASS|FAIL)[^\n]*\n", "icons check: PASS", 10.0),
    "settings check": ("Settings Store Sorted, Defaults In Range (71.5 settings check)",
                       r"settings check: (PASS|FAIL)[^\n]*\n", "settings check: PASS", 10.0),
    "fmtselftest": ("printk Formats Integers And Strings Correctly (40 review fmtselftest)",
                    r"FMTSELFTEST_(OK|FAIL)", "FMTSELFTEST_OK", 5.0),
    "keyselftest": ("Key Sequences: UTF-8, Alt, xterm Modifiers, CSI-u (37.5a keyselftest)",
                    r"KEYSEQ_SELFTEST_(OK|FAIL)[^\n]*\n", "KEYSEQ_SELFTEST_OK", 10.0),
    "editselftest": ("Editor: Wrap, Rows, UTF-8, Undo, Search, Safe Save (37.5b editselftest)",
                     r"EDITOR_SELFTEST_(OK|FAIL)[^\n]*\n", "EDITOR_SELFTEST_OK", 10.0),
    "vtermselftest": ("Virtual Terminal Subsystem (44.1 vtermselftest)",
                      r"vterm selftest: (PASSED|FAILED)[^\n]*\n", "vterm selftest: PASSED", 10.0),
    "ribbonselftest": ("Ribbon Window Geometry Manager (44.2 ribbonselftest)",
                       r"ribbon geometry selftest: (PASSED|FAILED)[^\n]*\n", "ribbon geometry selftest: PASSED", 10.0),
}


def selftest(session: Session, command: str) -> Result:
    """One self-test from SELFTESTS: its verdict line, and the pass marker in it."""
    name, pattern, good, timeout = SELFTESTS[command]
    ok, log = session.send_and_expect(command, pattern, timeout=timeout)
    ok = ok and good in log
    return name, ok, log if not ok else ""


def _steps(session: Session, steps: list[tuple[str, str]]) -> tuple[bool, str]:
    """Commands in order, each waiting for its pattern; the first miss ends it."""
    for cmd, pat in steps:
        ok, log = session.send_and_expect(cmd, pat, timeout=10.0)
        if not ok:
            return False, f"{cmd!r} did not give {pat!r}:\n{log}"
    return True, ""


# --- 77.1: the Settings app, text sizes, the menu bar ----------------------------

def settings_app_by_keys(session: Session) -> Result:
    """77.1: the Settings app, by keys only. Its window opens focused; Tab
    shows the focus on the first section and walks to the warmth slider
    (seven sections, the brightness slider, then it), Right steps it twice --
    applied, and told back to the view itself as WIN_EV_SETTINGS (two changes
    between two looks are one event). Narrowed (Cmd+- three times), it shows
    the list alone; Enter opens a section with a Back button, Esc goes back;
    Cmd+W closes. Leaves the app's section at Sound."""
    ok, log = _steps(session, [
        ("settings open", r"settings: open"),
        ("ribbon", r"View     \(settings[^\n]*\*"),
        ("key tab tab tab tab tab tab tab tab tab", r"key: 9 sent"),
        ("key right right", r"key: 2 sent"),
        ("settings get display.warmth", r"display\.warmth=2"),
        ("settings view", r"told [1-9]\d* setting changes?, the last display\.warmth"),
        ("key cmd--", r"key: 1 sent"), ("key cmd--", r"key: 1 sent"), ("key cmd--", r"key: 1 sent"),
        ("settings view", r"narrow, section Display \(list shown\)"),
        ("key down enter", r"key: 2 sent"),
        ("settings view", r"narrow, section Sound\r?\n"),
        ("key esc", r"key: 1 sent"),
        ("settings view", r"narrow, section Sound \(list shown\)"),
        ("settings set display.warmth 0", r"display\.warmth=0"),
        ("key cmd-w", r"key: 1 sent"),
        ("settings view", r"settings view: closed"),
    ])
    return "The Settings App By Keys: Warmth, Its Event, Narrow List And Back (77.1)", ok, log


def menubar_opens_settings(session: Session) -> Result:
    """77.1: the menu bar's right end opens Settings -- the gear (always
    drawn) at the section it was left at, the SD icon at Storage (where the
    screen shows one) -- and so does Cmd+,. The places come from `ribbon
    list` (the screen's own hit test, `status: ...`), clicked with the mouse.
    The gear's section is whatever the app was left at: Sound after
    settings_app_by_keys(), Display on a fresh boot."""
    name = "The Menu Bar's Gear, SD Icon And Cmd+, Open Settings (77.1)"
    ok, log = session.send_and_expect("ribbon list", r"status:[^\n]*\n", timeout=15.0)
    runs = {m.group(1): (int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r"(settings|sd|wifi|p9|battery) (\d+)\.\.(\d+)", log.split("status:")[-1])}
    if "settings" not in runs:
        return name, False, f"no status area in ribbon list:\n{log}"
    gx = runs["settings"][1] - 6                     # the gear, at the right end
    steps = [(f"ms click {gx} 8", r"ms: left click"), ("settings view", r"settings view: open[^\n]*section (Sound|Display)"),
             ("key cmd-w", r"key: 1 sent"), ("settings view", r"settings view: closed"),
             ("key cmd-,", r"key: 1 sent"), ("settings view", r"settings view: open"),
             ("key cmd-w", r"key: 1 sent")]
    if "sd" in runs:
        sx = (runs["sd"][0] + runs["sd"][1]) // 2
        steps += [(f"ms click {sx} 8", r"ms: left click"), ("settings view", r"settings view: open[^\n]*section Storage"),
                  ("key cmd-w", r"key: 1 sent")]
    ok, log = _steps(session, steps)
    return name, ok, log


def text_size_relayouts_reader(session: Session) -> Result:
    """77.1: the text size relayouts an open reader (WIN_EV_SETTINGS): the
    same document is shorter in Small and taller in Large than in Medium,
    and exactly as tall again back in Medium."""
    name = "Text Size Relayouts The Reader: Small Shorter, Large Taller, Medium Again (77.1)"
    ok, log = session.send_and_expect("reader shell.md", r"reader: (opened|focused)", timeout=10.0)
    if not ok:
        return name, False, f"reader shell.md:\n{log}"
    heights: dict[str, int] = {}
    pat = r"shell\.md[^\n]*scrolled -?\d+ px of (\d+)"
    result: tuple[bool, str] = (True, "")
    for size in ("medium", "small", "large", "medium"):
        session.send_and_expect(f"settings set ui.text_size {size}", r"ui\.text_size=", timeout=10.0)
        ok, log = session.send_and_expect("reader list", pat, timeout=10.0)
        m = re.search(pat, log)
        if not ok or not m:
            result = (False, f"reader list after {size}:\n{log}")
            break
        h = int(m.group(1))
        if size == "medium" and "medium" in heights and h != heights["medium"]:
            result = (False, f"back in medium: {h} px, first {heights['medium']} px")
            break
        heights.setdefault(size, h)
    if result[0] and not (heights["small"] < heights["medium"] < heights["large"]):
        result = (False, f"heights small/medium/large: {heights}")
    session.send_and_expect("key cmd-w", r"key: 1 sent", timeout=10.0)
    return name, result[0], result[1]


# --- Groups --------------------------------------------------------------------

def _selftests(*commands: str) -> list[Callable[[Session], Result]]:
    return [lambda s, c=c: selftest(s, c) for c in commands]


# group -> (what it covers, its checks in order)
GROUPS: dict[str, tuple[str, list[Callable[[Session], Result]]]] = {
    "screen": ("the screen, the ribbon, vterms, ugui (drivers/screen.c, vtterm.c, ribbon.c, ugui.c, kernel/vterm.c)",
               _selftests("vtselftest", "vtermselftest", "ribbonselftest", "uguiselftest")),
    "settings": ("the settings store, the Settings app, paper warmth, the menu bar's gear "
                 "(kernel/settings.c, drivers/settings_view.c, drivers/paper.c)",
                 _selftests("settings check", "apps check", "icons check")
                 + [settings_app_by_keys, menubar_opens_settings]),
    "reader": ("the reader and its text sizes (drivers/reader_view.c, ugui faces, fs/markdown.c)",
               [text_size_relayouts_reader]),
    "input": ("key decoding, USB descriptors and keyboard reports (kernel/keyseq.c, drivers/usb*.c)",
              _selftests("keyselftest", "usbselftest", "usbkbdselftest")),
    "editor": ("the editor and printk formatting (kernel/editor*.c, kernel/printk.c)",
               _selftests("editselftest", "fmtselftest")),
}
