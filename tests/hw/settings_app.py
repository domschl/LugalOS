"""The Settings app and the paper warmth (77.1) on a board with a panel.

Warmth: `display.warmth` 10 with the warm ink gives the endpoint pair
(ink 0x3943, paper 0xe674, the deep sepia), the warm ink off keeps black ink on the warm
paper, and warmth 0 is exactly black on white again. Each pair is read back
from the panel driver's own status (`lcd`: the P4's and the RP2350's
terminal, the VisionFive 2's display controller), not from the settings
store. On the P4 a coloured pair runs the frame in RGB565 (the 7B goes back
to GRAY8 at warmth 0), and `lcd check` must still find the glass equal to a
full redraw -- the views' gray surfaces are tinted on the way to the glass.

The app: `settings open` opens its window focused, the keyboard reaches it
(Tab shows the focus, Enter on a section opens it), and Cmd+W closes it. The
menu bar's gear opens it again where it was left, its SD icon at Storage,
and Cmd+, as the gear.

A suite passes `console(line)` (one shell line's output), as for
titlebar_menu.py. The settings it touches are put back.
"""

import re
import time
from collections.abc import Callable

PAIR = re.compile(r"fg 0x([0-9a-f]{4}) bg 0x([0-9a-f]{4})")
FRAME = re.compile(r"frame: (RGB565|GRAY8)")


def check(console: Callable[[str], str], glass_check: bool = False) -> tuple[bool, str]:
    steps: list[tuple[str, bool, str]] = []

    def step(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)[-300:]))

    def value(key: str) -> str:
        m = re.search(re.escape(key) + r"=(\S+)", console(f"settings get {key}"))
        return m.group(1) if m else ""

    def pair() -> tuple[str, str, str]:
        out = console("lcd")
        p = PAIR.search(out)
        f = FRAME.search(out)
        return (p.group(1) if p else "?"), (p.group(2) if p else "?"), (f.group(1) if f else "")

    warmth, warm_ink = value("display.warmth"), value("display.warm_ink")
    if not warmth:
        return True, "SKIPPED: no display.warmth on this image"
    if PAIR.search(console("lcd")) is None:
        return True, "SKIPPED: no panel status with a colour pair"
    try:
        console("settings set display.warmth 0")
        time.sleep(0.5)
        _, _, frame0 = pair()

        console("settings set display.warm_ink on")
        console("settings set display.warmth 10")
        time.sleep(0.5)
        fg, bg, frame = pair()
        step("warmth 10, warm ink: ink 0x3943, deep sepia 0xe674", (fg, bg) == ("3943", "e674"), f"fg {fg} bg {bg}")
        if frame:
            step("a coloured pair runs the frame in RGB565", frame == "RGB565", frame)
        if glass_check:
            out = console("lcd check")
            step("the glass equals a full redraw in the warm colours", "matches a full redraw" in out, out)

        console("settings set display.warm_ink off")
        time.sleep(0.5)
        fg, bg, _ = pair()
        step("warm ink off: black ink on the warm paper", (fg, bg) == ("0000", "e674"), f"fg {fg} bg {bg}")

        console("settings set display.warmth 0")
        time.sleep(0.5)
        fg, bg, frame = pair()
        step("warmth 0: exactly black on white", (fg, bg) == ("0000", "ffff"), f"fg {fg} bg {bg}")
        if frame and frame0:
            step("warmth 0: the frame back in its own format", frame == frame0, f"{frame} (was {frame0})")

        out = console("settings open")
        step("settings open", "settings: open" in out, out)
        console("key tab down enter")                    # the focus shown, down to Sound, opened
        out = console("settings view")
        step("the keyboard opens a section", "section Sound" in out, out)
        console("key cmd-w")
        time.sleep(0.5)
        out = console("settings view")
        step("Cmd+W closes it", "settings view: closed" in out, out)

        # The menu bar's right end: the gear (always there) opens the app at
        # the section it was left at, the SD icon at Storage, Cmd+, as the
        # gear. Places from the screen's own hit test (`ribbon list`).
        tail = console("ribbon list").split("status:")[-1].split("\n")[0]
        runs = {m.group(1): (int(m.group(2)), int(m.group(3)))
                for m in re.finditer(r"(settings|sd|wifi|p9|battery) (\d+)\.\.(\d+)", tail)}
        step("the menu bar has a status area", "settings" in runs, tail)
        if "settings" in runs:
            console(f"ms click {runs['settings'][1] - 6} 8")
            time.sleep(0.5)
            out = console("settings view")
            step("the gear opens Settings where it was left", "settings view: open" in out and "section Sound" in out, out)
            console("key cmd-w")
            time.sleep(0.5)
        if "sd" in runs:
            console(f"ms click {(runs['sd'][0] + runs['sd'][1]) // 2} 8")
            time.sleep(0.5)
            out = console("settings view")
            step("the SD icon opens Storage", "section Storage" in out, out)
            console("key cmd-w")
            time.sleep(0.5)
        console("key cmd-,")
        time.sleep(0.5)
        out = console("settings view")
        step("Cmd+, opens Settings", "settings view: open" in out, out)
        console("key cmd-w")
    finally:
        if warm_ink:
            console(f"settings set display.warm_ink {warm_ink}")
        console(f"settings set display.warmth {warmth}")

    bad = [f"{what}: {got}" for what, ok, got in steps if not ok]
    if bad:
        return False, "; ".join(bad)
    return True, f"{len(steps)} steps: warm pair, warm ink off, black on white, the app by keys"
