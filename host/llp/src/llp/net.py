"""Just enough of a host's IP stack to prove a radio passes frames (49.3):
DHCP with the radio's MAC, ARP (asking, and answering for our address), and
ICMP echo. Every packet is built and checked by hand -- this is a test
instrument, not a stack.
"""

from __future__ import annotations

import os
import struct
import time
from collections.abc import Callable
from dataclasses import dataclass

ETH_ARP = 0x0806
ETH_IP = 0x0800
BCAST = b"\xff" * 6


def csum(b: bytes) -> int:
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(f"!{len(b) // 2}H", b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_str(b: bytes) -> str:
    return ".".join(str(x) for x in b)


def eth(dst: bytes, src: bytes, etype: int, payload: bytes) -> bytes:
    f = dst + src + struct.pack("!H", etype) + payload
    return f + b"\0" * max(0, 60 - len(f))


def ipv4(src: bytes, dst: bytes, proto: int, payload: bytes, ident: int = 0) -> bytes:
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, 0, 64, proto, 0, src, dst)
    return h[:10] + struct.pack("!H", csum(h)) + h[12:] + payload


def udp(sport: int, dport: int, payload: bytes) -> bytes:
    return struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload   # checksum 0: allowed on IPv4


@dataclass
class Lease:
    ip: bytes
    mask: bytes
    gw: bytes
    server: bytes
    lease_s: int


class Host:
    """send(frame) / recv(timeout) -> frame | None: an LLP node's ETH queue."""

    def __init__(self, mac: bytes, send: Callable[[bytes], None], recv: Callable[[float], bytes | None],
                 log: Callable[[str], None] = print) -> None:
        self.mac = mac
        self.send = send
        self.recv = recv
        self.log = log
        self.ip = b"\0\0\0\0"
        self.arp: dict[bytes, bytes] = {}

    # Frames that are not what a caller waits for still get handled: ARP
    # requests for our address are answered, replies are learned.
    def _handle_arp(self, f: bytes) -> None:
        if len(f) < 42 or struct.unpack("!H", f[12:14])[0] != ETH_ARP:
            return
        op = struct.unpack("!H", f[20:22])[0]
        sha, spa, tpa = f[22:28], f[28:32], f[38:42]
        self.arp[spa] = sha
        if op == 1 and tpa == self.ip and self.ip != b"\0\0\0\0":
            rep = struct.pack("!HHBBH6s4s6s4s", 1, ETH_IP, 6, 4, 2, self.mac, self.ip, sha, spa)
            self.send(eth(sha, self.mac, ETH_ARP, rep))

    def _wait(self, want: Callable[[bytes], bool], timeout: float) -> bytes | None:
        t = time.monotonic() + timeout
        while (left := t - time.monotonic()) > 0:
            f = self.recv(min(left, 0.2))
            if f is None:
                continue
            self._handle_arp(f)
            if want(f):
                return f
        return None

    # --- DHCP -------------------------------------------------------------

    def _dhcp_msg(self, xid: int, mtype: int, extra: bytes = b"") -> bytes:
        bootp = struct.pack("!BBBBIHH4s4s4s4s16s64s128s", 1, 1, 6, 0, xid, 0, 0x8000,
                            b"\0" * 4, b"\0" * 4, b"\0" * 4, b"\0" * 4, self.mac.ljust(16, b"\0"), b"", b"")
        opts = b"\x63\x82\x53\x63" + bytes((53, 1, mtype)) + extra + bytes((55, 3, 1, 3, 6, 255))
        pkt = ipv4(b"\0\0\0\0", b"\xff\xff\xff\xff", 17, udp(68, 67, bootp + opts))
        return eth(BCAST, self.mac, ETH_IP, pkt)

    @staticmethod
    def _dhcp_parse(f: bytes, xid: int) -> tuple[int, bytes, dict[int, bytes]] | None:
        if len(f) < 14 + 20 + 8 + 240 or struct.unpack("!H", f[12:14])[0] != ETH_IP or f[23] != 17:
            return None
        ihl = (f[14] & 0x0F) * 4
        u = 14 + ihl
        if struct.unpack("!HH", f[u:u + 4]) != (67, 68):
            return None
        b = f[u + 8:]
        if b[0] != 2 or struct.unpack("!I", b[4:8])[0] != xid or b[236:240] != b"\x63\x82\x53\x63":
            return None
        opts: dict[int, bytes] = {}
        i = 240
        while i < len(b) and b[i] != 255:
            if b[i] == 0:
                i += 1
                continue
            opts[b[i]] = b[i + 2:i + 2 + b[i + 1]]
            i += 2 + b[i + 1]
        if 53 not in opts:
            return None
        return opts[53][0], b[16:20], opts

    def dhcp(self, timeout: float = 15.0) -> Lease | None:
        xid = int.from_bytes(os.urandom(4), "big")
        for attempt in range(3):
            self.send(self._dhcp_msg(xid, 1))
            f = self._wait(lambda f: (p := self._dhcp_parse(f, xid)) is not None and p[0] == 2, timeout / 3)
            if f is None:
                self.log(f"DHCP: no offer (attempt {attempt + 1})")
                continue
            parsed = self._dhcp_parse(f, xid)
            assert parsed is not None
            _, yiaddr, opts = parsed
            server = opts.get(54, b"\0\0\0\0")
            self.send(self._dhcp_msg(xid, 3, bytes((50, 4)) + yiaddr + bytes((54, 4)) + server))
            f = self._wait(lambda f: (p := self._dhcp_parse(f, xid)) is not None and p[0] in (5, 6), timeout / 3)
            if f is None:
                self.log("DHCP: no ACK")
                continue
            parsed = self._dhcp_parse(f, xid)
            assert parsed is not None
            mt, yiaddr, opts = parsed
            if mt != 5:
                self.log("DHCP: NAK")
                continue
            self.ip = yiaddr
            return Lease(yiaddr, opts.get(1, b"\xff\xff\xff\0"), opts.get(3, b"\0\0\0\0"), server,
                         struct.unpack("!I", opts[51])[0] if 51 in opts else 0)
        return None

    # --- ARP and ping -------------------------------------------------------

    def resolve(self, ip: bytes, timeout: float = 3.0) -> bytes | None:
        if ip in self.arp:
            return self.arp[ip]
        req = struct.pack("!HHBBH6s4s6s4s", 1, ETH_IP, 6, 4, 1, self.mac, self.ip, b"\0" * 6, ip)
        self.send(eth(BCAST, self.mac, ETH_ARP, req))
        self._wait(lambda f: ip in self.arp, timeout)
        return self.arp.get(ip)

    def ping(self, ip: bytes, count: int = 3, timeout: float = 2.0) -> list[float]:
        """Round-trip times in ms for the echoes that came back."""
        mac = self.resolve(ip)
        if mac is None:
            self.log(f"ARP: no answer from {ip_str(ip)}")
            return []
        ident = int.from_bytes(os.urandom(2), "big")
        rtts: list[float] = []
        for seq in range(count):
            body = struct.pack("!BBHHH", 8, 0, 0, ident, seq) + b"lugalos-llp-49.3"
            body = body[:2] + struct.pack("!H", csum(body)) + body[4:]
            t0 = time.monotonic()
            self.send(eth(mac, self.mac, ETH_IP, ipv4(self.ip, ip, 1, body, ident=seq)))

            def is_reply(f: bytes, seq: int = seq) -> bool:
                if len(f) < 42 or struct.unpack("!H", f[12:14])[0] != ETH_IP or f[23] != 1:
                    return False
                o = 14 + (f[14] & 0x0F) * 4
                return f[o] == 0 and struct.unpack("!HH", f[o + 4:o + 8]) == (ident, seq)

            if self._wait(is_reply, timeout) is not None:
                rtts.append((time.monotonic() - t0) * 1000)
        return rtts
