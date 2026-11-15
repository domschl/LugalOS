#!/usr/bin/env python3
"""Generate a compact binary root-CA trust store (roots.bin) from a PEM bundle.

Format ("LCRT", version 1):
  Header (8 bytes):
    magic:   4 bytes  b"LCRT"
    version: uint16 LE (1)
    count:   uint16 LE (N)
  Entries (N):
    subject_hash: 32 bytes (SHA-256 of raw DER subject Name SEQUENCE)
    key_type:     uint8  (1 = P-256, 2 = RSA)
    e_len:        uint8  (0 for P-256; byte length of RSA exponent)
    key_len:      uint16 LE (64 for P-256; byte length of RSA modulus)
    key_data:     key_len + e_len bytes (X||Y for P-256; n||e for RSA)
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import re
import struct
import sys
from pathlib import Path

OID_EC_PUBKEY = bytes([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01])
OID_P256 = bytes([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07])
OID_RSA_ENCR = bytes([0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01])


def der_next(buf: bytes, pos: int) -> tuple[int, bytes, bytes, int]:
    """Return (tag, content, raw_tlv, next_pos)."""
    start = pos
    tag = buf[pos]
    pos += 1
    lbyte = buf[pos]
    pos += 1
    if lbyte & 0x80:
        nlen = lbyte & 0x7F
        length = int.from_bytes(buf[pos : pos + nlen], "big")
        pos += nlen
    else:
        length = lbyte
    end = pos + length
    return tag, buf[pos:end], buf[start:end], end


def parse_root_cert(der: bytes) -> tuple[bytes, int, bytes, bytes] | None:
    """Return (subject_hash, key_type, key_bytes, exp_bytes) or None if unsupported."""
    try:
        tag, cert, _, end = der_next(der, 0)
        if tag != 0x30 or end != len(der):
            return None
        tag, tbs, _, _ = der_next(cert, 0)
        if tag != 0x30:
            return None

        p = 0
        tag, _, _, p2 = der_next(tbs, p)
        if tag == 0xA0:  # optional version
            p = p2
        _, _, _, p = der_next(tbs, p)  # serial
        _, _, _, p = der_next(tbs, p)  # sig alg
        _, _, _, p = der_next(tbs, p)  # issuer
        _, _, _, p = der_next(tbs, p)  # validity
        _, _, subject_raw, p = der_next(tbs, p)
        tag, spki, _, p = der_next(tbs, p)
        if tag != 0x30:
            return None

        sp = 0
        _, alg, _, sp = der_next(spki, sp)
        ap = 0
        _, oid, _, ap = der_next(alg, ap)
        _, bits, _, _ = der_next(spki, sp)
        if not bits or bits[0] != 0:
            return None
        key_bits = bits[1:]

        subj_hash = hashlib.sha256(subject_raw).digest()

        if oid == OID_EC_PUBKEY:
            _, curve_oid, _, _ = der_next(alg, ap)
            if curve_oid != OID_P256 or len(key_bits) != 65 or key_bits[0] != 0x04:
                return None
            return subj_hash, 1, key_bits[1:], b""

        if oid == OID_RSA_ENCR:
            _, rsa_seq, _, _ = der_next(key_bits, 0)
            rp = 0
            _, n, _, rp = der_next(rsa_seq, rp)
            _, e, _, rp = der_next(rsa_seq, rp)
            n = n.lstrip(b"\x00")
            e = e.lstrip(b"\x00")
            if not n or len(n) > 512 or not e or len(e) > 8:
                return None
            return subj_hash, 2, n, e
    except (IndexError, ValueError):
        return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--in",
        dest="pem_in",
        default="/etc/ssl/certs/ca-certificates.crt",
        help="Input PEM CA bundle",
    )
    ap.add_argument(
        "--out",
        dest="bin_out",
        default="tools/sd_root/system/etc/ssl/roots.bin",
        help="Output compact binary root store",
    )
    args = ap.parse_args()

    pem_path = Path(args.pem_in)
    if not pem_path.exists():
        print(f"error: {pem_path} not found", file=sys.stderr)
        return 1

    pem_text = pem_path.read_text(encoding="utf-8", errors="replace")
    blocks = re.findall(
        r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----",
        pem_text,
        re.DOTALL,
    )

    entries: list[bytes] = []
    seen: set[tuple[bytes, bytes]] = set()
    for blk in blocks:
        der = base64.b64decode("".join(blk.split()))
        parsed = parse_root_cert(der)
        if not parsed:
            continue
        subj_hash, key_type, key_data, exp_data = parsed
        dedup = (subj_hash, key_data)
        if dedup in seen:
            continue
        seen.add(dedup)
        entries.append(
            subj_hash
            + struct.pack("<BBH", key_type, len(exp_data), len(key_data))
            + key_data
            + exp_data
        )

    out_path = Path(args.bin_out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    header = b"LCRT" + struct.pack("<HH", 1, len(entries))
    out_path.write_bytes(header + b"".join(entries))
    print(f"Wrote {len(entries)} roots ({out_path.stat().st_size} bytes) to {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
