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


# --- 77.2: the launcher's two faces ---------------------------------------------

def _only_terminal(session: Session) -> None:
    """Every window but the first terminal closed: the launcher by its
    command (it has no close box), the rest by Cmd+W on each."""
    session.send_and_expect("launcher close", r"lsh> ", timeout=10.0)
    for _ in range(6):
        ok, log = session.send_and_expect("ribbon", r"Ribbon Status: (\d+) windows", timeout=10.0)
        m = re.search(r"Ribbon Status: (\d+) windows", log)
        if not ok or not m or int(m.group(1)) <= 1:
            return
        session.send_and_expect("ribbon focus 1", r"lsh> ", timeout=10.0)
        session.send_and_expect("key cmd-w", r"key: ", timeout=10.0)
        session.send_and_expect("launcher status", r"launcher: ", timeout=10.0)     # the views task caught up


# The launcher's content on QEMU's 800 x 480 RAM screen, opened beside the
# terminal and scrolled to: window-relative (x, y) is screen (x + 5, y + 39).
LAUNCHER_OFF = (5, 39)


def _launcher_hits(session: Session) -> tuple[dict[tuple[int, int], tuple[int, int]], str]:
    """`launcher status`'s hit-boxes as (id, index) -> screen centre; and the status."""
    ok, log = session.send_and_expect("launcher status", r"launcher: [^\n]*\n(?:.*\n)*?.*lsh> ", timeout=10.0)
    hits = {}
    for m in re.finditer(r"hit id (\d+) index (-?\d+): x (\d+)\.\.(\d+) y (\d+)\.\.(\d+)", log):
        x = (int(m.group(3)) + int(m.group(4))) // 2 + LAUNCHER_OFF[0]
        y = (int(m.group(5)) + int(m.group(6))) // 2 + LAUNCHER_OFF[1]
        hits[(int(m.group(1)), int(m.group(2)))] = (x, y)
    return hits, log


def launcher_files_by_keys(session: Session) -> Result:
    """77.2's acceptance, keys only: the launcher, Ctrl+Tab to Files, Enter
    into /sd0, type-ahead to help and shell.md, Enter opens it in the
    reader; Cmd+Space back to the launcher, Shift+Enter's sheet adds it to
    the shortcuts; Ctrl+Tab to Shortcuts, its tile's sheet (m) removes it,
    confirmed. The shortcuts put back to the defaults after."""
    name = "Launcher By Keys: Files To A Document, Shortcut Added And Removed (77.2)"
    _only_terminal(session)
    session.send_and_expect("shortcuts reset", r"shortcuts: ok", timeout=10.0)
    ok, log = _steps(session, [
        ("launcher", r"launcher: (opened|focused)"),
        ("key ctrl-tab tab", r"key: 2 sent"), ("launcher status", r"face Files"),
        ("key down enter", r"key: 2 sent"), ("launcher status", r'files: "/sd0"'),
        ("key h enter", r"key: 2 sent"), ("launcher status", r'files: "/sd0/help"'),
        ("key s s", r"key: 2 sent"), ("launcher status", r'focus: file \d+ "shell\.md"'),
        ("key enter", r"key: 1 sent"), ("launcher status", r"launcher: open"),
        ("ribbon", r"Reader [^\n]*\*"),
        ("key cmd-space", r"key: 1 sent"), ("launcher status", r'focus: file \d+ "shell\.md"'),
        ("key shift-enter", r"key: 1 sent"), ("launcher status", r"sheet: shell\.md: [^\n]*\[2\] Add to Shortcuts"),
        ("key down down enter", r"key: 3 sent"), ("shortcuts", r"shortcuts: ok, 7[^$]*/sd0/help/shell\.md"),
        ("key ctrl-tab", r"key: 1 sent"), ("launcher status", r'focus: shortcut 0'),
        ("key right right right right right right", r"key: 6 sent"), ("launcher status", r'focus: shortcut 6'),
        ("key m", r"key: 1 sent"), ("launcher status", r"sheet: [^\n]*\[2\] Remove"),
        ("key down down enter", r"key: 3 sent"), ("launcher status", r"alert: Remove the shortcut\?"),
        ("key enter", r"key: 1 sent"), ("shortcuts", r"shortcuts: ok, 6\r?\n"),
    ])
    session.send_and_expect("shortcuts reset", r"shortcuts: ok", timeout=10.0)
    session.send_and_expect("launcher close", r"launcher: closed", timeout=10.0)
    return name, ok, log


def launcher_500_entries(session: Session) -> Result:
    """77.2: a directory of 500 files lists whole, in natural order (file2
    before file10); PgDn moves the focus a page and End to the last; the
    last one's sheet. The files are removed again after."""
    name = "Launcher Files: 500 Entries, Natural Order, Paging By Keys (77.2)"
    _only_terminal(session)
    session.send_and_expect("mkdir /sd0/many", r"lsh> ", timeout=10.0)
    session.send_and_expect("(define i 0)", r"=> ", timeout=10.0)
    session.send_and_expect('(while (< i 500) (write-file (string-append "/sd0/many/file" (number->string i) ".txt") "x") '
                            "(set! i (+ i 1)))", r"lsh> ", timeout=120.0)
    ok, log = _steps(session, [
        ("launcher", r"launcher: (opened|focused)"),
        ("key ctrl-tab", r"key: 1 sent"), ("launcher status", r"face Files"),
        ("launcher status", r"launcher: open"),
    ])
    if ok:
        # To /sd0/many from wherever the Files face was left: the roots first.
        ok, log = _steps(session, [
            ("key tab bs bs bs", r"key: 4 sent"), ("launcher status", r'files: "/"'),
            ("key down enter m enter", r"key: 4 sent"), ("launcher status", r'files: "/sd0/many", 500 entries'),
            ("key down down", r"key: 2 sent"), ("launcher status", r'focus: file 2 "file2\.txt"'),
            ("key pgdn", r"key: 1 sent"), ("launcher status", r'focus: file (1[0-9]|[3-9]) "file'),
            ("key end", r"key: 1 sent"), ("launcher status", r'focus: file 499 "file499\.txt"'),
            ("key shift-enter", r"key: 1 sent"), ("launcher status", r"sheet: file499\.txt: "),
            ("key esc", r"key: 1 sent"),
        ])
    session.send_and_expect("launcher close", r"launcher: closed", timeout=10.0)
    # The card shared with the checks after this one: the files gone again.
    session.send_and_expect("(set! i 0)", r"=> ", timeout=10.0)
    session.send_and_expect('(while (< i 500) (rm (string-append "/sd0/many/file" (number->string i) ".txt")) '
                            "(set! i (+ i 1)))", r"lsh> ", timeout=120.0)
    session.send_and_expect('(rmdir "/sd0/many")', r"=> ", timeout=10.0)
    return name, ok, log


def launcher_rename_shortcut(session: Session) -> Result:
    """77.2: a shortcut renamed through its sheet's dialog (the text field
    selected whole, so typing replaces it), and kept in the file."""
    name = "Launcher Shortcuts: Rename Through The Dialog, Kept In The File (77.2)"
    _only_terminal(session)
    session.send_and_expect("shortcuts reset", r"shortcuts: ok", timeout=10.0)
    ok, log = _steps(session, [
        ("launcher", r"launcher: (opened|focused)"),
        ("launcher status", r"face (Shortcuts|Files)"),
    ])
    if ok and "face Files" in log:
        session.send_and_expect("key ctrl-tab", r"key: 1 sent", timeout=10.0)
    if ok:
        ok, log = _steps(session, [
            ("key tab m", r"key: 2 sent"), ("launcher status", r"sheet: Settings: [^\n]*\[1\] Rename"),
            ("key down enter", r"key: 2 sent"), ("launcher status", r'renaming shortcut 0: "Settings"'),
            ('key "Prefs" enter', r"key: 6 sent"), ("shortcuts", r"\[0\] app  Prefs"),
            ("cat /sd0/system/shortcuts", r"app\tsettings\tPrefs"),
        ])
    session.send_and_expect("shortcuts reset", r"shortcuts: ok", timeout=10.0)
    session.send_and_expect("launcher close", r"launcher: closed", timeout=10.0)
    return name, ok, log


def launcher_by_mouse(session: Session) -> Result:
    """71.7 / 77.2: the mouse (`ms`) on the launcher and the reader. Hover
    outlines a shortcut tile, a right click opens its sheet and a click
    beside it closes it, a click opens the Help document, a click on the
    reader's close box closes it; the wheel scrolls a reader under the
    pointer while the terminal has the focus (two notches away, natural
    scrolling: down six lines of 18 px), the sideways wheel moves the focus
    along the ribbon (tilted left, natural: the window on the right). Tile
    places from `launcher status`."""
    name = "Mouse: Hover, Click, Context Menu, Wheel Under The Pointer (71.7 ms)"
    _only_terminal(session)
    session.send_and_expect("shortcuts reset", r"shortcuts: ok", timeout=10.0)
    ok, log = session.send_and_expect("launcher", r"launcher: (opened|focused)", timeout=10.0)
    if not ok:
        return name, False, log
    hits, log = _launcher_hits(session)
    if "face Files" in log:
        session.send_and_expect("key ctrl-tab", r"key: 1 sent", timeout=10.0)
        hits, log = _launcher_hits(session)
    help_tile = next((k for k in hits if k[0] == 10 and k[1] == 4), None)       # Settings, Terminal, Chess, Moonbase, Help
    if not help_tile:
        return name, False, f"no fifth shortcut tile in:\n{log}"
    hx, hy = hits[help_tile]
    steps = [(f"ms move {hx} {hy}", r"ms: moved"), ("launcher status", r"hover: id 10 index 4"),
             (f"ms click {hx} {hy} right", r"ms: right click"), ("launcher status", r"sheet: Help"),
             (f"ms click {hx} {hy}", r"ms: left click"), ("launcher status", r"launcher: open"),
             (f"ms click {hx} {hy}", r"ms: left click"), ("launcher status", r"launcher: open"),
             ("ribbon", r"\[2\] Reader[^\n]*\*"),
             ("ms click 13 30", r"ms: left click"), ("ribbon", r"\[1\] Launcher[^\n]*\*"),
             ("launcher close", r"launcher: closed"),
             ("reader /sd0/help/shell.md", r"reader: opened"), ("ribbon focus 0", r"lsh> "),
             ("ms move 600 200", r"ms: moved"), ("ms wheel 2", r"ms: wheel"),
             ("reader list", r"shell\.md[^\n]*scrolled 108 px"),
             ("ms wheel 0 -1", r"ms: wheel"), ("ribbon", r"\[1\] Reader[^\n]*\*"),
             ("ms click 417 30", r"ms: left click"), ("ribbon", r"Ribbon Status: 1 windows")]
    for i, (cmd, pat) in enumerate(steps):
        ok, log = session.send_and_expect(cmd, pat, timeout=10.0)
        if ok and i == 5 and "sheet:" in log:
            ok = False                                   # the click beside it left the sheet up
        if not ok:
            return name, False, f"{cmd!r} did not give {pat!r}:\n{log}"
    return name, True, ""


GROUPS["launcher"] = ("the launcher's faces, shortcuts and files (drivers/launcher_view.c, drivers/shortcuts.c)",
                      [launcher_files_by_keys, launcher_rename_shortcut, launcher_500_entries, launcher_by_mouse])
