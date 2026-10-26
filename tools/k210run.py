#!/usr/bin/env python3
"""K210 host tool: BROM ISP loader + console (phase 65, from 65.1).

The Maix Bit's CH552 download circuit derives the K210's reset/boot
state from the USB serial control lines: a session must keep the port
open with DTR/RTS asserted, or the board resets (a closing tool hands
the board back to the flash image). kflash works but opens/closes the
port, so this tool speaks the BROM ISP protocol itself — SLIP frames
directly — and owns the port for its whole life:

    open (DTR/RTS asserted) -> greeting -> memory-write chunks
        -> boot -> stay open and stream the console at the ISP baud

BROM protocol (the vendor "ISP" ops, as the K210 bootloader docs and
the kflash reference agree):

    0xC2 NOP            greeting (a 12-zero NOP frame answers with OK)
    0xC3 MEMORY_WRITE   crc32 + addr + len + data
    0xC5 MEMORY_BOOT    crc32 + addr + 0

Frames are SLIP (C0 ... C0, DB DC/DD escaping). Responses are
[op, reason] with reason 0xE0 == OK.

Usage:
    tools/k210run.py --load build/k210/minimal_k210.elf --capture 12
    tools/k210run.py --load x.elf --run-label K210-PROBE-DONE --send g
"""

from __future__ import annotations

import argparse
import elftools.elf.elffile
import glob
import os
import serial
import struct
import sys
import time
import zlib

GREETING = b"\xc0\xc2\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xc0"
CHUNK = 1024


def find_port(override: str | None) -> str:
    if override:
        return override
    for pat in ("*1a86*", "*USB2.0-Serial*"):
        hits = sorted(glob.glob(f"/dev/serial/by-id/{pat}"))
        if hits:
            return os.path.realpath(hits[0])
    sys.exit("no CH552/CH340 (1a86:) serial adapter found in /dev/serial/by-id")


def slip_encode(payload: bytes) -> bytes:
    body = payload.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
    return b"\xc0" + body + b"\xc0"


class SlipReader:
    """Yield SLIP packets; returns None on timeout (does not close the port)."""

    def __init__(self, ser: serial.Serial):
        self.ser = ser
        self.buf = b""

    def next_packet(self, timeout: float) -> bytes | None:
        t_end = time.monotonic() + timeout
        pkt = bytearray()
        escaped = False
        started = False
        while time.monotonic() < t_end:
            data = self.ser.read(4096)
            for b in data:
                if not started:
                    if b == 0xC0:
                        started = True
                    continue
                if escaped:
                    if b == 0xDC:
                        pkt.append(0xC0)
                    elif b == 0xDD:
                        pkt.append(0xDB)
                    else:
                        return None       # framing error
                    escaped = False
                elif b == 0xDB:
                    escaped = True
                elif b == 0xC0:
                    if pkt:
                        return bytes(pkt)
                    # else: back-to-back delimiter, keep waiting
                else:
                    pkt.append(b)
        return None


class K210Isp:
    def __init__(self, port: str, baud: int = 115200, to_isp: bool = True):
        # The Maix Bit's CH552 download circuit drives the K210 reset
        # and download-select lines from DTR/RTS (measured through
        # kflash with strace, 2026-10-26):
        #
        #   (dtr=0, rts=0) -> hold -> (0, 1) -> hold -> (1, 0)
        #
        # enters BROM ISP; (1, 0) is then the *run* state (the session
        # flashes and boots in it); dropping both lines -- which is what
        # closing the port does -- resets the board back to the flash
        # image. This object therefore owns the port for its whole life
        # and never leaves 0/0 except inside the entry pulse.
        self.ser = serial.Serial(baudrate=baud, timeout=0.1)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        self.reader = SlipReader(self.ser)
        if to_isp:
            self.enter_isp()

    def enter_isp(self) -> None:
        self.ser.dtr = False
        self.ser.rts = False
        time.sleep(0.1)
        self.ser.rts = True          # (0, 1): reset pulse, download sel
        time.sleep(0.1)
        self.ser.rts = False
        self.ser.dtr = True          # (1, 0): release into BROM ISP
        time.sleep(0.1)

    def close(self) -> None:
        self.ser.close()

    def _cmd(self, payload: bytes, timeout: float = 3.0) -> tuple[int, int]:
        self.ser.write(slip_encode(payload))
        pkt = self.reader.next_packet(timeout)
        if pkt is None or len(pkt) < 2:
            raise RuntimeError(f"no ISP response to op {payload[0]:#x} "
                               f"(got {pkt!r})")
        return pkt[0], pkt[1]

    def greeting(self, timeout: float = 10.0) -> None:
        # the BROM answers the greeting as a NOP_OK. Retry for a while:
        # the board may be mid-reset or the first greeting may race
        # the auto-download circuit.
        t_end = time.monotonic() + timeout
        tries = 0
        while time.monotonic() < t_end:
            tries += 1
            self.ser.reset_input_buffer()
            self.ser.write(GREETING)
            pkt = self.reader.next_packet(0.7)
            if pkt is not None and len(pkt) >= 2 and pkt[1] == 0xE0:
                return
        raise RuntimeError(f"greeting: no ISP_OK in {timeout}s "
                           f"({tries} attempts); board not in BROM?")

    def mem_write(self, addr: int, data: bytes) -> None:
        for off in range(0, len(data), CHUNK):
            chunk = data[off:off + CHUNK]
            head = struct.pack("<II", addr + off, len(chunk))
            crc = struct.pack("<I", zlib.crc32(head + chunk) & 0xFFFFFFFF)
            op, reason = self._cmd(struct.pack("<HH", 0xC3, 0) + crc + head + chunk)
            if reason != 0xE0:
                raise RuntimeError(f"mem_write@{addr + off:#x} -> reason {reason:#x}")

    def boot(self, addr: int = 0x80000000) -> None:
        head = struct.pack("<II", addr, 0)
        crc = struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)
        # the boot command answers right as the chip jumps away; accept
        # either a short OK or silence as success
        try:
            self._cmd(struct.pack("<HH", 0xC5, 0) + crc + head, timeout=1.0)
        except RuntimeError:
            pass

    def write_str(self, s: str) -> None:
        self.ser.write(s.encode())


def elf_load_segments(path: str) -> list[tuple[int, bytes]]:
    out: list[tuple[int, bytes]] = []
    with open(path, "rb") as f:
        elf = elftools.elf.elffile.ELFFile(f)
        if elf.header["e_entry"] != 0x80000000:
            print(f"warning: entry {elf.header['e_entry']:#x} != 0x80000000",
                  file=sys.stderr)
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD" or seg["p_filesz"] == 0:
                continue
            out.append((seg["p_paddr"], seg.data()))
    if not out:
        raise RuntimeError(f"{path}: no loadable PT_LOAD segment")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--load", required=True, help="ELF to SRAM-load and boot")
    ap.add_argument("--capture", type=float, default=10.0,
                    help="seconds of console to capture after boot")
    ap.add_argument("--send", default="g",
                    help="byte(s) to poke the probe with while waiting")
    ap.add_argument("--until", default=None,
                    help="stop capturing after this marker appears")
    args = ap.parse_args()

    port = find_port(args.port)
    segs = elf_load_segments(args.load)
    print(f"port {port}; loading {args.load}: " +
          ", ".join(f"{a:#x}+{len(d)}" for a, d in segs))

    isp = K210Isp(port, args.baud)
    try:
        isp.greeting()
        print("greeting OK")
        for addr, data in segs:
            isp.mem_write(addr, data)
        print("download OK; booting")
        isp.boot(0x80000000)

        blob = bytearray()
        t_end = time.monotonic() + args.capture
        t_poke = time.monotonic() + 2.0
        while time.monotonic() < t_end:
            if time.monotonic() < t_poke:
                isp.write_str(args.send)   # the probe waits for a byte
            blob += isp.ser.read(4096)
            time.sleep(0.05)
            if args.until and args.until.encode() in blob:
                break
        sys.stdout.write(bytes(blob).decode("ascii", "replace"))
        sys.stdout.write(f"\n--- captured {len(blob)} bytes ---\n")
    finally:
        isp.close()


if __name__ == "__main__":
    main()
