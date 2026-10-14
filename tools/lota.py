#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial"]
# ///
"""Build a LugalOS update image, and update a node with it over 9P.

    lota.py image build/esp32c6-mini                 # -> build/esp32c6-mini/lugalos-ota.img
    lota.py push build/esp32c6-mini --p4 lcd7b --prefix /c6 --reboot --wait
    lota.py push lugalos-ota.img --tcp 192.168.178.160 --keyfile ~/.config/lugalos/p9.key \\
            --prefix /c6 --reboot --wait
    lota.py status --p4 lcd7b --prefix /c6

The image (kernel/include/kernel/ota_image.h) is a 256-byte header -- this
board, this chip, the build id, the SHA-256 of what follows -- and the build's
two parts. `push` writes it to <prefix>/dev/otaimage after `begin SIZE` on
<prefix>/dev/ota, then `commit` (the node checks the header and the SHA-256 of
what landed in flash), and with --reboot restarts the node into it. --wait
watches /dev/ota until the new image has confirmed itself, or the node has
fallen back to the old one.

Any 9P path to the node will do, which is the point: --prefix /c6 reaches the
C6 beside a P4 through the P4's own namespace, over the P4's console (--p4,
9P on the console wire as tools/p9sync.py does it) or over TCP to the P4.

--break is for testing the fall-back: it zeroes 64 KB of the kernel's code
before the hash is taken, so the image passes every check, boots, and dies.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import sys
import time
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

MAGIC = 0x41544F4C
VERSION = 1
HEADER_LEN = 256
HEADER_FMT = "<IHH24s12sHHII32s32s132sI"
assert struct.calcsize(HEADER_FMT) == HEADER_LEN


def read_manifest(build: Path) -> dict[str, list[str]]:
    man = {}
    for line in open(build / "flash.manifest"):
        f = line.split()
        if f:
            man[f[0]] = f[1:]
    return man


def build_id(build: Path) -> str:
    m = re.search(r'LUGALOS_BUILD_ID "([^"]*)"', (build / "lugalos_build_id.h").read_text())
    if not m:
        sys.exit(f"no build id in {build}/lugalos_build_id.h")
    return m.group(1)


def header(board: str, chip: str, bid: str, parts: list[bytes]) -> bytes:
    sha = hashlib.sha256(b"".join(parts)).digest()
    body = struct.pack(HEADER_FMT, MAGIC, VERSION, HEADER_LEN, board.encode(), chip.encode(),
                       0, 0xFFFF, len(parts[0]), len(parts[1]), bid.encode()[:31], sha, bytes(132), 0)
    return body[:-4] + struct.pack("<I", zlib.crc32(body[:-4]))


def parts_esp32c6(build: Path, flash_size: int) -> list[bytes]:
    """Part 0: the RAM half in the boot ROM's image format; part 1: the XIP half."""
    import c6run
    ram_elf = build / "lugalos-ram.elf"
    ram_img = build / "lugalos-ram.img"
    r = c6run.esptool("elf2image", "--flash-mode", "dio", "--flash-freq", "40m",
                      "--flash-size", "%dMB" % (flash_size >> 20), "-o", str(ram_img), str(ram_elf))
    if r.returncode != 0:
        sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-600:])
    return [ram_img.read_bytes(), (build / "lugalos-xip.bin").read_bytes()]


def make_image(build: Path, broken: bool = False) -> tuple[bytes, dict]:
    man = read_manifest(build)
    if "board" not in man:
        sys.exit(f"{build}/flash.manifest names no board: rebuild it")
    board, chip = man["board"]
    if chip != "esp32c6":
        sys.exit(f"no update image format for {chip} yet")
    parts = parts_esp32c6(build, int(man["flash"][1], 0))
    if broken:
        # The kernel's own text starts 512 KB into the XIP half (linker/esp32c6.ld:
        # the radio's block comes first). Zeroes are illegal instructions.
        xip = bytearray(parts[1])
        xip[0x80000:0x90000] = bytes(0x10000)
        parts[1] = bytes(xip)
    hdr = header(board, chip, build_id(build), parts)
    info = {"board": board, "chip": chip, "build_id": build_id(build),
            "part_len": [len(p) for p in parts], "sha256": hashlib.sha256(b"".join(parts)).hexdigest()}
    return hdr + b"".join(parts), info


def slot_image(image: bytes, part_offsets: tuple[int, int]) -> bytes:
    """The image as it lies in a slot (drivers/include/drivers/ota_esp32c6.h):
    header at 0, the parts at their offsets, erased flash (0xFF) between."""
    _, _, _, _, _, _, _, l0, l1, *_ = struct.unpack(HEADER_FMT, image[:HEADER_LEN])
    p0, p1 = image[HEADER_LEN:HEADER_LEN + l0], image[HEADER_LEN + l0:HEADER_LEN + l0 + l1]
    out = bytearray(b"\xff" * (part_offsets[1] + l1))
    out[0:HEADER_LEN] = image[:HEADER_LEN]
    out[part_offsets[0]:part_offsets[0] + l0] = p0
    out[part_offsets[1]:] = p1
    return bytes(out)


# --- talking to the node -------------------------------------------------------

class Node:
    """A 9P session to the node, and the paths of its update files."""

    def __init__(self, a: argparse.Namespace) -> None:
        import p9lib
        self.p4 = a.p4
        self.prefix = a.prefix.rstrip("/")
        key = None
        if a.keyfile:
            key = bytes.fromhex(Path(a.keyfile).expanduser().read_text().split()[0])
        elif a.key:
            key = bytes.fromhex(a.key)
        if a.p4:
            import p4run
            import p9sync
            self.port, _ = p4run.board_ports(a.p4)
            p9sync.p4_shell(self.port, "p9share")
            self.client = p9lib.connect_open_serial(p4run.open_console(self.port), framing="slip",
                                                    timeout=30.0, closer=p4run.close_console)
        elif a.tcp:
            host, _, port = a.tcp.partition(":")
            self.client = p9lib.connect_tcp(host, int(port or 564), timeout=30.0)
        else:
            sys.exit("say how to reach the node: --p4 BOARD or --tcp HOST[:PORT]")
        self.sess = p9lib.Session(self.client, aname="/", key=key)

    def close(self) -> None:
        try:
            self.client.close()
        finally:
            if self.p4:
                import p9sync
                time.sleep(0.3)
                p9sync.p4_shell(self.port, "p9share off")

    def status(self) -> dict[str, str]:
        text = self.sess.read(self.prefix + "/dev/ota").decode(errors="replace")
        return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)

    def ctl(self, cmd: str) -> None:
        self.sess.write(self.prefix + "/dev/ota", cmd.encode(), create=False, truncate=False)

    def send(self, image: bytes, rate_kbs: float = 0.0) -> None:
        from p9lib.client import OWRITE
        c = self.client
        fid = self.sess._alloc_work_fid()
        self.sess._walk_or_raise(fid, self.sess._split(self.prefix + "/dev/otaimage"), "otaimage", "walk to")
        try:
            c.open(fid, mode=OWRITE)
            chunk = c.io_chunk()
            t0 = time.monotonic()
            off = 0
            while off < len(image):
                try:
                    n = c.write(fid, off, image[off:off + chunk])
                except Exception as e:
                    raise RuntimeError(f"writing at offset {off} of {len(image)} after "
                                       f"{time.monotonic() - t0:.1f} s: {e}") from e
                if n <= 0:
                    raise RuntimeError(f"the node took nothing at offset {off}")
                off += n
                if rate_kbs > 0:
                    ahead = off / 1024 / rate_kbs - (time.monotonic() - t0)
                    if ahead > 0:
                        time.sleep(ahead)
                if (off // chunk) % 32 == 0 or off == len(image):
                    rate = off / 1024 / max(time.monotonic() - t0, 1e-3)
                    print(f"\r  {off}/{len(image)} bytes, {rate:.0f} KB/s", end="", flush=True)
            print()
        finally:
            c.clunk(fid)


def show(st: dict[str, str]) -> str:
    keys = ("board", "running", "trial", "fell_back", "slot_a", "slot_b", "active", "pending", "state", "error")
    return ", ".join(f"{k}={st[k]}" for k in keys if k in st)


def push(a: argparse.Namespace) -> int:
    src = Path(a.source)
    if src.is_dir():
        image, info = make_image(src, broken=a.broken)
    else:
        image = src.read_bytes()
        info = {"board": image[8:32].rstrip(b"\0").decode(), "build_id": image[56:88].rstrip(b"\0").decode()}
    print(f"image: {len(image)} bytes for {info['board']}, build {info.get('build_id', '?')}"
          f"{' (BROKEN on purpose)' if a.broken else ''}")
    node = Node(a)
    try:
        st = node.status()
        print("node:", show(st))
        if st.get("supported") == "no":
            sys.exit("the node cannot update itself")
        if st.get("board") != info["board"] and not a.no_board_check:
            sys.exit(f"the node is a {st.get('board')}, the image is for {info['board']}")
        old_slot = st.get("running")
        try:
            node.ctl(f"begin {len(image)}")
            node.send(image, a.rate)
            node.ctl("commit")
        except Exception as e:
            # The node says why in /dev/ota; a 9P write error says only that it failed.
            why = node.status().get("error", str(e))
            print(f"\nthe node refused the update: {why}")
            node.ctl("abort")
            return 1
        st = node.status()
        print("committed:", show(st))
        if not a.reboot:
            return 0
        node.ctl("reboot")
        if not a.wait:
            print("rebooting")
            return 0
        print(f"rebooting; waiting up to {a.wait_s} s for the new image to confirm")
        end = time.monotonic() + a.wait_s
        seen_trial = False
        while time.monotonic() < end:
            time.sleep(3)
            try:
                st = node.status()
            except Exception:
                continue                          # the node (or its mount) is restarting
            if st.get("running") != old_slot and st.get("trial") == "no":
                print("confirmed:", show(st))
                return 0
            if st.get("running") == old_slot and st.get("fell_back") == "yes":
                print("fell back to the old image:", show(st))
                return 2
            if st.get("trial") == "yes" and not seen_trial:
                seen_trial = True
                print("on trial:", show(st))
        print("no answer in time; last seen:", show(st))
        return 1
    finally:
        node.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    im = sub.add_parser("image", help="build the update image from a build directory")
    im.add_argument("build", type=Path)
    im.add_argument("-o", "--output", type=Path)
    for name in ("push", "status"):
        p = sub.add_parser(name)
        if name == "push":
            p.add_argument("source", help="a build directory, or an image file")
            p.add_argument("--reboot", action="store_true", help="restart into the new image")
            p.add_argument("--wait", action="store_true", help="then watch it confirm, or fall back")
            p.add_argument("--wait-s", type=int, default=150)
            p.add_argument("--break", dest="broken", action="store_true",
                           help="ruin the kernel's code (testing the fall-back)")
            p.add_argument("--rate", type=float, default=0.0, metavar="KB/S",
                           help="send no faster than this (testing a cut-off update by hand)")
            p.add_argument("--no-board-check", action="store_true",
                           help="send even to another board (testing the node's own refusal)")
        p.add_argument("--p4", metavar="BOARD", help="through a P4 board's console (nano|lcd7b)")
        p.add_argument("--tcp", metavar="HOST[:PORT]")
        p.add_argument("--key", help="9P key, hex")
        p.add_argument("--keyfile", help="file holding the 9P key, hex")
        p.add_argument("--prefix", default="", help="where the node's namespace is, e.g. /c6")
    a = ap.parse_args()

    if a.cmd == "image":
        image, info = make_image(a.build)
        out = a.output or a.build / "lugalos-ota.img"
        out.write_bytes(image)
        print(f"{out}: {len(image)} bytes, {info['board']}, build {info['build_id']}, sha256 {info['sha256']}")
        return 0
    if a.cmd == "status":
        node = Node(a)
        try:
            for k, v in node.status().items():
                print(f"{k}={v}")
        finally:
            node.close()
        return 0
    return push(a)


if __name__ == "__main__":
    sys.exit(main())
