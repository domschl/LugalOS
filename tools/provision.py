#!/usr/bin/env python3
"""tools/provision.py -- I2, plan/phase21_identity_and_authentication.md.

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

Bootstrap note (§6): this writes a file on the host. Getting the resulting
image onto a board is a separate, explicit step (attaching it as QEMU's
second virtio-blk drive, or -- once I7 lands -- flashing it to the reserved
sector) precisely because provisioning over a running node's own network
must stay refused, not merely discouraged.
"""

import argparse
import secrets
import struct
import sys

IDSTORE_SIZE_BYTES = 4096
IDSTORE_HEADER_LEN = 12
MAGIC = b"LGID"
VERSION = 1

FIELD_UID = 1
FIELD_NAME = 2


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
    ap.add_argument("output", help="path to write the identity image to")
    ap.add_argument("--name", required=True, help="the instance name (§2: instance scope, freely rewritable)")
    ap.add_argument("--uid", help="16 hex chars (8 bytes); a fresh random UID is minted if omitted")
    args = ap.parse_args()

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

    record = build_record([(FIELD_UID, uid), (FIELD_NAME, name_bytes)])
    with open(args.output, "wb") as f:
        f.write(record)

    print(f"wrote {args.output}: name={args.name!r} uid={uid.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
