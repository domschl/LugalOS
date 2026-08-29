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
import secrets
import struct
import sys

IDSTORE_SIZE_BYTES = 4096
IDSTORE_HEADER_LEN = 12
MAGIC = b"LGID"
VERSION = 1

FIELD_UID = 1
FIELD_NAME = 2
FIELD_WLAN_SSID = 4
FIELD_WLAN_PSK = 5

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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", nargs="?", help="path to write the identity image to")
    ap.add_argument("--name", help="the instance name (§2: instance scope, freely rewritable)")
    ap.add_argument("--uid", help="16 hex chars (8 bytes); a fresh random UID is minted if omitted")
    ap.add_argument("--wlan-ssid", help="the network's SSID (I6, §5.3)")
    ap.add_argument("--wlan-passphrase", help="the network's WPA2 passphrase -- derived into a PSK here, "
                                              "never written to the image or sent to the board")
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

    record = build_record(fields)
    with open(args.output, "wb") as f:
        f.write(record)

    msg = f"wrote {args.output}: name={args.name!r} uid={uid.hex()}"
    if psk_hex:
        msg += f" wlan_ssid={args.wlan_ssid!r} wlan_psk={psk_hex}"
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
