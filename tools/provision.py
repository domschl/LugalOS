#!/usr/bin/env python3
"""tools/provision.py -- I2/I6, plan/phase21_identity_and_authentication.md.

Builds an identity-store image: the same record kernel/idstore.c reads and
writes (magic, version, length, CRC32, then typed fields), on the host,
without a board. QEMU's second virtio-blk device (drivers/virtio_blk_id.c)
is the target this writes for; I7's RP2350 flash-sector backend reads the
identical format, so this tool does not change when that lands.

The format is deliberately re-implemented here rather than shared with the
C source: this is the one artifact meant to be inspected and modified with
nothing but a hex editor and this script if the kernel is ever unavailable,
and a format worth trusting for that should be simple enough to write twice.

Usage:
    provision.py out.img --name clock-3f2a [--uid 0011223344556677]
    provision.py out.img --name clock-3f2a --wlan-ssid homenet --wlan-passphrase "correct horse"

I6, §5.3: the WLAN passphrase, if given, never reaches the board -- only
the *derived* 256-bit PSK does (derive_wpa2_psk() below). That derivation
runs here, once, on the host: 4096 iterations of HMAC is a poor use of an
RP2350, and a board holding only a derived, single-SSID key cannot leak a
passphrase someone reused somewhere else.

Bootstrap note (§6): this writes a file on the host. Getting the resulting
image onto a board is a separate, explicit step (attaching it as QEMU's
second virtio-blk drive, or -- once I7 lands -- flashing it to the reserved
sector) precisely because provisioning over a running node's own network
must stay refused, not merely discouraged.
"""

import argparse
import hashlib
import os
import re
import subprocess
import tempfile
import secrets
from pathlib import Path
import struct
import sys

IDSTORE_SIZE_BYTES = 4096
IDSTORE_HEADER_LEN = 12
MAGIC = b"LGID"
VERSION = 1

FIELD_UID = 1
FIELD_NAME = 2
FIELD_DEVKEY = 3    # the node's own auth key (kernel/idstore.h's IDSTORE_FIELD_DEVKEY)
FIELD_WLAN_SSID = 4
FIELD_WLAN_PSK = 5
FIELD_IPV4 = 6      # 12 bytes: ip[4] mask[4] gw[4], dotted-quad order
FIELD_GRANTS = 7    # the peer grants list, as the text fs/9p.c parses

WLAN_PSK_LEN = 32  # WPA2's PSK is always 256 bits -- kernel/identity.h's NODE_WLAN_PSK_LEN


def crc32_compute(data: bytes) -> int:
    """The exact bitwise construction kernel/idstore.c's crc32_compute() uses
    (IEEE 802.3 polynomial, init/final 0xFFFFFFFF) -- not zlib.crc32(), so
    this file has no import whose behaviour could drift from the kernel's
    without either side's tests noticing."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            mask = (-(crc & 1)) & 0xFFFFFFFF
            crc = (crc >> 1) ^ (0xEDB88320 & mask)
    return crc ^ 0xFFFFFFFF


def derive_wpa2_psk(ssid: str, passphrase: str) -> bytes:
    """PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, dklen=32) -- the exact
    construction WPA2 itself defines (IEEE 802.11i) for turning a
    passphrase into the PSK an AP and a station actually authenticate
    with. Uses Python's stdlib hashlib, not a hand-rolled PBKDF2/HMAC-SHA1
    -- there is no on-device equivalent to keep in step with (§5.3: this
    runs on the host, never on the board), so there is nothing here worth
    re-implementing by hand the way kernel/idstore.c's CRC32 is.

    The two length limits are WPA2's own, not this tool's invention: a
    passphrase outside 8-63 ASCII characters and an SSID outside 1-32
    bytes are both meaningless to any real AP."""
    if not (8 <= len(passphrase) <= 63):
        raise ValueError("a WPA2 passphrase must be 8-63 ASCII characters")
    if not (1 <= len(ssid.encode("utf-8")) <= 32):
        raise ValueError("an SSID must be 1-32 bytes")
    return hashlib.pbkdf2_hmac("sha1", passphrase.encode("utf-8"), ssid.encode("utf-8"), 4096, dklen=WLAN_PSK_LEN)


def _wpa2_selftest() -> int:
    """I6's own verify point: 'on the host, a known passphrase and SSID
    derive the PSK the standard gives.' SSID='IEEE', passphrase='password'
    is IEEE 802.11i's own worked example, reproduced in every independent
    WPA2-PSK calculator and test suite that cites one -- an external
    reference this tool's derive_wpa2_psk() is checked against, not a
    value invented alongside it."""
    failures = 0
    expected = "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"
    got = derive_wpa2_psk("IEEE", "password").hex()
    ok = got == expected
    print(f"  [{'ok' if ok else 'FAIL'}] SSID='IEEE' passphrase='password' -> {got}")
    if not ok:
        print(f"        expected                                                {expected}")
        failures += 1

    # Property checks, not further known-answer vectors: different inputs
    # must give different outputs, and the same inputs must be deterministic.
    a = derive_wpa2_psk("network-a", "passphrase1")
    b = derive_wpa2_psk("network-b", "passphrase1")
    c = derive_wpa2_psk("network-a", "passphrase2")
    d = derive_wpa2_psk("network-a", "passphrase1")
    ok = a != b and a != c and a == d
    print(f"  [{'ok' if ok else 'FAIL'}] different ssid/passphrase differ; same inputs are deterministic")
    if not ok:
        failures += 1

    print("WPA2_PSK_SELFTEST_OK" if failures == 0 else f"WPA2_PSK_SELFTEST_FAIL ({failures} failed)")
    return failures


def build_record(fields: list[tuple[int, bytes]]) -> bytes:
    field_bytes = bytearray()
    for ftype, val in fields:
        if len(val) > 0xFFFF:
            raise ValueError(f"field {ftype}: {len(val)} bytes exceeds the 16-bit length")
        field_bytes += struct.pack("<BH", ftype, len(val)) + val

    length = IDSTORE_HEADER_LEN + len(field_bytes)
    if length > IDSTORE_SIZE_BYTES:
        raise ValueError(f"record is {length} bytes; the store holds {IDSTORE_SIZE_BYTES}")

    header_wo_crc = MAGIC + struct.pack("<BBH", VERSION, 0, length)
    crc = crc32_compute(header_wo_crc + bytes(field_bytes))
    record = header_wo_crc + struct.pack("<I", crc) + bytes(field_bytes)
    return record.ljust(IDSTORE_SIZE_BYTES, b"\x00")


# --- The identity segment as a flashable image (§3.3's third segment) --------

def identity_flash_base(repo_root: Path) -> int:
    """The address of the identity sector, read from cmake/flash_layout.cmake.

    Parsed rather than repeated. That file is the single place the three
    segment addresses are written down -- the whole point of I7a's layout is
    that nothing else knows them -- and a Python copy of 0x103FF000 would be
    a second place to forget when the map changes."""
    layout = repo_root / "cmake" / "flash_layout.cmake"
    m = re.search(r"set\(\s*LUGALOS_IDENTITY_BASE\s+(0x[0-9A-Fa-f]+)",
                  layout.read_text())
    if not m:
        sys.exit(f"could not find LUGALOS_IDENTITY_BASE in {layout}")
    return int(m.group(1), 16)


def write_uf2(record: bytes, out_path: str, repo_root: Path) -> str:
    """Wraps the record as a UF2 for the identity sector, using the same
    converter and family the build already uses for flashfs.uf2 -- a data
    blob at a fixed flash address is a problem this tree has solved once
    already, and solving it a second way would be a second thing to be wrong."""
    base = identity_flash_base(repo_root)
    conv = repo_root / "tools" / "uf2conv.py"
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
        tf.write(record)
        tmp = tf.name
    try:
        r = subprocess.run(
            [sys.executable, str(conv), "--family", "rp2350_riscv",
             "--base", hex(base), tmp, "-o", out_path],
            capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"uf2conv failed: {r.stderr.strip() or r.stdout.strip()}")
    finally:
        os.unlink(tmp)
    return f"{out_path} (identity sector at {hex(base)})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", nargs="?", help="path to write the identity image to")
    ap.add_argument("--name", help="the instance name (§2: instance scope, freely rewritable)")
    ap.add_argument("--uid", help="16 hex chars (8 bytes); a fresh random UID is minted if omitted")
    ap.add_argument("--wlan-ssid", help="the network's SSID (I6, §5.3)")
    ap.add_argument("--wlan-passphrase", help="the network's WPA2 passphrase -- derived into a PSK here, "
                                              "never written to the image or sent to the board")
    ap.add_argument("--ipv4", metavar="IP/MASK[/GW]",
                    help="the address this board comes up on, e.g. 192.168.1.50/255.255.255.0/192.168.1.1. "
                         "Stored in the record rather than in a boot script, so the filesystem image stays "
                         "identical on every board; applied when the network stack starts")
    ap.add_argument("--key", metavar="HEX",
                    help="this node's device key, as hex (I4, §2). 16-64 bytes. "
                         "Never printed back -- only its fingerprint is.")
    ap.add_argument("--key-generate", action="store_true",
                    help="mint a fresh 32-byte device key from the host's CSPRNG")
    ap.add_argument("--key-out", metavar="FILE",
                    help="also write the key, as hex, where a 9P client can read it "
                         "(chmod 600). Without this a generated key exists only on the "
                         "board and nothing can ever authenticate to it.")
    ap.add_argument("--grant", action="append", metavar="NAME:KEYHEX[:ANAME[:ro]]",
                    help="authorise a peer to attach here, repeatable. NAME may be "
                         "'*' for any peer. ANAME defaults to '/' and the mode to "
                         "rw. This is the INBOUND direction -- who may attach to "
                         "this board -- and is not the same as --key, which is what "
                         "this board presents when it dials out.")
    ap.add_argument("--uf2", metavar="FILE",
                    help="also write the record as a UF2 for the RP2350's identity "
                         "sector, so a board with no console can be provisioned by "
                         "flashing a third image")
    ap.add_argument("--selftest", action="store_true",
                    help="check derive_wpa2_psk() against the IEEE 802.11i worked example and exit")
    args = ap.parse_args()

    if args.selftest:
        return 1 if _wpa2_selftest() else 0

    if not args.output or not args.name:
        ap.error("output and --name are required (unless --selftest)")

    if args.uid:
        try:
            uid = bytes.fromhex(args.uid)
        except ValueError:
            sys.exit("--uid must be hex")
        if len(uid) != 8:
            sys.exit("--uid must be exactly 16 hex characters (8 bytes)")
    else:
        uid = secrets.token_bytes(8)

    name_bytes = args.name.encode("ascii", errors="strict")
    if not (1 <= len(name_bytes) <= 31):
        sys.exit("--name must be 1-31 ASCII characters (kernel/identity.h's NODE_NAME_MAX)")

    fields = [(FIELD_UID, uid), (FIELD_NAME, name_bytes)]

    # I4, §2: the device key. Its own field rather than a file on the SD card,
    # because a key that does not survive a reflash is a key somebody has to
    # reinstall by hand on a board that may have no console to type it on --
    # which is exactly the case this option exists for.
    devkey = None
    if args.key and args.key_generate:
        sys.exit("--key and --key-generate are alternatives, not a pair")
    if args.key_generate:
        devkey = secrets.token_bytes(32)
    elif args.key:
        try:
            devkey = bytes.fromhex(args.key)
        except ValueError:
            sys.exit("--key must be hex")
        if not (16 <= len(devkey) <= 64):
            sys.exit("--key must be 16 to 64 bytes (32 to 128 hex characters)")
    if args.grant:
        print(f"  grants     : {len(args.grant)} " +
              ", ".join(g.split(":")[0] for g in args.grant))

    if devkey is not None:
        fields.append((FIELD_DEVKEY, devkey))

    psk_hex = None
    if args.wlan_ssid or args.wlan_passphrase:
        if not (args.wlan_ssid and args.wlan_passphrase):
            sys.exit("--wlan-ssid and --wlan-passphrase must be given together")
        try:
            psk = derive_wpa2_psk(args.wlan_ssid, args.wlan_passphrase)
        except ValueError as e:
            sys.exit(str(e))
        psk_hex = psk.hex()
        fields.append((FIELD_WLAN_SSID, args.wlan_ssid.encode("utf-8")))
        fields.append((FIELD_WLAN_PSK, psk))

    # I5's grants, in the record rather than on an SD card. Serialised here in
    # the exact one-line-per-grant form fs/9p.c's parse_grant_line() reads --
    # deliberately the same bytes the board would have written itself, so the
    # host and the device agree by construction rather than by a second format.
    if args.grant:
        lines = []
        for spec in args.grant:
            parts = spec.split(":")
            if len(parts) < 2:
                sys.exit(f"--grant '{spec}': want NAME:KEYHEX[:ANAME[:ro]]")
            gname, ghex = parts[0], parts[1]
            ganame = parts[2] if len(parts) > 2 and parts[2] else "/"
            gmode = parts[3] if len(parts) > 3 and parts[3] else "rw"
            if not gname:
                sys.exit(f"--grant '{spec}': the name may not be empty")
            if gmode not in ("ro", "rw"):
                sys.exit(f"--grant '{spec}': mode must be 'ro' or 'rw'")
            if not ganame.startswith("/"):
                sys.exit(f"--grant '{spec}': aname must start with '/'")
            try:
                gkey = bytes.fromhex(ghex)
            except ValueError:
                sys.exit(f"--grant '{spec}': the key must be hex")
            if not (16 <= len(gkey) <= 64):
                sys.exit(f"--grant '{spec}': key must be 16 to 64 bytes")
            lines.append(f"{gname} {gkey.hex()} {ganame} {gmode}\n")
        blob = "".join(lines).encode("ascii")
        if len(blob) > 1536:   # NODE_GRANTS_MAX (kernel/identity.h)
            sys.exit("--grant: the grants list exceeds what the record holds")
        fields.append((FIELD_GRANTS, blob))

    if args.ipv4:
        parts = args.ipv4.split("/")
        if len(parts) not in (2, 3):
            sys.exit("--ipv4 wants IP/MASK or IP/MASK/GW")
        if len(parts) == 2:
            parts.append("0.0.0.0")   # no router on this segment; a real configuration
        quads = b""
        for part in parts:
            octets = part.split(".")
            if len(octets) != 4:
                sys.exit(f"--ipv4: '{part}' is not a dotted quad")
            try:
                vals = [int(o) for o in octets]
            except ValueError:
                sys.exit(f"--ipv4: '{part}' is not a dotted quad")
            if any(v < 0 or v > 255 for v in vals):
                sys.exit(f"--ipv4: '{part}' has an octet outside 0-255")
            quads += bytes(vals)
        # Same refusals as kernel/identity.c's node_identity_set_ipv4(): an
        # all-zero address or mask describes a board that comes up looking
        # configured and answering nothing.
        if quads[0:4] == b"\0\0\0\0":
            sys.exit("--ipv4: 0.0.0.0 is not an address")
        if quads[4:8] == b"\0\0\0\0":
            sys.exit("--ipv4: a zero netmask puts every destination off-link")
        fields.append((FIELD_IPV4, quads))

    record = build_record(fields)
    with open(args.output, "wb") as f:
        f.write(record)

    msg = f"wrote {args.output}: name={args.name!r} uid={uid.hex()}"
    if psk_hex:
        msg += f" wlan_ssid={args.wlan_ssid!r} wlan_psk={psk_hex}"
    print(msg)

    if devkey is not None:
        # The fingerprint, never the key -- matching what the board's own
        # `identity` command will print back, so the two can be compared
        # without either of them ever showing the secret.
        fp = hashlib.sha256(devkey).hexdigest()[:16]
        print(f"  device key : {len(devkey)} bytes, fingerprint {fp}")
        if args.key_out:
            with open(args.key_out, "w") as f:
                f.write(devkey.hex() + "\n")
            os.chmod(args.key_out, 0o600)
            print(f"  key written: {args.key_out} (mode 600) -- this is the secret itself")
        elif args.key_generate:
            print("  NOTE: no --key-out given, so this key now exists only in the image "
                  "above.\n        Nothing will be able to authenticate to the board "
                  "that gets it.")

    if args.uf2:
        repo_root = Path(__file__).resolve().parent.parent
        print(f"  wrote {write_uf2(record, args.uf2, repo_root)}")
        print("  flash it like any other segment:  "
              "uv run tests/hw/flash.py --uf2 " + args.uf2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
