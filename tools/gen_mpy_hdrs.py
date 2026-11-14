#!/usr/bin/env python3
"""Generate MicroPython build headers (mpversion.h, qstrdefs.generated.h,
moduledefs.h, root_pointers.h) for the LugalOS U-mode MicroPython ELF.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=True, text=True, **kwargs)


def write_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--mpy-dir", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--cflag", action="append", default=[])
    ap.add_argument("--inc", action="append", default=[])
    ap.add_argument("sources", nargs="+")
    args = ap.parse_args()

    mpy_dir = Path(args.mpy_dir)
    py_dir = mpy_dir / "py"
    out_dir = Path(args.out_dir)
    genhdr = out_dir / "genhdr"
    tmp_dir = out_dir / "tmp"
    genhdr.mkdir(parents=True, exist_ok=True)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    # 1. mpversion.h (write fallback if makeversionhdr.py cannot find mpy git tags)
    ver_h = genhdr / "mpversion.h"
    try:
        run([sys.executable, str(py_dir / "makeversionhdr.py"), str(ver_h)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        write_if_changed(
            ver_h,
            '#define MICROPY_GIT_TAG "v1.24.1-lugalos"\n'
            '#define MICROPY_GIT_HASH "v1.24.1"\n'
            '#define MICROPY_BUILD_DATE "2026-11-14"\n',
        )

    # 2. Preprocess all C sources with -DNO_QSTR
    cc_cmd = [args.cc, "-E", "-DNO_QSTR"]
    cc_cmd.extend(args.cflag)
    for inc in args.inc:
        cc_cmd.extend(["-I", inc])
    cc_cmd.extend(["-I", str(out_dir)])

    pp_file = tmp_dir / "all_pp.i"
    with pp_file.open("w", encoding="utf-8") as fout:
        # Feed one combined translation unit? No, preprocess individually so
        # per-file #includes and static guards don't collide.
        for src in args.sources:
            res = subprocess.run(
                cc_cmd + [src], check=True, stdout=subprocess.PIPE, text=True
            )
            fout.write(f'# 1 "{src}"\n')
            fout.write(res.stdout)

    # 3. Extract qstr, module, root_pointer definitions
    makeqstrdefs = str(py_dir / "makeqstrdefs.py")
    for mode in ("qstr", "module", "root_pointer"):
        mode_dir = tmp_dir / mode
        mode_dir.mkdir(parents=True, exist_ok=True)
        run([sys.executable, makeqstrdefs, "split", mode, str(pp_file), str(mode_dir), "_"])
        collected = tmp_dir / f"{mode}.collected"
        run([sys.executable, makeqstrdefs, "cat", mode, "_", str(mode_dir), str(collected)])

    # 4. qstrdefs.generated.h
    qstr_pp_in = tmp_dir / "qstrdefs_all.h"
    qstr_base = (py_dir / "qstrdefs.h").read_text(encoding="utf-8")
    qstr_port = (Path(args.inc[0]) / "qstrdefsport.h").read_text(encoding="utf-8") if (Path(args.inc[0]) / "qstrdefsport.h").exists() else ""
    qstr_collected = (tmp_dir / "qstr.collected").read_text(encoding="utf-8")
    qstr_pp_in.write_text(qstr_base + "\n" + qstr_port + "\n" + qstr_collected + "\n", encoding="utf-8")

    # Preprocess qstrdefs_all.h using makeqstrdefs pp-style or direct CC -E
    qstr_pp_out = tmp_dir / "qstrdefs.preprocessed.h"
    res = subprocess.run(
        [args.cc, "-E", "-P", "-I", str(out_dir)]
        + [x for inc in args.inc for x in ("-I", inc)]
        + args.cflag
        + [str(qstr_pp_in)],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    qstr_pp_out.write_text(res.stdout, encoding="utf-8")

    qstr_gen = subprocess.run(
        [sys.executable, str(py_dir / "makeqstrdata.py"), str(qstr_pp_out)],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout
    write_if_changed(genhdr / "qstrdefs.generated.h", qstr_gen)

    # 5. moduledefs.h
    mod_gen = subprocess.run(
        [sys.executable, str(py_dir / "makemoduledefs.py"), str(tmp_dir / "module.collected")],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout
    write_if_changed(genhdr / "moduledefs.h", mod_gen)

    # 6. root_pointers.h
    rp_gen = subprocess.run(
        [sys.executable, str(py_dir / "make_root_pointers.py"), str(tmp_dir / "root_pointer.collected")],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout
    write_if_changed(genhdr / "root_pointers.h", rp_gen)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
