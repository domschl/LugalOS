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


def make_image(build: Path, broken: bool = False, idf: bool = False) -> tuple[bytes, dict]:
    man = read_manifest(build)
    if "board" not in man:
        sys.exit(f"{build}/flash.manifest names no board: rebuild it")
    board, chip = man["board"]
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
    keys = ("board", "layout", "running", "trial", "fell_back", "slot_a", "slot_b",
            "active", "pending", "state", "error")
    return ", ".join(f"{k}={st[k]}" for k in keys if k in st)


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
            image, info = make_image(src, broken=a.broken, idf=is_idf)
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
        while time.monotonic() < end:
            time.sleep(3)
            try:
                st = node.status()
                answered_since_reboot = True
            except Exception as e:
                last_err = str(e) or type(e).__name__
                if a.tcp:
                    # The node's own 9P server restarted with it: a new
                    # connection, and a new session, once it is back.
                    try:
                        node.close()
                    except Exception:
                        pass
                    try:
                        node = Node(a)
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
    for name in ("push", "status", "switch", "dump-other"):
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
            p.add_argument("--sign-key", help=f"PEM private key to sign with (default {DEFAULT_SIGNING_KEY}, if present)")
            p.add_argument("--no-sign", action="store_true", help="send the image unsigned")
            p.add_argument("--tamper", choices=["sig", "header", "part"],
                           help="damage the signed image (testing the node's refusal): a signature byte, "
                                "a signed header byte (CRC fixed up), or a part byte")
        elif name == "switch":
            p.add_argument("--reboot", action="store_true", help="restart into the switched slot")
        elif name == "dump-other":
            p.add_argument("-o", "--output", type=Path, required=True, help="output file path")
            p.add_argument("--bytes", type=int, default=0, help="explicit byte count (0: auto-detect 0xE9 image size)")
        p.add_argument("--p4", metavar="BOARD", help="through a P4 board's console (nano|lcd7b|tab5)")
        p.add_argument("--tcp", metavar="HOST[:PORT]")
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
            for k, v in node.status().items():
                print(f"{k}={v}")
        finally:
            node.close()
        return 0
    if a.cmd == "switch":
        return switch_slot(a)
    if a.cmd == "dump-other":
        return dump_other(a)
    return push(a)


if __name__ == "__main__":
    sys.exit(main())
