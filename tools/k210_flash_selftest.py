#!/usr/bin/env python3
"""K210 NOR flash self-test over the SRAM writer blob.

Run immediately after a physical power cycle (unplug/replug) so the chip
starts from POR. Loads tools/k210_wblob.c via BROM ISP, then exercises:
RDID -> raw read (expect FF after erase) -> sector erase -> page program
-> full-sector program/read. Prints a verdict per step.

Usage: uv run --with pyserial --with pyelftools python3 tools/k210_flash_selftest.py
"""
import sys
import time

sys.path.insert(0, __file__.removesuffix("k210_flash_selftest.py"))
import k210run          # noqa: E402
import k210flash        # noqa: E402


def main() -> int:
    port = k210run.find_port(None)
    isp = k210run.K210Isp(port)
    isp.greeting()
    for addr, data in k210run.elf_load_segments("build/k210/k210_wblob.elf"):
        isp.mem_write(addr, data)
    isp.boot(0x80000000)
    isp.release()
    blob = k210flash.WriterBlob(isp.ser, leak=b"K")

    ok = True
    rdid = blob._req("I", 0, resp=3).hex()
    print(f"RDID           = {rdid}  ({'OK' if rdid == 'c86018' else 'BAD'})")
    ok &= rdid == "c86018"

    head = blob.read(0x912000, 32)
    print(f"read 0x912000  = {head.hex()}")

    blob.erase_sector(0x912000)
    era = blob.read(0x912000, 32)
    good = era == b"\xff" * 32
    print(f"post-erase     = {era.hex()}  ({'OK FF' if good else 'NOT FF'})")
    ok &= good

    pat = bytes([(i * 7 + 0x31) & 0xFF for i in range(256)])
    blob.write(0x912000, pat)
    rb = blob.read(0x912000, 256)
    good = rb == pat
    print(f"page program   = {'PERFECT' if good else f'got {rb[:16].hex()}'}")
    ok &= good

    pat2 = bytes([(0x11 * ((i // 16) % 16)) & 0xFF for i in range(4096)])
    # re-erase: NOR programming is AND-only, the single-page pattern
    # above must not linger under the full-sector pattern
    blob.erase_sector(0x912000)
    for off in range(0, 4096, 256):
        blob.write(0x912000 + off, pat2[off:off + 256])
    rb = blob.read(0x912000, 4096)
    bad = sum(1 for i in range(4096) if rb[i] != pat2[i])
    print(f"full sector    = {'PERFECT' if not bad else f'{bad} bad bytes'}")
    ok &= not bad

    print("VERDICT:", "FLASH PASS" if ok else "FLASH FAIL")
    isp.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
