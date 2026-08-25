#!/usr/bin/env python3
"""A packet-level peer on the other end of a QEMU virtio-net link.

R1, plan/phase19_ip_stack_and_ethernet.md §4(a). This is the testbed a
hand-written IP stack needs, and it is most of why phase 19 is affordable
without hardware.

**How it works.** QEMU's `dgram` network backend puts raw Ethernet frames on
a plain UDP socket: whatever the guest transmits arrives here as one datagram
per frame, and whatever is sent to the guest's port arrives in its receive
queue as one frame. No tap device, no bridge, no root, nothing to clean up if
a test crashes -- and, unlike a slirp `hostfwd` (which is what §4(b) uses for
the end-to-end host tools), full control over every byte on the wire.

That control is the point. A peer that can only speak correct TCP cannot test
a TCP implementation. This one can send a truncated IP header, a bad checksum,
a segment out of order, a RST mid-stream, or two frames with the same
sequence number, and can assert byte-for-byte on what came back.

**Usage:**

    peer = NetPeer()
    session.start(extra_qemu_args=peer.qemu_args())
    ...
    peer.send(eth_frame(dst=..., src=..., ethertype=0x88b5, payload=b"hi"))
    frame = peer.expect_frame(timeout=3.0)
    peer.close()

Kept deliberately free of any protocol knowledge beyond Ethernet framing: R2
adds ARP/IPv4/ICMP builders, R3 adds TCP, and each of those belongs next to
the milestone that needs it rather than here in advance.
"""

from __future__ import annotations

import socket
import threading

ETH_HDR_LEN = 14
BROADCAST = b"\xff" * 6

# IEEE-reserved for local experimental use, so a frame carrying it can never
# be confused with a real protocol -- which is exactly what a test frame
# wants. kernel/shell.c's `net txtest` builds frames with this type.
ETHERTYPE_TEST = 0x88B5
TEST_TAG = b"LUGALOS-NETIF-TEST"


def eth_frame(dst: bytes, src: bytes, ethertype: int, payload: bytes,
              min_len: int = 60) -> bytes:
    """One Ethernet II frame, zero-padded to `min_len` (no FCS -- neither
    virtio-net nor the ENC28J60 hands one up)."""
    if len(dst) != 6 or len(src) != 6:
        raise ValueError("MAC addresses are six bytes")
    frame = dst + src + ethertype.to_bytes(2, "big") + payload
    if len(frame) < min_len:
        frame += bytes(min_len - len(frame))
    return frame


def parse_eth(frame: bytes) -> tuple[bytes, bytes, int, bytes]:
    """(dst, src, ethertype, payload)."""
    if len(frame) < ETH_HDR_LEN:
        raise ValueError(f"runt frame, {len(frame)} bytes")
    return (frame[0:6], frame[6:12],
            int.from_bytes(frame[12:14], "big"), frame[ETH_HDR_LEN:])


def mac_str(mac: bytes) -> str:
    return ":".join(f"{b:02x}" for b in mac)


def expected_test_frame(src_mac: bytes, seq: int) -> bytes:
    """What kernel/shell.c's build_test_frame() produces, rebuilt here rather
    than described, so the assertion is byte-for-byte rather than a substring
    match on a hex dump."""
    payload = TEST_TAG + seq.to_bytes(4, "big")
    return eth_frame(BROADCAST, src_mac, ETHERTYPE_TEST, payload)


class NetPeer:
    """The far end of the guest's only network cable.

    Binds two UDP ports on the loopback: one this object receives on, and one
    the guest receives on. `qemu_args()` returns the `-netdev`/`-device` pair
    that wires them together. Frames are collected by a background thread from
    the moment the peer is constructed, so nothing the guest sends during boot
    is missed.
    """

    def __init__(self, host: str = "127.0.0.1") -> None:
        self.host = host

        self._rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._rx.bind((host, 0))
        self.peer_port: int = self._rx.getsockname()[1]

        # The guest's port has to be chosen before QEMU starts and cannot be
        # held open while it does -- QEMU binds it itself. Picking it by
        # binding and immediately closing leaves a small race with any other
        # process on the machine, which is the same race every ephemeral-port
        # test in this suite already runs and has never lost.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind((host, 0))
        self.guest_port: int = probe.getsockname()[1]
        probe.close()

        self._tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.frames: list[bytes] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def qemu_args(self, netdev_id: str = "n0") -> list[str]:
        return [
            "-netdev",
            (f"dgram,id={netdev_id},"
             f"local.type=inet,local.host={self.host},local.port={self.guest_port},"
             f"remote.type=inet,remote.host={self.host},remote.port={self.peer_port}"),
            "-device", f"virtio-net-device,netdev={netdev_id}",
        ]

    def _reader(self) -> None:
        self._rx.settimeout(0.25)
        while not self._stop.is_set():
            try:
                data, _ = self._rx.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            with self._lock:
                self.frames.append(data)

    def send(self, frame: bytes) -> None:
        self._tx.sendto(frame, (self.host, self.guest_port))

    def received(self) -> list[bytes]:
        with self._lock:
            return list(self.frames)

    def clear(self) -> None:
        with self._lock:
            self.frames.clear()

    def wait_for(self, count: int, timeout: float = 5.0) -> list[bytes]:
        """Blocks until at least `count` frames have arrived, or `timeout`.
        Returns whatever is held, so a caller reports "2 of 3" rather than
        just "timed out"."""
        import time
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if len(self.frames) >= count:
                    return list(self.frames)
            time.sleep(0.02)
        return self.received()

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)
        try:
            self._rx.close()
        finally:
            self._tx.close()

    def __enter__(self) -> "NetPeer":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()
