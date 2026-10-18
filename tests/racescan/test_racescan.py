#!/usr/bin/env python3
"""tools/racescan.py against tests/racescan/fixture.c, whose answer is known."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main() -> int:
    build = REPO / "build" / "racescan-fixture"
    build.mkdir(parents=True, exist_ok=True)
    src = REPO / "tests" / "racescan" / "fixture.c"
    (build / "compile_commands.json").write_text(json.dumps([{
        "directory": str(build), "file": str(src),
        "command": f"cc -std=c11 -c {src}",
    }]))
    out = REPO / "build" / "racescan-fixture" / "out"
    subprocess.run([sys.executable, str(REPO / "tools" / "racescan.py"), "--build", str(build),
                    "--out", str(out)], check=True, capture_output=True)
    d = json.loads((out / "candidates.json").read_text())
    shared = {s["var"].split("::")[-1]: s for s in d["shared"]}
    fails: list[str] = []

    def want(cond: bool, what: str) -> None:
        if not cond:
            fails.append(what)

    want("g_shared_counter" in shared and shared["g_shared_counter"]["group"] == "A"
         and "isr:isr" in shared["g_shared_counter"]["written_in"], "g_shared_counter: A with the ISR")
    want("g_boot_cfg" in shared and shared["g_boot_cfg"]["group"] == "B", "g_boot_cfg: B")
    want("g_text" in shared and shared["g_text"]["group"] == "A", "g_text: A (filled through a call)")
    for v in ("g_only_task", "g_table", "g_by_const_ptr", "g_lock"):
        want(v not in shared, f"{v}: not listed")
    want(shared.get("g_marked", {}).get("marked", "").lower().startswith("guarded by"), "g_marked: marked")
    for v in ("g_grp_b", "g_grp_c"):
        want(shared.get(v, {}).get("marked", "").lower().startswith("interrupt-shared"),
             f"{v}: marked by the group's comment")
    want("g_after_blank" in shared, "g_after_blank: listed")
    want(not shared.get("g_after_blank", {}).get("marked"), "g_after_blank: a blank line ends the group")
    for v in ("g_guarded_a", "g_guarded_b"):
        want(v not in shared, f"{v}: behind a devirq_in_handler() guard, not reached from the handler")
    want(shared.get("g_slots", {}).get("group") == "A", "g_slots: written through `T *p = &g_slots[i]`")
    for v in ("g_via_const", "g_ptr_var"):
        want(v not in shared, f"{v}: a const pointer or a copied pointer is not a write")
    want(shared.get("g_via_table", {}).get("group") == "A", "g_via_table: reached through a const ops table")
    blk = {(b["context"], b["function"]) for b in d["isr_blocking"]}
    want(("isr:isr2", "sched_yield") in blk, "isr2 reaches a blocking call")
    want(not any(c == "isr:isr" for c, _ in blk), "isr's blocking calls are all behind guards")
    want(any("fixture.c" in f for f in d["mmio"]) and len(next(iter(d["mmio"].values()))) >= 2,
         "register access in two contexts")
    cc = {c["function"]: c for c in d["chan_calls"]}
    want(cc.get("worker", {}).get("loop_reads_clock") is True, "worker's chan_call loop reads the clock")
    want(cc.get("other", {}).get("in_loop") is True and cc["other"]["loop_reads_clock"] is False,
         "other's chan_call loop counts attempts")
    for f in fails:
        print("FAIL", f)
    print(f"racescan fixture: {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
