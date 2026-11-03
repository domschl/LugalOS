#!/usr/bin/env python3
"""Who holds a running board's RAM: the runtime half of the ledger (66.1).

Reads /proc/meminfo, /proc/ps and /proc/pages from a board (or from a file
captured earlier), resolves each page run's caller address against the
board's ELF, and sums the pages by subsystem (tools/ramsubsys.py, the same
grouping as tools/sizereport.py's static figures). Task stacks are listed
with their high-water marks, which is what 66.3 sizes them from.

    tools/ramledger.py --c6 zero   --elf build/esp32c6/lugalos.elf
    tools/ramledger.py --p4 tab5   --elf build/esp32p4-tab5/lugalos.elf
    tools/ramledger.py --rp2350 /dev/serial/by-id/...-if00 --elf build/rp2350-wifi/lugalos.elf
    tools/ramledger.py --capture saved.txt --elf ...       # a capture from before
    ... --wait-tasks mqttsrv,httpd --timeout 120           # the load scenario's services first
    ... --load 192.168.178.25 --load-secs 60               # the active scenario first
    ... --json out.json                                    # for tools/ram_baselines/runtime/

The ELF must be the image the board runs: the tool compares the build id in
/proc/buildid with the ELF's and refuses a mismatch, because every caller
address would otherwise name the wrong function.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ramsubsys  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
PAGE = 4096

# What the board is asked, in order. One capture holds all of it.
COMMANDS = ["cat /proc/buildid", "cat /proc/meminfo", "cat /proc/ps", "cat /proc/pages"]

# Callers that allocate on someone else's behalf: their runs are named by
# what they are, not by the file they are in.
WRAPPERS: list[tuple[str, str]] = [
    ("stack", r"^kernel/sched\.c$"),
    ("sub-page heap (balloc)", r"^kernel/balloc\.c$"),
    ("U-mode heap (uheap)", r"^kernel/uheap\.c$"),
    ("programs (ELF loader)", r"^arch/riscv/common/elf\.c$"),
    ("page tables", r"^arch/riscv/(rv64_mmu|rv32_nommu)/vmm\.c$|^arch/riscv/common/mem_domain\.c$"),
]


# ---- talking to a board -----------------------------------------------------

def capture_esp(tool: str, board: str, cmd_wait: float) -> str:
    """Runs tools/c6run.py or tools/p4run.py in --listen mode (neither resets
    the board) and returns what the console said."""
    args = [sys.executable, str(REPO / "tools" / tool), "--board", board, "--listen",
            "--listen-secs", "1", "--cmd-wait", str(cmd_wait)]
    for c in COMMANDS:
        args += ["--cmd", c]
    r = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if r.returncode != 0 and not r.stdout:
        sys.exit(f"{tool} failed:\n{r.stderr[-800:]}")
    return r.stdout


def capture_rp2350(port: str, cmd_wait: float) -> str:
    """The RP2350's console is USB CDC: opening it resets nothing (only a
    1200-baud open does, into BOOTSEL, so the rate here must not be that)."""
    import serial
    out = bytearray()
    with serial.Serial(port, 115200, timeout=0.2) as s:
        s.reset_input_buffer()
        for c in COMMANDS:
            s.write(c.encode() + b"\r")
            until = time.time() + cmd_wait
            while time.time() < until:
                out.extend(s.read(4096))
    return out.decode("utf-8", "replace")


# ---- the active scenario -----------------------------------------------------

def run_load(host: str, secs: float) -> dict[str, int]:
    """Drives the node's network services from this host for `secs` seconds:
    two threads fetching the admin page over HTTP and HTTPS (TLS where the node
    has a certificate; not verified, the point is the work, not the trust), and
    one opening a 9P session on port 564 and sending Tversion (no grant needed
    for that). The ledger is read afterwards: stack high-water marks and
    'Pages Peak' keep what the load drove them to."""
    import socket
    import ssl
    import struct
    import threading
    counts = {"http": 0, "https": 0, "9p": 0, "errors": 0}
    lock = threading.Lock()
    stop = time.time() + secs
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    def bump(k: str) -> None:
        with lock:
            counts[k] += 1

    def get(tls: bool) -> None:
        port = 443 if tls else 80
        while time.time() < stop:
            try:
                with socket.create_connection((host, port), timeout=5) as raw:
                    sock = ctx.wrap_socket(raw, server_hostname=host) if tls else raw
                    sock.sendall(b"GET / HTTP/1.1\r\nHost: " + host.encode() +
                                 b"\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n")
                    while sock.recv(4096):
                        pass
                bump("https" if tls else "http")
            except OSError:
                bump("errors")
                time.sleep(1)

    def ninep() -> None:
        msg = b"9P2000"
        body = struct.pack("<BHIH", 100, 0xFFFF, 8192, len(msg)) + msg
        tversion = struct.pack("<I", 4 + len(body)) + body
        while time.time() < stop:
            try:
                with socket.create_connection((host, 564), timeout=5) as sock:
                    sock.sendall(tversion)
                    sock.recv(64)
                bump("9p")
            except OSError:
                bump("errors")
                time.sleep(1)
            time.sleep(0.5)

    threads = [threading.Thread(target=get, args=(False,)), threading.Thread(target=get, args=(True,)),
               threading.Thread(target=ninep)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return counts


# ---- parsing ----------------------------------------------------------------

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def parse(text: str) -> dict:
    text = ANSI.sub("", text).replace("\r", "")
    d: dict = {"runs": [], "tasks": [], "fast": {}, "bulk": None, "build": None}
    m = re.search(r"^\d+\.\d+\.\d+ (\d+\.[0-9a-f]+\+?)\s*$", text, re.M)
    if m:
        d["build"] = m.group(1)
    for key, rx in (("total", r"Pages Total: (\d+)"), ("free", r"Pages Free: (\d+)"),
                    ("peak", r"Pages Peak: (\d+)"), ("largest_run", r"Largest Free Run: (\d+)")):
        m = re.search(rx, text)
        if m:
            d["fast"][key] = int(m.group(1))
    m = re.search(r"Bulk Pages: (\d+) total, (\d+) free, peak (\d+), largest run (\d+), SRAM fallbacks (\d+)", text)
    if m:
        d["bulk"] = dict(zip(("total", "free", "peak", "largest_run", "fallbacks"), map(int, m.groups())))
    for m in re.finditer(r"^\s*(\d+)\s+(RUNNING|READY|BLOCKED|DEAD|SLEEPING)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(?:(\d+)/(\d+) B|-)",
                         text, re.M):
        pid, state, name, _exit, isol, hart, used, size = m.groups()
        d["tasks"].append({"pid": int(pid), "state": state, "name": name, "isolation": isol,
                           "hart": hart, "stack_used": int(used) if used else None,
                           "stack_size": int(size) if size else None})
    for m in re.finditer(r"^([fb]) ([0-9a-f]+) (\d+) ([0-9a-f]+)(?: (\d+) (\S+))?\s*$", text, re.M):
        zone, base, pages, caller, pid, task = m.groups()
        d["runs"].append({"zone": "bulk" if zone == "b" else "fast", "base": int(base, 16),
                          "pages": int(pages), "caller": int(caller, 16),
                          "pid": int(pid) if pid else None, "task": task})
    m = re.search(r"# (\d+) runs \((\d+) pages\) not shown", text)
    d["not_shown"] = {"runs": int(m.group(1)), "pages": int(m.group(2))} if m else None
    return d


# ---- checks the hardware suites run ----------------------------------------

def problems(d: dict, max_stack_pct: int = 90, slack_pages: int = 2) -> list[str]:
    """What is wrong with a capture, without the ELF: /proc/pages missing,
    pages in use that no run accounts for (beyond `slack_pages`, since meminfo
    and pages are read one after the other on a live board), or a task stack
    whose high-water mark is above `max_stack_pct` of its size."""
    out = []
    if not d["runs"]:
        return ["no /proc/pages (an image older than 66.1?)"]
    hidden = d["not_shown"]["pages"] if d["not_shown"] else 0
    f = d["fast"]
    if "total" in f and "free" in f:
        used = f["total"] - f["free"]
        seen = sum(r["pages"] for r in d["runs"] if r["zone"] == "fast")
        if abs(used - seen - (hidden if not d["bulk"] else 0)) > slack_pages:
            out.append(f"fast zone: {used} pages in use, /proc/pages accounts for {seen}")
    if d["bulk"] and not hidden:
        used = d["bulk"]["total"] - d["bulk"]["free"]
        seen = sum(r["pages"] for r in d["runs"] if r["zone"] == "bulk")
        if abs(used - seen) > slack_pages:
            out.append(f"bulk zone: {used} pages in use, /proc/pages accounts for {seen}")
    for t in d["tasks"]:
        if t["stack_size"] and t["stack_used"] * 100 > max_stack_pct * t["stack_size"]:
            out.append(f"stack of {t['name']}: {t['stack_used']} of {t['stack_size']} bytes used")
    return out


def check_console(run, max_stack_pct: int = 90) -> tuple[bool, str]:
    """The hardware suites' ledger case: `run(cmd) -> console text` is the
    suite's own console. (passed, detail)."""
    d = parse("\n".join(run(c) for c in COMMANDS))
    bad = problems(d, max_stack_pct)
    if bad:
        return False, "; ".join(bad)
    stacks = [t for t in d["tasks"] if t["stack_size"]]
    deepest = max(stacks, key=lambda t: t["stack_used"] / t["stack_size"], default=None)
    detail = (f"{d['fast'].get('free', '?')} of {d['fast'].get('total', '?')} pages free, "
              f"{len(d['runs'])} runs, {len(stacks)} stacks")
    if deepest:
        detail += f", deepest {deepest['name']} {deepest['stack_used']}/{deepest['stack_size']} B"
    return True, detail


def load():
    """This file as a module, for tests/hw (which import it by path)."""
    return sys.modules[__name__]


# ---- naming the callers -----------------------------------------------------

def find_tool(name: str) -> str:
    for prefix in ("riscv64-elf-", "riscv64-unknown-elf-", "riscv32-unknown-elf-", ""):
        if shutil.which(prefix + name):
            return prefix + name
    sys.exit(f"no {name} found")


def elf_build_id(elf: Path) -> str | None:
    """The build id compiled into the image (the one /proc/buildid prints)."""
    out = subprocess.run([find_tool("strings"), "-n", "8", str(elf)], capture_output=True, text=True).stdout
    m = re.search(r"\(build (\d+\.[0-9a-f]+\+?)\)", out)
    return m.group(1) if m else None


def symbolize(elf: Path, addrs: list[int]) -> dict[int, tuple[str, str]]:
    """{return address: (function, repo-relative file)}. The address one byte
    back is inside the call instruction, which is the line that asked."""
    if not addrs:
        return {}
    args = [find_tool("addr2line"), "-f", "-e", str(elf)] + [f"{a - 1:#x}" for a in addrs]
    lines = subprocess.run(args, capture_output=True, text=True).stdout.splitlines()
    out = {}
    for i, a in enumerate(addrs):
        fn = lines[2 * i] if 2 * i < len(lines) else "??"
        loc = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "??"
        path = loc.rsplit(":", 1)[0]
        if "lugalos/" in path:
            path = path.split("lugalos/", 1)[1]
        out[a] = (fn, path)
    return out


def owner_of(run: dict, names: dict[int, tuple[str, str]]) -> tuple[str, str]:
    """(subsystem, detail) for one run."""
    if run["task"]:
        return "stack", run["task"]
    if not run["caller"]:
        return "(unknown caller)", ""
    fn, path = names.get(run["caller"], ("??", "??"))
    for label, rx in WRAPPERS:
        if re.search(rx, path):
            return label, fn
    return ramsubsys.subsystem(path), f"{fn} ({path.rsplit('/', 1)[-1]})"


def ledger(d: dict, elf: Path) -> dict:
    names = symbolize(elf, sorted({r["caller"] for r in d["runs"] if r["caller"]}))
    by_zone: dict[str, dict[str, dict]] = {"fast": {}, "bulk": {}}
    for r in d["runs"]:
        sub, detail = owner_of(r, names)
        z = by_zone[r["zone"]].setdefault(sub, {"pages": 0, "runs": 0, "detail": {}})
        z["pages"] += r["pages"]
        z["runs"] += 1
        z["detail"][detail] = z["detail"].get(detail, 0) + r["pages"]
    stacks = [t for t in d["tasks"] if t["stack_size"]]
    return {"build": d["build"], "fast": d["fast"], "bulk": d["bulk"], "not_shown": d["not_shown"],
            "owners": {z: {k: {"pages": v["pages"], "runs": v["runs"], "detail": v["detail"]}
                           for k, v in sorted(o.items(), key=lambda kv: -kv[1]["pages"])}
                       for z, o in by_zone.items()},
            "stacks": [{"task": t["name"], "pid": t["pid"], "isolation": t["isolation"],
                        "size": t["stack_size"], "used": t["stack_used"]} for t in stacks]}


def print_ledger(L: dict) -> None:
    f = L["fast"]
    print(f"fast zone (SRAM): {f.get('total', '?')} pages, {f.get('free', '?')} free, "
          f"peak used {f.get('peak', '?')}, largest free run {f.get('largest_run', '?')}")
    for zone in ("fast", "bulk"):
        owners = L["owners"][zone]
        if not owners:
            continue
        if zone == "bulk":
            b = L["bulk"] or {}
            print(f"\nbulk zone (PSRAM): {b.get('total', '?')} pages, {b.get('free', '?')} free, "
                  f"peak used {b.get('peak', '?')}, SRAM fallbacks {b.get('fallbacks', '?')}")
        print(f"  {'pages':>6} {'runs':>5}  owner")
        for sub, o in owners.items():
            print(f"  {o['pages']:6d} {o['runs']:5d}  {sub}")
            if sub != "stack":
                for det, n in sorted(o["detail"].items(), key=lambda kv: -kv[1]):
                    print(f"  {n:6d} {'':5}    {det}")
    if L["not_shown"]:
        print(f"\n  ({L['not_shown']['runs']} runs, {L['not_shown']['pages']} pages, did not fit /proc/pages)")
    print(f"\n  {'stack':>6} {'used':>6} {'%':>4}  task (isolation)")
    for s in sorted(L["stacks"], key=lambda s: -s["size"]):
        pct = 100 * s["used"] // s["size"] if s["size"] else 0
        print(f"  {s['size']:6d} {s['used']:6d} {pct:3d}%  {s['task']} ({s['isolation']})")
    total = sum(s["size"] for s in L["stacks"])
    used = sum(s["used"] for s in L["stacks"])
    print(f"  {total:6d} {used:6d}        all task stacks ({total // PAGE} pages)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--c6", metavar="BOARD", help="a board in ~/.config/lugalos/c6-ports.env")
    src.add_argument("--p4", metavar="BOARD", help="a board in ~/.config/lugalos/p4-ports.env")
    src.add_argument("--rp2350", metavar="PORT", help="the RP2350's console (CDC if00)")
    src.add_argument("--capture", metavar="FILE", help="console text captured earlier")
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--wait-tasks", default="", help="comma-separated tasks that must be running first")
    ap.add_argument("--timeout", type=float, default=120.0, help="how long to wait for them, seconds")
    ap.add_argument("--cmd-wait", type=float, default=2.5)
    ap.add_argument("--load", metavar="HOST", help="drive HTTP, HTTPS and 9P on HOST before the capture")
    ap.add_argument("--load-secs", type=float, default=60.0)
    ap.add_argument("--save", type=Path, help="keep the raw console text here")
    ap.add_argument("--json", type=Path, help="write the ledger as JSON")
    ap.add_argument("--allow-build-mismatch", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="also fail on unaccounted pages or a stack above --max-stack-pct")
    ap.add_argument("--max-stack-pct", type=int, default=90)
    a = ap.parse_args()

    def grab() -> str:
        if a.capture:
            return Path(a.capture).read_text()
        if a.c6:
            return capture_esp("c6run.py", a.c6, a.cmd_wait)
        if a.p4:
            return capture_esp("p4run.py", a.p4, a.cmd_wait)
        return capture_rp2350(a.rp2350, a.cmd_wait)

    want = {t for t in a.wait_tasks.split(",") if t}
    deadline = time.time() + a.timeout
    while True:
        text = grab()
        d = parse(text)
        running = {t["name"] for t in d["tasks"] if t["state"] != "DEAD"}
        missing = want - running
        if not missing or a.capture or time.time() > deadline:
            break
        print(f"[ramledger] waiting for {', '.join(sorted(missing))} ...", file=sys.stderr)
        time.sleep(10)
    load = None
    if a.load and not missing:
        print(f"[ramledger] load on {a.load} for {a.load_secs:.0f} s ...", file=sys.stderr)
        load = run_load(a.load, a.load_secs)
        print(f"[ramledger] load done: {load}", file=sys.stderr)
        text = grab()
        d = parse(text)
    if a.save:
        a.save.write_text(text)
    if missing:
        print(f"[ramledger] WARNING: not running after {a.timeout:.0f} s: {', '.join(sorted(missing))}",
              file=sys.stderr)
    if not d["runs"]:
        sys.exit("[ramledger] no /proc/pages in the capture (an image older than 66.1?)")

    eb = elf_build_id(a.elf)
    if d["build"] and eb and d["build"] != eb and not a.allow_build_mismatch:
        sys.exit(f"[ramledger] the board runs build {d['build']}, {a.elf} is {eb}: "
                 "the caller addresses would name the wrong code")

    L = ledger(d, a.elf)
    L["waited_for"] = sorted(want)
    L["missing"] = sorted(missing)
    L["load"] = load
    print_ledger(L)
    bad = problems(d, a.max_stack_pct) if a.check else []
    for p in bad:
        print(f"[ramledger] FAIL: {p}", file=sys.stderr)
    L["problems"] = bad
    if a.json:
        a.json.write_text(json.dumps(L, indent=2, sort_keys=True) + "\n")
    return 1 if (missing or bad) else 0


if __name__ == "__main__":
    raise SystemExit(main())
