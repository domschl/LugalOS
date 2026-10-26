#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial", "cryptography"]
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

## Signing (plan/phase52_remote_admin.md 52.7)

    lota.py keygen                      # -> ~/.config/lugalos/keys/ota-release.pem
    lota.py pubkey                      # the public key, for `otakey set <hex>` on a node

`image` and `push` sign every image with that key when it exists (--sign-key
names another, --no-sign sends it unsigned): ECDSA P-256 over the header's
first 132 bytes, which carry the SHA-256 of the parts. A node with a release
key installed (`otakey`) takes only images signed with it, and checks the
signature before it erases anything.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
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

# The signature fields inside the 132 bytes version 1 reserved
# (kernel/include/kernel/ota_image.h): sig_alg, 3 pad, key_id[8], sig[64].
SIG_ALG_OFF, KEY_ID_OFF, SIG_OFF, SIGNED_LEN = 120, 124, 132, 132
SIG_ALG_P256 = 1
DEFAULT_SIGNING_KEY = Path.home() / ".config" / "lugalos" / "keys" / "ota-release.pem"


def load_signing_key(path: Path):
    """The private key, PEM. `cryptography` is imported here and not at the
    top: the build runs `lota.py app-image` under a plain python3."""
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    key = load_pem_private_key(path.read_bytes(), password=None)
    from cryptography.hazmat.primitives.asymmetric import ec
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit(f"{path}: not a P-256 private key")
    return key


def public_raw(key) -> bytes:
    """64 bytes, X then Y: the form `otakey set` and the node take."""
    n = key.public_key().public_numbers()
    return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")


def key_id(pub: bytes) -> bytes:
    return hashlib.sha256(pub).digest()[:8]


def is_signed(image: bytes) -> bool:
    return image[SIG_ALG_OFF] != 0


def sign_image(image: bytes, key) -> bytes:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    hdr = bytearray(image[:HEADER_LEN])
    hdr[SIG_ALG_OFF:SIG_ALG_OFF + 4] = bytes([SIG_ALG_P256, 0, 0, 0])
    hdr[KEY_ID_OFF:KEY_ID_OFF + 8] = key_id(public_raw(key))
    hdr[SIG_OFF:HEADER_LEN - 4] = bytes(HEADER_LEN - 4 - SIG_OFF)
    r, s_ = decode_dss_signature(key.sign(bytes(hdr[:SIGNED_LEN]), ec.ECDSA(hashes.SHA256())))
    hdr[SIG_OFF:SIG_OFF + 64] = r.to_bytes(32, "big") + s_.to_bytes(32, "big")
    hdr[HEADER_LEN - 4:] = struct.pack("<I", zlib.crc32(bytes(hdr[:HEADER_LEN - 4])))
    return bytes(hdr) + image[HEADER_LEN:]


def maybe_sign(image: bytes, a: argparse.Namespace) -> tuple[bytes, str]:
    """Signs `image` unless --no-sign, or it is signed already, or there is no key."""
    if is_signed(image):
        return image, "signed (as given), key id " + image[KEY_ID_OFF:KEY_ID_OFF + 8].hex()
    if getattr(a, "no_sign", False):
        return image, "unsigned (--no-sign)"
    path = Path(a.sign_key) if getattr(a, "sign_key", None) else DEFAULT_SIGNING_KEY
    if not path.exists():
        if getattr(a, "sign_key", None):
            sys.exit(f"{path}: no such signing key")
        return image, f"unsigned (no signing key at {path}; `lota.py keygen` makes one)"
    key = load_signing_key(path)
    return sign_image(image, key), "signed, key id " + key_id(public_raw(key)).hex()


def keygen(a: argparse.Namespace) -> int:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    out = Path(a.output) if a.output else DEFAULT_SIGNING_KEY
    if out.exists() and not a.force:
        sys.exit(f"{out} exists; --force replaces it (and every node that trusts it then refuses your images)")
    out.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    pub = public_raw(key)
    print(f"{out}: new P-256 release key, key id {key_id(pub).hex()}")
    print(f"on each node's console:  otakey set {pub.hex()}")
    return 0


def pubkey(a: argparse.Namespace) -> int:
    path = Path(a.sign_key) if a.sign_key else DEFAULT_SIGNING_KEY
    if not path.exists():
        sys.exit(f"{path}: no signing key (`lota.py keygen` makes one)")
    pub = public_raw(load_signing_key(path))
    print(f"key id {key_id(pub).hex()}")
    print(f"otakey set {pub.hex()}")
    return 0


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


def header(board: str, chip: str, bid: str, parts: list[bytes], rev: tuple[int, int] = (0, 0xFFFF)) -> bytes:
    sha = hashlib.sha256(b"".join(parts)).digest()
    body = struct.pack(HEADER_FMT, MAGIC, VERSION, HEADER_LEN, board.encode(), chip.encode(),
                       rev[0], rev[1], len(parts[0]), len(parts[1]), bid.encode()[:31], sha, bytes(132), 0)
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


def p4_rev_range(man: dict[str, list[str]]) -> tuple[int, int]:
    """The silicon revisions a P4 build runs on: v1.x (below 300) or v3.x and later."""
    rev = int(man["rev"][0])
    return (0, 299) if rev < 300 else (300, 0xFFFF)


def parts_esp32p4(build: Path) -> list[bytes]:
    """Part 0: the RAM half in the boot ROM's image format; part 1: the XIP half."""
    import p4flash
    ram_img = build / "lugalos-ram.img"
    cmd = p4flash.esptool_cmd() + ["elf2image", "-o", str(ram_img), str(build / "lugalos-ram.elf")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-600:])
    return [ram_img.read_bytes(), (build / "lugalos-xip.bin").read_bytes()]


def corrupt_esp_app_image(raw: bytes) -> bytes:
    """Zero 64 KB of .text (0x42080000) in an ESP-IDF 0xE9 image and recompute its checksum and SHA-256."""
    data = bytearray(raw)
    if len(data) < 64 or data[0] != 0xE9:
        sys.exit("not an ESP-IDF 0xE9 image")
    nseg = data[1]
    hash_appended = data[23]
    off = 24
    chk = 0xEF
    corrupted = False
    for _ in range(nseg):
        load_addr, seg_len = struct.unpack_from("<II", data, off)
        off += 8
        if load_addr == 0x42080000:
            n = min(0x10000, seg_len)
            data[off:off + n] = bytes(n)
            corrupted = True
        for b in data[off:off + seg_len]:
            chk ^= b
        off += seg_len
    if not corrupted:
        sys.exit("no 0x42080000 (.text) segment found in app image to corrupt")
    chk_off = off | 15
    data[chk_off] = chk
    if hash_appended == 1:
        data[-32:] = hashlib.sha256(data[:-32]).digest()
    return bytes(data)


def make_app_image(build: Path, broken: bool = False) -> tuple[bytes, dict]:
    """Build a single-file ESP-IDF 0xE9 application image (lugalos-c6-app.bin) with esp_app_desc_t at 0x20."""
    import c6run
    man = read_manifest(build)
    if "board" not in man:
        sys.exit(f"{build}/flash.manifest names no board: rebuild it")
    board, chip = man["board"]
    flash_size = int(man["flash"][1], 0)
    app_elf = build / "lugalos-app.elf"
    if not app_elf.exists():
        sys.exit(f"{app_elf} does not exist: rebuild {build}")
    app_bin = build / "lugalos-c6-app.bin"
    r = c6run.esptool("elf2image", "--flash-mode", "dio", "--flash-freq", "40m",
                      "--flash-size", "%dMB" % (flash_size >> 20), "-o", str(app_bin), str(app_elf))
    if r.returncode != 0:
        sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-600:])
    raw = app_bin.read_bytes()
    if len(raw) < 0x120 or raw[0] != 0xE9:
        sys.exit("lugalos-c6-app.bin is not a valid 0xE9 image")
    app_magic = struct.unpack_from("<I", raw, 0x20)[0]
    if app_magic != 0xABCD5432:
        sys.exit(f"esp_app_desc_t magic at 0x20 is 0x{app_magic:08x}, expected 0xABCD5432")
    if broken:
        raw = corrupt_esp_app_image(raw)
    bid = build_id(build)
    info = {"board": board, "chip": chip, "build_id": bid,
            "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    return raw, info


def patch_tbyb(binary: bytes) -> bytes:
    """Flag the image try-before-you-buy (datasheet 5.9.3.1, EXE TBYB): the
    bootrom then boots it only in a flash update boot, until it buys. The
    IMAGE_DEF item sits in the block loop within the first 4 KB."""
    data = bytearray(binary)
    words = struct.unpack_from("<%dI" % (min(len(data), 4096) // 4), data)
    for i, w in enumerate(words):
        if w != 0xFFFFDED3:
            continue
        j = i + 1
        while j < len(words):
            item = words[j]
            if item & 0x7F == 0x42:
                flags = struct.unpack_from("<H", data, j * 4 + 2)[0]
                struct.pack_into("<H", data, j * 4 + 2, flags | 0x8000)
                return bytes(data)
            size = (item >> 8) & 0xFFFF if item & 0x80 else (item >> 8) & 0xFF
            if item & 0xFF == 0xFF or size == 0:
                break
            j += size
    sys.exit("no IMAGE_DEF found in the first 4 KB of the image")


def patch_version(binary: bytes, floor: tuple[int, int]) -> bytes:
    """Make the image's VERSION item (major, minor) strictly higher than `floor`.

    The bootrom's explicit_buy erases the other partition unless the bought
    image is a higher version than it (datasheet 5.4.8.4), and a booted pair
    is ordered by version: an update that is not strictly newer than the
    running image destroys the fallback it was supposed to keep."""
    data = bytearray(binary)
    words = struct.unpack_from("<%dI" % (min(len(data), 4096) // 4), data)
    for i, w in enumerate(words):
        if w != 0xFFFFDED3:
            continue
        j = i + 1
        while j < len(words):
            item = words[j]
            if item & 0x7F == 0x48:
                minor, major = struct.unpack_from("<HH", data, j * 4 + 4)
                if (major, minor) <= floor:
                    major, minor = floor[0], floor[1] + 1
                    if minor > 0xFFFF:
                        major, minor = major + 1, 0
                    struct.pack_into("<HH", data, j * 4 + 4, minor, major)
                return bytes(data)
            size = (item >> 8) & 0xFFFF if item & 0x80 else (item >> 8) & 0xFF
            if item & 0xFF == 0xFF or size == 0:
                break
            j += size
    sys.exit("no VERSION item in the image's first block")


def make_image(build: Path, broken: bool = False, idf: bool = False,
               running_version: tuple[int, int] | None = None) -> tuple[bytes, dict]:
    man = read_manifest(build)
    if "board" not in man:
        sys.exit(f"{build}/flash.manifest names no board: rebuild it")
    board, chip = man["board"]
    if chip == "rp2350":
        raw = (build / "lugalos.bin").read_bytes()
        if running_version is not None:
            raw = patch_version(raw, running_version)
        parts = [patch_tbyb(raw), b""]
        if broken:
            # The IMAGE_DEF, the entry code and the first bytes stay: it passes
            # the bootrom, calls explicit_buy, then dies in code that is zeroes.
            img = bytearray(parts[0])
            img[0x40000:0x50000] = bytes(0x10000)
            parts[0] = bytes(img)
        hdr = header(board, chip, build_id(build), parts)
        info = {"board": board, "chip": chip, "build_id": build_id(build),
                "part_len": [len(p) for p in parts], "sha256": hashlib.sha256(b"".join(parts)).hexdigest(),
                "layout": "partition"}
        return hdr + b"".join(parts), info
    if chip == "esp32p4":
        parts = parts_esp32p4(build)
        if broken:
            # Zeroes are illegal instructions: the image passes every check, boots, and dies.
            xip = bytearray(parts[1])
            xip[0x10000:0x20000] = bytes(0x10000)
            parts[1] = bytes(xip)
        hdr = header(board, chip, build_id(build), parts, p4_rev_range(man))
        info = {"board": board, "chip": chip, "build_id": build_id(build),
                "part_len": [len(p) for p in parts], "sha256": hashlib.sha256(b"".join(parts)).hexdigest(),
                "layout": "loader"}
        return hdr + b"".join(parts), info
    if chip != "esp32c6":
        sys.exit(f"no update image format for {chip} yet")
    if idf:
        app_raw, app_info = make_app_image(build, broken=broken)
        parts = [app_raw, b""]
        hdr = header(board, chip, app_info["build_id"], parts)
        info = {"board": board, "chip": chip, "build_id": app_info["build_id"],
                "part_len": [len(app_raw), 0], "sha256": app_info["sha256"], "layout": "idf"}
        return hdr + app_raw, info
    parts = parts_esp32c6(build, int(man["flash"][1], 0))
    if broken:
        # The kernel's own text starts 512 KB into the XIP half (linker/esp32c6.ld:
        # the radio's block comes first). Zeroes are illegal instructions.
        xip = bytearray(parts[1])
        xip[0x80000:0x90000] = bytes(0x10000)
        parts[1] = bytes(xip)
    hdr = header(board, chip, build_id(build), parts)
    info = {"board": board, "chip": chip, "build_id": build_id(build),
            "part_len": [len(p) for p in parts], "sha256": hashlib.sha256(b"".join(parts)).hexdigest(),
            "layout": "loader"}
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
            console = p4run.open_console(self.port)
            try:
                self.client = p9lib.connect_open_serial(console, framing="slip",
                                                        timeout=30.0, closer=p4run.close_console)
            except Exception:
                # A console left open would be fought over by the next attempt, and opening it
                # is what resets the board: the retry loops of push-board depend on this.
                p4run.close_console(console)
                raise
        elif a.tcp:
            host, _, port = a.tcp.partition(":")
            self.client = p9lib.connect_tcp(host, int(port or 564), timeout=30.0)
        elif a.serial:
            self.client = p9lib.connect_serial(a.serial, baudrate=115200, timeout=30.0)
        else:
            sys.exit("say how to reach the node: --p4 BOARD, --tcp HOST[:PORT] or --serial PORT")
        self.sess = p9lib.Session(self.client, aname="/", key=key)

    def close(self) -> None:
        try:
            self.client.close()
        finally:
            if self.p4:
                import p9sync
                time.sleep(0.3)
                p9sync.p4_shell(self.port, "p9share off")

    def status(self, prefix: str | None = None) -> dict[str, str]:
        text = self.sess.read((self.prefix if prefix is None else prefix) + "/dev/ota").decode(errors="replace")
        return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)

    def read_kv(self, path: str) -> dict[str, str]:
        """A node file of key=value lines (/proc/llp), or {} if it cannot be read."""
        try:
            text = self.sess.read(path).decode(errors="replace")
        except Exception:
            return {}
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

    def read_other_slot(self, max_bytes: int = 0) -> bytes:
        """Read the inactive slot from <prefix>/dev/otaimage. If the slot starts with an ESP-IDF 0xE9
        header, parse its segment headers on the fly so only the exact image bytes are transferred."""
        from p9lib.client import OREAD
        c = self.client
        fid = self.sess._alloc_work_fid()
        self.sess._walk_or_raise(fid, self.sess._split(self.prefix + "/dev/otaimage"), "otaimage", "walk to")
        try:
            c.open(fid, mode=OREAD)
            chunk = min(c.io_chunk(), 2048)
            first = c.read(fid, 0, min(chunk, 4096))
            if not first:
                return b""
            target_len = max_bytes
            if target_len == 0 and first[0] == 0xE9 and len(first) >= 24:
                nseg = first[1]
                hash_appended = first[23]
                pos = 24
                for _ in range(nseg):
                    hdr_bytes = c.read(fid, pos, 8)
                    if len(hdr_bytes) < 8:
                        break
                    _, seg_len = struct.unpack("<II", hdr_bytes)
                    pos += 8 + seg_len
                target_len = ((pos | 15) + 1) + (32 if hash_appended == 1 else 0)
                print(f"detected ESP-IDF 0xE9 image in inactive slot: {nseg} segments, {target_len} bytes")
            if target_len == 0:
                target_len = 0x1C0000
            out = bytearray()
            t0 = time.monotonic()
            while len(out) < target_len:
                want = min(chunk, target_len - len(out))
                try:
                    b = c.read(fid, len(out), want)
                except Exception as e:
                    print(f"\n[read error] at offset {len(out)}/{target_len} (chunk={want}): {e}")
                    raise
                if not b:
                    break
                out.extend(b)
                if (len(out) // chunk) % 32 == 0 or len(out) == target_len:
                    rate = len(out) / 1024 / max(time.monotonic() - t0, 1e-3)
                    print(f"\r  {len(out)}/{target_len} bytes, {rate:.0f} KB/s", end="", flush=True)
                time.sleep(0.002)
            print()
            return bytes(out)
        finally:
            c.clunk(fid)


def show(st: dict[str, str]) -> str:
    keys = ("board", "layout", "version", "running", "trial", "fell_back", "slot_a", "slot_b",
            "active", "pending", "state", "error")
    return ", ".join(f"{k}={st[k]}" for k in keys if k in st)


def running_build(st: dict[str, str]) -> str:
    return st.get(f"slot_{st['running']}", "?") if st.get("running") in ("a", "b") else "?"


def radio_state(node: Node) -> tuple[dict[str, str], dict[str, str]] | None:
    """The C6 beside a P4, through its /c6 (status, link), or None if there is none."""
    try:
        radio = node.status("/c6")
    except Exception:
        return None
    if "board" not in radio:
        return None
    return radio, {"p4_speaks": node.read_kv("/c6/proc/llp").get("peer_version", "?"),
                   "c6_speaks": node.read_kv("/proc/llp").get("peer_version", "?")}


def report_both(node: Node) -> bool:
    """Prints a P4 and its C6 side by side; True if the pair is in step (same LLP version, same build)."""
    own = node.status("")
    print("this node:", show(own))
    pair = radio_state(node)
    if pair is None:
        return True
    radio, link = pair
    print("the radio: ", show(radio))
    same_llp = link["p4_speaks"] == link["c6_speaks"] != "?"
    same_build = running_build(own) == running_build(radio)
    print(f"link:       LLP {link['p4_speaks']} on the P4, {link['c6_speaks']} on the C6: "
          f"{'in step' if same_llp else 'NOT in step'}")
    print(f"builds:     P4 {running_build(own)}, C6 {running_build(radio)}: "
          f"{'the same' if same_build else 'different'}")
    return same_llp and same_build


def push_board(a: argparse.Namespace) -> int:
    """The whole board: the C6 first, until it has confirmed, then the P4, then the pair is checked.

    The order is the point. The C6 runs the link the P4's update arrives over, and a P4 that
    came up on a new LLP against an old C6 would be unreachable; so the radio goes first and
    the P4 is not touched unless it confirmed. If the P4 then fails, the board is left with a
    new C6 and the old P4 -- reported, not hidden -- and `switch --prefix /c6 --reboot` undoes it.
    """
    for k, v in (("reboot", True), ("wait", True), ("broken", False)):
        if not hasattr(a, k): setattr(a, k, v)
    p4_build = Path(a.source)
    c6_build = Path(a.c6_build) if a.c6_build else p4_build.parent / "esp32c6-mini"
    if not (c6_build / "flash.manifest").exists():
        sys.exit(f"no C6 build at {c6_build}: build esp32c6-mini, or say where with --c6-build")
    node = Node(a)
    try:
        before = node.status("")
        p4_was_running = before.get("running")
        c6_was_running = (radio_state(node) or ({}, {}))[0].get("running")
        if before.get("chip") != "esp32p4" or radio_state(node) is None:
            sys.exit("this is not a P4 with a C6 mounted at /c6: use `push` for a single node")
        print("== before"); report_both(node)
    finally:
        node.close()

    def part(prefix: str, build: Path, wait: bool, broken: bool) -> argparse.Namespace:
        ns = argparse.Namespace(**vars(a))
        ns.prefix, ns.source, ns.reboot, ns.wait, ns.broken = prefix, str(build), True, wait, broken
        return ns

    print("\n== the C6 first")
    rc = push(part("/c6", c6_build, True, a.break_part == "c6"))
    if rc != 0:
        print("\nthe C6 did not take the update, so the P4 is untouched")
        return rc

    print("\n== then the P4")
    over_net = bool(a.tcp)
    rc = push(part("", p4_build, over_net, a.break_part == "p4"))
    if rc == 2:
        print("\nthe P4 fell back to its old image; the C6 runs the new one (`lota.py switch --prefix /c6 --reboot` undoes that)")
    if not over_net:
        # The console is the P4's own reset line: opening it ends a trial that has not confirmed,
        # so look only after the trial has had its time to settle.
        settle = 50
        print(f"waiting {settle} s for the P4's trial before looking at it")
        time.sleep(settle)

    print("\n== after")
    ok = False
    for attempt in range(10):
        try:
            node = Node(a)
            try:
                ok = report_both(node)
                st = node.status("")
                rst = node.status("/c6")
                confirmed = st.get("trial") == "no" and rst.get("trial") == "no"
                # Each part must be running the slot just written: a fall-back also ends with
                # no trial, and with builds that look alike.
                p4_new = st.get("running") != p4_was_running and st.get("fell_back") != "yes"
                c6_new = rst.get("running") != c6_was_running and rst.get("fell_back") != "yes"
            finally:
                node.close()
            break
        except Exception as e:
            print(f"  (not reachable yet: {e})")
            time.sleep(6)
    else:
        print("the board does not answer; check its console: ota")
        return 1
    if not confirmed:
        print("one of the two is still on trial")
        return 1
    if not (p4_new and c6_new):
        mixed = ", ".join(n for n, new in (("the P4", p4_new), ("the C6", c6_new)) if not new)
        print(f"\n{mixed} did not take the update and runs the old image; the other part is new."
              "\nThe pair may still be in step; `lota.py status` shows the slots.")
        return 2
    print("\nthe whole board is updated and in step" if ok else "\nthe board is up but the pair is NOT in step")
    return 0 if ok else 2


def push(a: argparse.Namespace) -> int:
    src = Path(a.source)
    node = Node(a)
    try:
        st = node.status()
        print("node:", show(st))
        if st.get("supported") == "no":
            sys.exit("the node cannot update itself")
        is_idf = st.get("layout") == "idf"
        if src.is_dir():
            rv = None
            if "version" in st:
                major, _, minor = st["version"].partition(".")
                rv = (int(major), int(minor))
            image, info = make_image(src, broken=a.broken, idf=is_idf, running_version=rv)
        else:
            raw = src.read_bytes()
            if len(raw) >= HEADER_LEN and struct.unpack_from("<I", raw, 0)[0] == MAGIC:
                image = raw
                info = {"board": image[8:32].rstrip(b"\0").decode(),
                        "build_id": image[56:88].rstrip(b"\0").decode()}
            elif raw and raw[0] == 0xE9 and is_idf:
                if a.broken:
                    raw = corrupt_esp_app_image(raw)
                bid = "esp-idf-app"
                if len(raw) >= 0x120 and struct.unpack_from("<I", raw, 0x20)[0] == 0xABCD5432:
                    ver = raw[0x30:0x50].split(b"\0", 1)[0].decode(errors="replace")
                    proj = raw[0x50:0x70].split(b"\0", 1)[0].decode(errors="replace")
                    bid = ver if proj == "LugalOS" else f"{proj}:{ver}"
                target_board = st.get("board", "esp32c6-mini")
                hdr = header(target_board, st.get("chip", "esp32c6"), bid, [raw, b""])
                image = hdr + raw
                info = {"board": target_board, "build_id": bid}
            else:
                sys.exit(f"{src}: not a LugalOS OTA image or ESP-IDF 0xE9 app binary")
        image, sig_note = maybe_sign(image, a)
        if a.tamper:
            b = bytearray(image)
            at = {"sig": SIG_OFF + 5, "header": 60, "part": HEADER_LEN + len(image) // 3}[a.tamper]
            b[at] ^= 0x01
            if a.tamper != "part":
                b[HEADER_LEN - 4:HEADER_LEN] = struct.pack("<I", zlib.crc32(bytes(b[:HEADER_LEN - 4])))
            image = bytes(b)
            sig_note += f", TAMPERED ({a.tamper} byte {at})"
        print(f"image: {len(image)} bytes for {info['board']}, build {info.get('build_id', '?')}, {sig_note}"
              f"{' (BROKEN on purpose)' if a.broken else ''}")
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
        last_err = ""
        answered_since_reboot = False
        failures = 0
        while time.monotonic() < end:
            time.sleep(3)
            try:
                st = node.status()
                answered_since_reboot = True
                failures = 0
            except Exception as e:
                last_err = str(e) or type(e).__name__
                failures += 1
                # Over the P4's console the session can end with the restart of the C6 behind it
                # (the Tab5's does); reopening the console resets some boards, so only after it
                # has stayed down for a while.
                if a.tcp or (a.p4 and failures >= 3):
                    # The node's own 9P server restarted with it: a new
                    # connection, and a new session, once it is back.
                    try:
                        node.close()
                    except Exception:
                        pass
                    try:
                        node = Node(a)
                        failures = 0
                    except Exception as e2:
                        last_err = str(e2) or type(e2).__name__
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
        if not answered_since_reboot:
            print(f"no answer since the reboot ({last_err or 'no reply'}): the node may be fine but "
                  "unreachable to this session -- a key set with `p9key` lasts one boot only. "
                  "Check its console: cat /dev/ota")
        else:
            print("no answer in time; last seen:", show(st))
        return 1
    finally:
        node.close()


def switch_slot(a: argparse.Namespace) -> int:
    node = Node(a)
    try:
        st = node.status()
        print("node:", show(st))
        node.ctl("switch")
        st = node.status()
        print("switched:", show(st))
        if a.reboot:
            node.ctl("reboot")
            print("rebooting into the other slot")
        return 0
    finally:
        node.close()


def dump_other(a: argparse.Namespace) -> int:
    node = Node(a)
    try:
        st = node.status()
        print("node:", show(st))
        data = node.read_other_slot(a.bytes)
        if not data:
            sys.exit("read 0 bytes from inactive slot")
        if data[0] == 0xE9 and len(data) >= 64 and data[23] == 1:
            expected_sha = data[-32:]
            actual_sha = hashlib.sha256(data[:-32]).digest()
            if expected_sha != actual_sha:
                sys.exit(f"ESP-IDF image SHA-256 mismatch: expected {expected_sha.hex()}, got {actual_sha.hex()}")
            print(f"verified ESP-IDF image SHA-256: {actual_sha.hex()}")
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_bytes(data)
        print(f"wrote {len(data)} bytes to {a.output}")
        return 0
    finally:
        node.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    im = sub.add_parser("image", help="build the update image from a build directory")
    im.add_argument("build", type=Path)
    im.add_argument("-o", "--output", type=Path)
    im.add_argument("--idf", action="store_true", help="pack single-file ESP-IDF 0xE9 app image instead of loader split")
    for p in (im,):
        p.add_argument("--sign-key", help=f"PEM private key to sign with (default {DEFAULT_SIGNING_KEY}, if present)")
        p.add_argument("--no-sign", action="store_true", help="leave the image unsigned")
    kg = sub.add_parser("keygen", help="make a P-256 release key for signing update images")
    kg.add_argument("-o", "--output", help=f"where to write it (default {DEFAULT_SIGNING_KEY})")
    kg.add_argument("--force", action="store_true", help="replace an existing key")
    pk = sub.add_parser("pubkey", help="print the release key's public half, for `otakey set`")
    pk.add_argument("--sign-key", help=f"PEM private key (default {DEFAULT_SIGNING_KEY})")
    aim = sub.add_parser("app-image", help="build the single-file ESP-IDF 0xE9 application image (lugalos-c6-app.bin)")
    aim.add_argument("build", type=Path)
    aim.add_argument("-o", "--output", type=Path)
    for name in ("push", "push-board", "status", "switch", "dump-other"):
        p = sub.add_parser(name)
        if name in ("push", "push-board"):
            p.add_argument("source", help="a build directory, or an image file")
            if name == "push":
                p.add_argument("--reboot", action="store_true", help="restart into the new image")
                p.add_argument("--wait", action="store_true", help="then watch it confirm, or fall back")
            p.add_argument("--wait-s", type=int, default=150)
            if name == "push":
                p.add_argument("--break", dest="broken", action="store_true",
                               help="ruin the kernel's code (testing the fall-back)")
            else:
                p.add_argument("--c6-build", help="the C6's build directory (default: esp32c6-mini beside the P4's)")
                p.add_argument("--break", dest="break_part", choices=["c6", "p4"],
                               help="ruin that part's code (testing the order and the fall-back)")
            p.add_argument("--rate", type=float, default=0.0, metavar="KB/S",
                           help="send no faster than this (testing a cut-off update by hand)")
            p.add_argument("--no-board-check", action="store_true",
                           help="send even to another board (testing the node's own refusal)")
            p.add_argument("--sign-key", help=f"PEM private key to sign with (default {DEFAULT_SIGNING_KEY}, if present)")
            p.add_argument("--no-sign", action="store_true", help="send the image unsigned")
            p.add_argument("--tamper", choices=["sig", "header", "part"],
                           help="damage the signed image (testing the node's refusal): a signature byte, "
                                "a signed header byte (CRC fixed up), or a part byte")
        elif name == "status":
            p.add_argument("--single", action="store_true", help="only the node itself, not the C6 beside it")
        elif name == "switch":
            p.add_argument("--reboot", action="store_true", help="restart into the switched slot")
        elif name == "dump-other":
            p.add_argument("-o", "--output", type=Path, required=True, help="output file path")
            p.add_argument("--bytes", type=int, default=0, help="explicit byte count (0: auto-detect 0xE9 image size)")
        p.add_argument("--p4", metavar="BOARD", help="through a P4 board's console (nano|lcd7b|tab5)")
        p.add_argument("--tcp", metavar="HOST[:PORT]")
        p.add_argument("--serial", metavar="PORT", help="the 9P port of an RP2350 (its USB interface 02)")
        p.add_argument("--key", help="9P key, hex")
        p.add_argument("--keyfile", help="file holding the 9P key, hex")
        p.add_argument("--prefix", default="", help="where the node's namespace is, e.g. /c6")
    a = ap.parse_args()

    if a.cmd == "app-image":
        raw, info = make_app_image(a.build)
        out = a.output or a.build / "lugalos-c6-app.bin"
        out.write_bytes(raw)
        print(f"{out}: {len(raw)} bytes, {info['board']}, build {info['build_id']}, sha256 {info['sha256']}")
        return 0
    if a.cmd == "keygen":
        return keygen(a)
    if a.cmd == "pubkey":
        return pubkey(a)
    if a.cmd == "image":
        image, info = make_image(a.build, idf=a.idf)
        image, sig_note = maybe_sign(image, a)
        info["layout"] = f"{info['layout']}, {sig_note}"
        out = a.output or a.build / "lugalos-ota.img"
        out.write_bytes(image)
        print(f"{out}: {len(image)} bytes, {info['board']} ({info['layout']}), build {info['build_id']}, sha256 {info['sha256']}")
        return 0
    if a.cmd == "status":
        node = Node(a)
        try:
            if not a.prefix and not a.single and radio_state(node) is not None:
                report_both(node)
                return 0
            for k, v in node.status().items():
                print(f"{k}={v}")
        finally:
            node.close()
        return 0
    if a.cmd == "push-board":
        return push_board(a)
    if a.cmd == "switch":
        return switch_slot(a)
    if a.cmd == "dump-other":
        return dump_other(a)
    return push(a)


if __name__ == "__main__":
    sys.exit(main())
