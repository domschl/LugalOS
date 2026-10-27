#!/usr/bin/env python3
"""65.4: the "unenforced" domain verdict must be reachable ONLY on the K210.

The no-enforcement mode exists because K210 silicon has no PMP; on every
other target "unenforced" would be a configuration lying about hardware
that exists. This test reads the one file that decides the verdict
(arch/riscv/common/mem_domain.c) and asserts:

  1. exactly one backend reports enforced()==false unconditionally, and it
     lives inside the CONFIG_BOARD_K210 arm;
  2. the K210 arm cannot be selected by any other configuration symbol;
  3. the PMP and Sv39 backends still report enforced()==true / paging-
     conditioned truth.
"""
import re
import sys
import pathlib

SRC = pathlib.Path(__file__).resolve().parent.parent / "arch" / "riscv" / "common" / "mem_domain.c"

def main() -> int:
    text = SRC.read_text()

    # Split on the backend guards.
    k210_arm = re.search(r"#if defined\(CONFIG_BOARD_K210\)(.*?)#elif defined\(CONFIG_MODE_M\)", text, re.S)
    pmp_arm  = re.search(r"#elif defined\(CONFIG_MODE_M\)(.*?)#else /\* CONFIG_MODE_S", text, re.S)
    sv39_arm = re.search(r"#else /\* CONFIG_MODE_S.*?\*/(.*?)\n#endif", text, re.S)
    assert k210_arm, "no CONFIG_BOARD_K210 backend arm found in mem_domain.c"
    assert pmp_arm, "PMP backend arm missing or its guard changed shape"
    assert sv39_arm, "Sv39 backend arm missing or its guard changed shape"

    # 1. exactly one unconditional enforced()=false, and it is K210's.
    falses = re.findall(r"bool mem_domain_enforced\(void\) \{ return false; \}", text)
    assert len(falses) == 1, f"expected exactly one unenforced backend, got {len(falses)}"
    assert "mem_domain_enforced(void) { return false; }" in k210_arm.group(1), \
        "the unenforced backend is not inside the K210 arm"

    # 2. no other config symbol selects it: the arm's #if mentions K210 only.
    guard = k210_arm.start()
    line = text[:guard].count("\n")
    guard_line = text.splitlines()[line]
    assert "CONFIG_BOARD_K210" in guard_line and "||" not in guard_line, \
        f"K210 arm guard is broader than K210: {guard_line!r}"

    # 3. the real backends keep claiming what their hardware has.
    assert re.search(r"mem_domain_enforced\(void\) \{ return true; \}", pmp_arm.group(1)), \
        "PMP arm no longer reports enforced()=true"
    assert "vmm_paging_enabled()" in sv39_arm.group(1), \
        "Sv39 arm no longer conditions enforcement on paging"

    # 4. the status sentence is the plan's, for K210.
    assert "unenforced (silicon: no pmp)" in k210_arm.group(1), \
        "K210 status string drifted from the plan's wording"

    print("PASS: unenforced-domain verdict is reachable only on CONFIG_BOARD_K210")
    return 0

if __name__ == "__main__":
    sys.exit(main())
