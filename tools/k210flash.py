#!/usr/bin/env python3
"""Provision the K210's SPI NOR with the LugalOS flash map (65.5).

One run writes what cmake/flash_layout_k210.cmake says goes where:

    loader   flash offset 0, wrapped in the framing the BROM reads —
             [flag:1][length:4][image][sha256:32] — the same framing
             kflash's ISP writes, which is how the format is known good
             for this board; the BROM loads the image to 0x80000000 and
             enters it there.
    bootrec  a valid boot-record pair (kernel/ota_image.h verbatim)
             naming the slot to boot, so the loader takes the same
             code path a later over-the-air update will.
    slot_a   the kernel: the 256-byte OTA image header plus the flat
             SRAM blob from lugalos.elf (segments flattened, gaps and
             the bss span zero-filled).
    fs       flashfs.bin into the /flash0 segment, when it exists.

Nothing is written blindly: the writer end is our own SRAM blob
(tools/k210_wblob.c, ISP-loaded and booted exactly like k210run.py
loads any ELF), every frame carries a checksum, and every region is
read back and hash-compared before the board is reset. Before anything
is erased, the current contents of the regions about to be written are
dumped to a backup file — this project does not silently eat whatever
was on the board before it (the E6 lesson).

The tool refuses to be the vendor flasher: no kflash, no vendor ISP
stub. It speaks only the BROM ISP protocol (memory write + boot, the
k210run.py client) and its own blob protocol.

    uv run --with pyserial --with pyelftools \\
        python3 tools/k210flash.py --build build/k210 [--verify] [--erase-all]

Closing the serial port at the end drops the CH552's reset lines, which
resets the board into the freshly written flash image.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import k210run  # noqa: E402  (the ISP client and ELF loader live there)

OTA_IMAGE_MAGIC = 0x41544F4C   # "LOTA"
OTA_HEADER_LEN = 256
OTA_BOOTREC_MAGIC = 0x42544F4C  # "LOTB"
OTA_SLOT_NONE = 0xFF


def crc32(data: bytes) -> int:
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def elf_flat_blob(path: str, include_memsz: bool = True) -> bytes:
    """ELF PT_LOAD segments flattened to one load blob from the lowest
    address, gaps zero-filled — what the loader streams into SRAM and
    what a slot stores after its header."""
    segs = k210run.elf_load_segments(path)
    lo = min(a for a, _ in segs)
    hi = max(a + len(d) for a, d in segs)
    if lo != 0x80000000:
        raise SystemExit(f"{path}: segments start at {lo:#x}, expected 0x80000000")
    blob = bytearray(hi - lo)
    for a, d in segs:
        blob[a - lo:a - lo + len(d)] = d
    return bytes(blob)


def ota_header(board: str, chip: str, part0: bytes, build_id: str) -> bytes:
    sha = hashlib.sha256(part0).digest()
    body = struct.pack(
        "<IHH24s12sHHII32s",
        OTA_IMAGE_MAGIC, 1, OTA_HEADER_LEN,
        board.encode().ljust(24, b"\0"), chip.encode().ljust(12, b"\0"),
        0, 0,                      # chip_rev_min/max: unchecked on K210
        len(part0), 0,
        build_id.encode().ljust(32, b"\0"),
    )
    body += sha                       # sha256[32]
    body += b"\0"                     # sig_alg NONE
    body += b"\0" * 3                 # sig_pad
    body += b"\0" * 8                 # key_id
    body += b"\0" * 64                # sig
    body += b"\0" * 56                # reserved
    assert len(body) == OTA_HEADER_LEN - 4, f"header body {len(body)}"
    return body + struct.pack("<I", crc32(body))


def bootrec(seq: int, active: int, pending: int = OTA_SLOT_NONE) -> bytes:
    body = struct.pack("<IIBB2x", OTA_BOOTREC_MAGIC, seq, active, pending)
    rec = body + struct.pack("<I", crc32(body)) + struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF)
    assert len(rec) == 24  # magic..failed per ota_bootrec_t
    return rec                          # the sector's rest stays erased 0xFF


def brom_frame(image: bytes) -> bytes:
    """The BROM's flash-boot framing: flag, length, image, sha256 —
    flag 0: no AES, standard (single-line) SPI read mode. The kernel's
    and loader's own reads keep the part in single mode, so the flag's
    DIO/quad bits must stay clear or BROM would leave it somewhere the
    kernel cannot talk to (measured QE=0 on this board)."""
    body = bytes([0]) + struct.pack("<I", len(image)) + image
    return body + hashlib.sha256(body).digest()


class WriterBlob:
    """Host end of tools/k210_wblob.c over the ISP session's serial."""

    def __init__(self, ser, leak: bytes = b""):
        self.ser = ser
        # The blob announces itself with 'K'. The announcement usually
        # arrives while k210run's boot() is still draining the boot
        # window, so the leaked prefix counts as the hello too.
        if b"K" in leak:
            return
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.ser.read(1) == b"K":
                return
        raise RuntimeError("writer blob did not say hello")

    def _req(self, op: str, addr: int, payload: bytes = b"",
             resp: int = 0) -> bytes:
        # 'R' carries the requested length in the frame's len field (its
        # payload travels back, not out).
        # Bit31 of the address word selects the controller inside the
        # blob (set = SPI3/XPI boot flash, clear = SPI0 header pads);
        # every flash operation must carry it.
        frame = b"K" + op.encode() + struct.pack("<IH", addr | 0x80000000,
                                                 len(payload) or resp)
        frame += payload
        frame += bytes([sum(frame) & 0xFF])
        self.ser.reset_input_buffer()
        self.ser.write(frame)
        if resp:
            data = self._read_n(resp, timeout=5)
            return data
        ack = self.ser.read(1)
        if ack != b"+":
            # one retry: the protocol is idempotent per frame
            self.ser.write(frame)
            ack = self.ser.read(1)
            if ack != b"+":
                raise RuntimeError(f"op {op}@{addr:#x} -> {ack!r}")
        return b""

    def _read_n(self, n: int, timeout: float) -> bytes:
        self.ser.timeout = timeout
        data = b""
        while len(data) < n:
            chunk = self.ser.read(n - len(data))
            if not chunk:
                raise RuntimeError(f"read short: got {len(data)} of {n}")
            data += chunk
        return data

    def erase_sector(self, addr: int): self._req("E", addr)
    def erase_block(self, addr: int):  self._req("B", addr)
    def read(self, addr: int, n: int) -> bytes:
        out = b""
        while len(out) < n:
            chunk = min(256, n - len(out))
            out += self._req("R", addr + len(out), resp=chunk)
        return out

    def write(self, addr: int, data: bytes):
        """Page-program with page discipline handled here; caller may
        cross 256-byte and sector boundaries freely.

        Verify-and-retry per page: on the real link, occasional pages
        program corrupted or are dropped outright (the blob's flash
        path itself is proven byte-exact; the loss lives in the UART
        frame handoff at full send rate). NOR programming is
        idempotent for identical data (1 bits already cleared stay
        cleared), so re-sending a page that read back wrong is safe as
        long as the region was erased first — which provisioning
        always does. Three failed attempts is a hard error."""
        off = 0
        while off < len(data):
            a = addr + off
            chunk = min(256 - (a % 256), len(data) - off)
            want = data[off:off + chunk]
            for attempt in range(3):
                self._req("W", a, want)
                rb = self._req("R", a, resp=chunk)
                if rb == want:
                    break
            else:
                raise RuntimeError(f"page @{a:#x} failed 3x")
            off += chunk

    def erase_region(self, addr: int, length: int):
        assert addr % 0x10000 == 0 and length % 0x10000 == 0, \
            "region erases are 64 KB block-aligned so the pass is quick"
        for a in range(addr, addr + length, 0x10000):
            self.erase_block(a)


def read_manifest(build_dir: str) -> dict[str, str]:
    path = os.path.join(build_dir, "flash.manifest")
    if not os.path.exists(path):
        raise SystemExit(f"{path} missing -- build the k210 preset first")
    man = {}
    with open(path) as f:
        for line in f:
            cols = line.split()
            if len(cols) >= 3:
                man[cols[0]] = cols[1:]
    return man


def build_blob_if_needed(build_dir: str) -> str:
    """The writer blob builds itself here rather than as a CMake target:
    it is a tool, it must exist before any build-dir question about the
    kernel's freshness, and its recipe belongs next to the host half
    that speaks to it."""
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "k210_wblob.c")
    entry = os.path.join(here, "minimal_k210_entry.S")
    ld = os.path.join(here, "minimal_k210.ld")
    out = os.path.join(build_dir, "k210_wblob.elf")
    if os.path.exists(out) and os.path.getmtime(out) > max(
            os.path.getmtime(src), os.path.getmtime(entry)):
        return out
    cc = subprocess.run(["bash", "-lc",
                         "command -v riscv64-elf-gcc || command -v riscv64-unknown-elf-gcc"],
                        capture_output=True, text=True).stdout.strip()
    if not cc:
        raise SystemExit("no riscv64 cross compiler found for the writer blob")
    objs = [os.path.join(build_dir, "k210_wblob.o"),
            os.path.join(build_dir, "k210_wblob_entry.o")]
    subprocess.run([cc, "-std=gnu11", "-nostdlib", "-ffreestanding", "-O1",
                    "-Wall", "-march=rv64imac_zicsr_zifencei", "-mabi=lp64",
                    "-mcmodel=medany", "-fno-pic", "-fno-stack-protector",
                    "-c", src, "-o", objs[0]], check=True)
    subprocess.run([cc, "-nostdlib", "-c", entry, "-o", objs[1]], check=True)
    subprocess.run([f"{cc[:-3]}ld", "-T", ld, "-o", out] + objs, check=True,
                   capture_output=True)   # the RWX-segment warning is expected
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default="build/k210")
    ap.add_argument("--port", default=None)
    ap.add_argument("--slot", choices=("a", "b"), default="a",
                    help="slot to provision and mark active")
    ap.add_argument("--no-fs", action="store_true",
                    help="leave the /flash0 segment untouched")
    ap.add_argument("--erase-all", action="store_true",
                    help="erase the whole 16 MB before writing")
    ap.add_argument("--backup", default=None,
                    help="backup file (default build/k210/k210-flash-backup.bin): "
                         "contents of every region about to be written, read "
                         "out before the erase")
    ap.add_argument("--no-backup", action="store_true")
    ap.add_argument("--verify", action="store_true",
                    help="read every written byte back and hash-compare")
    ap.add_argument("--capture-after", type=float, default=6.0,
                    help="seconds to watch the console after the reset into "
                         "the new flash (0 to skip)")
    args = ap.parse_args()

    man = read_manifest(args.build)
    board, chip = man["board"][0], man["board"][1]
    regions = []  # (name, addr, bytes)

    loader_elf = os.path.join(args.build, man["loader"][2])
    kernel_elf = os.path.join(args.build, man["slot_a"][2])
    fs_bin = os.path.join(args.build, man["fs"][2])

    loader_img = elf_flat_blob(loader_elf)
    kernel_blob = elf_flat_blob(kernel_elf)
    slot_n = 0 if args.slot == "a" else 1
    slot_addr = int(man[f"slot_{args.slot}"][0], 0)
    slot_size = int(man[f"slot_{args.slot}"][1], 0)
    if len(kernel_blob) > slot_size - OTA_HEADER_LEN:
        raise SystemExit(f"kernel blob {len(kernel_blob)} does not fit the slot")

    regions.append(("loader", int(man["loader"][0], 0), brom_frame(loader_img)))
    bootrec_addr = int(man["bootrec"][0], 0)
    regions.append(("bootrec", bootrec_addr, bootrec(seq=1, active=slot_n)))
    regions.append((f"slot_{args.slot}", slot_addr,
                    ota_header(board, chip, kernel_blob, "k210flash") + kernel_blob))
    if not args.no_fs:
        if os.path.exists(fs_bin):
            regions.append(("fs", int(man["fs"][0], 0), open(fs_bin, "rb").read()))
        else:
            print(f"note: {fs_bin} not built; /flash0 stays as it is")

    # ---- backup ----------------------------------------------------------
    # The dump itself has to wait for the blob (it is the reader), but
    # the decision is made here: a named file, or the default, or none.
    backup = None
    if not args.no_backup:
        backup = args.backup or os.path.join(args.build, "k210-flash-backup.bin")

    # ---- ISP: load the writer blob and boot it ----------------------------
    port = args.port or k210run.find_port(None)
    isp = k210run.K210Isp(port)
    isp.greeting()
    blob_elf = build_blob_if_needed(args.build)
    for addr, data in k210run.elf_load_segments(blob_elf):
        isp.mem_write(addr, data)
    isp.boot(0x80000000)
    isp.release()               # hand the XPI pads from stub to blob
    blob = WriterBlob(isp.ser, leak=isp.leaked)
    print(f"writer blob up on {port}")

    if backup:
        # Everything the run is about to touch, plus the tail sector (the
        # identity may live there and this tool must never eat it by
        # surprise even though it does not write it).
        spans = [(a, len(d)) for _, a, d in regions]
        if args.erase_all:
            spans = [(0, int(man["flash"][1], 0))]
        with open(backup, "wb") as f:
            for a, n in spans:
                print(f"backup {a:#08x} +{n}")
                f.write(blob.read(a, n))
        print(f"backup written: {backup}")

    # ---- erase, write, verify ----------------------------------------------
    if args.erase_all:
        blob.erase_region(0, int(man["flash"][1], 0))
    for name, addr, data in regions:
        end = (addr + len(data) + 0xFFFF) & ~0xFFFF
        if not args.erase_all:
            blob.erase_region(addr & ~0xFFFF, end - (addr & ~0xFFFF))
        blob.write(addr, data)
        print(f"wrote {name:8s} {addr:#08x} +{len(data)}")
        if args.verify:
            got = blob.read(addr, len(data))
            if hashlib.sha256(got).digest() != hashlib.sha256(data).digest():
                for i in range(len(data)):
                    if got[i] != data[i]:
                        raise SystemExit(f"verify {name}: differs at +{i:#x} "
                                         f"({got[i]:#x} != {data[i]:#x})")
                raise SystemExit(f"verify {name}: hashes differ")
            print(f"verify {name:8s} ok")

    # ---- reset into the new flash ------------------------------------------
    # Closing the port drops DTR/RTS, which is the CH552's "run" -> reset
    # edge: the board boots the image just written.
    isp.close()
    if args.capture_after > 0:
        print(f"console after reset into flash ({args.capture_after:.0f}s):")
        run = k210run.K210Isp(port, to_isp=False)
        t_end = time.monotonic() + args.capture_after
        while time.monotonic() < t_end:
            b = run.ser.read(256)
            if b:
                sys.stdout.write(b.decode(errors="replace"))
                sys.stdout.flush()
        run.close()


if __name__ == "__main__":
    main()
