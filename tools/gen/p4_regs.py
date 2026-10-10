#!/usr/bin/env python3
"""Resolve IDF register/field macros to numbers, for both hw_ver1 and hw_ver3,
and fail if they disagree. Prints C #defines (address, _S, _V).

A line starting with `v3!` names a register whose fields exist only on (or
differ on) v3 silicon: it is resolved from hw_ver3 alone and the output says
so, for drivers that only ever run on v3 (the LCD-7B's display, 47.4)."""
import re, sys, pathlib
R = pathlib.Path.home()/"Source/gith/esp/esp-idf/components/soc/esp32p4/register"
def load(ver):
    d = {}
    for f in (R/ver/"soc").glob("*.h"):
        t = re.sub(r"/\*.*?\*/", "", f.read_text(errors="ignore"), flags=re.S)
        t = re.sub(r"//[^\n]*", "", t)
        for m in re.finditer(r"^#define\s+(\w+)\s+(.+?)\s*$", t, re.M):
            d.setdefault(m.group(1), m.group(2))
    return d
def ev(d, name, depth=0):
    e = d[name]
    e = re.sub(r"\bBIT\((\d+)\)", r"(1<<\1)", e)
    e = re.sub(r"(\d+|0x[0-9a-fA-F]+)[Uu][Ll]?\b", r"\1", e)
    for tok in set(re.findall(r"\b[A-Z_][A-Z0-9_]+\b", e)):
        e = re.sub(r"\b%s\b" % tok, "(%d)" % ev(d, tok, depth+1), e)
    return int(eval(e))
v1, v3 = load("hw_ver1"), load("hw_ver3")
out = []
for line in sys.stdin:
    line = line.strip()
    if not line or line.startswith("#"): continue
    v3_only = line.startswith("v3!")
    if v3_only: line = line[3:].strip()
    reg, fields = line.split(":", 1) if ":" in line else (line, "")
    reg = reg.strip()
    a3 = ev(v3, reg)
    if v3_only:
        out.append("/* v3 only: %s's fields as hw_ver3 has them */" % reg)
    else:
        a1 = ev(v1, reg)
        assert a1 == a3, (reg, hex(a1), hex(a3))
    out.append("#define %-44s 0x%08xu" % (reg, a3))
    for f in fields.split():
        for suf in ("_S", "_V"):
            if v3_only: continue
            x1, x3 = ev(v1, f+suf), ev(v3, f+suf)
            assert x1 == x3, (f+suf, x1, x3)
        out.append("#define %-44s %du" % (f+"_S", ev(v3, f+"_S")))
        out.append("#define %-44s 0x%xu" % (f+"_V", ev(v3, f+"_V")))
print("\n".join(out))
