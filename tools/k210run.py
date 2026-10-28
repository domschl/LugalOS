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

# The ISP conversation itself starts at 115200 (the BROM's fixed greeting
# rate). Op 0xC6 is the RAM-mode baud switch -- the frame the kflash
# reference calls "stage0": crc32 over pack('<III', 0, 4, baud), and the
# BROM then speaks `baud` for the rest of the session. Measured on this
# Maix Bit (CH552 CDC link): it does NOT answer at the old rate (no SLIP
# ack to observe -- the switch happens under the reply), so success is
# proven by re-greeting at the new rate, and failure by that greeting
# timing out while 115200 answers again. 1.5 Mbaud is kflash's fixed
# stage0 value and cuts the 1.7 MB kernel download from ~5 min to ~15 s.
# (The flash-mode switch, op 0xD6, is answered but belongs to the flash
# protocol's state machine; using it during a RAM session simply gets no
# reply -- measured.) Vendor caution in kflash says CH340-serial boards
# (which includes the Maix Bit's download circuit) skip the fast path;
# that caution is about boards whose adapter cannot clock 1.5 M, and this
# link has now been measured taking it.
ISP_OP_BAUD_RAM = 0xC6


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
    def __init__(self, port: str, baud: int | None = None, to_isp: bool = True):
        # `baud` is accepted for symmetry with set_baud() but ignored on
        # open: the BROM greeting always answers at 115200; callers reach
        # a faster rate with greeting() then set_baud().
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
        self.ser = serial.Serial(baudrate=115200, timeout=0.1)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        self.reader = SlipReader(self.ser)
        self.leaked = b""      # console bytes that arrived during boot()
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

    def set_baud(self, baud: int) -> bool:
        """Switch the live ISP session to `baud` (op 0xC6); True only if
        the BROM answers the greeting at the new rate. On any failure the
        session is left back at 115200 and able to greet."""
        body = struct.pack("<III", 0, 4, baud)
        crc = struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        self.ser.write(slip_encode(struct.pack("<HH", ISP_OP_BAUD_RAM, 0) + crc + body))
        time.sleep(0.05)               # the switch lands before any reply could form
        self.ser.baudrate = baud
        self.ser.reset_input_buffer()
        for _ in range(3):
            self.ser.write(GREETING)
            pkt = self.reader.next_packet(0.5)
            if pkt is not None and len(pkt) >= 2 and pkt[1] == 0xE0:
                return True
        # not answering there: come back at the boot rate so the caller's
        # fallback path (stay at 115200) actually works
        self.ser.baudrate = 115200
        self.ser.reset_input_buffer()
        for _ in range(3):
            self.ser.write(GREETING)
            pkt = self.reader.next_packet(0.5)
            if pkt is not None and len(pkt) >= 2 and pkt[1] == 0xE0:
                break
        return False

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

    def release(self) -> None:
        """ISP_REBOOT (0xD5): end the stub's residency.

        After a successful `boot` jump the ISP stub's SPI3 driver can
        remain the owner of the XPI NOR's pads -- a booted blob then
        clocks into a bus the stub still holds and reads nothing (the
        symptom that reads as 'every SPI read returns 00/FF'). One
        0xD5 byte releases it: measured, RDID goes from ffffff to the
        real C8 60 18 on the very next command. Best-effort: an older
        stub may not answer, and a board already in flash-boot has no
        stub at all."""
        try:
            self.ser.write(b"\xd5")
            self.ser.flush()
            time.sleep(0.05)
            self.ser.reset_input_buffer()
        except Exception:
            pass

    def boot(self, addr: int = 0x80000000) -> None:
        head = struct.pack("<II", addr, 0)
        crc = struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)
        # The boot command answers right as the chip jumps away -- and the
        # bytes the booted program prints immediately arrive on the same
        # wires microseconds later. They are NOT ISP traffic: swallowing
        # them here (as an earlier revision did, reading them as a
        # malformed ack) loses exactly the first, most valuable console
        # characters of a boot. So: write the op, then drain raw bytes for
        # a beat, sift the ack (C0 C5 E0 C0) out of them, and hand the
        # rest to the caller via .leaked so capture prepends it.
        self.ser.write(slip_encode(struct.pack("<HH", 0xC5, 0) + crc + head))
        ack_pat = bytes([0xC0, 0xC5, 0xE0, 0xC0])
        buf = bytearray()
        t_end = time.monotonic() + 0.6
        while time.monotonic() < t_end:
            self.ser.timeout = 0.2
            b = self.ser.read(4096)
            if not b:
                break
            buf.extend(b)
            if ack_pat in buf:
                break
        if ack_pat in buf:
            pre, _, rest = bytes(buf).partition(ack_pat)
            self.leaked = pre + rest
        else:
            self.leaked = bytes(buf)
        # bytes after the ack may still be in flight; capture's first
        # read picks them up on its own.


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
            # Skip the non-RISC-V-Code segment the toolchain emits
            # (.riscv.attributes arrives as a PT_LOAD at vaddr 0): writing
            # it to address 0 aims at the K210's flash-XIP window, not
            # SRAM. kflash skips it the same way (p_vaddr==0).
            if not (seg["p_paddr"] & 0x80000000):
                continue
            out.append((seg["p_paddr"], seg.data()))
    if not out:
        raise RuntimeError(f"{path}: no loadable PT_LOAD segment")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=115200,
                    help="ISP transfer baud after the 0xC6 switch. "
                         "Default 115200: MEASURED on this Maix Bit "
                         "(CH552), MEMORY_WRITE after the switch runs "
                         "at ~9 KB/s -- slower than plain 115200 -- "
                         "matching kflash's own note that CH340/CH552 "
                         "boards cannot take the fast path; 1500000 is "
                         "kept available to re-measure other links")
    ap.add_argument("--load", required=True, help="ELF to SRAM-load and boot")
    ap.add_argument("--capture", type=float, default=10.0,
                    help="seconds of console to capture after boot")
    ap.add_argument("--send", default="g",
                    help="byte(s) to poke the console with once, 1 s after "
                         "boot (the 65.1 probe waits for any byte); pass "
                         "'' to send nothing")
    ap.add_argument("--exec", action="append", default=[],
                    metavar="CMD", help="console command to type at the "
                    "running image (newline appended); repeatable, sent "
                    "--exec-wait apart, starting after the --send poke")
    ap.add_argument("--exec-wait", type=float, default=2.0,
                    help="seconds between console injections")
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
        isp_fast = False
        if args.baud != 115200:  # default 115200 -- see --baud's measurement
            if isp.set_baud(args.baud):
                isp_fast = True
                print(f"ISP now at {args.baud} bps")
            else:
                print("warning: no ack for the baud switch; staying at 115200")
        for addr, data in segs:
            isp.mem_write(addr, data)
        print("download OK; booting")
        isp.boot(0x80000000)

        # The booted program's UARTHS runs at the BROM divisor (115200 --
        # uart_k210.c deliberately never touches it), while this session
        # may still sit at the ISP transfer rate: the 0xD6 switch lives
        # only in the BROM's conversation, not in the program we jump
        # into. So anything after the ack is 115200 stream read at 1.5M
        # (garbage), and the first milliseconds of boot are unsalvageable
        # in fast mode: switch back, clear the pipe, capture from here.
        # Use --baud 115200 when the very first characters matter.
        if isp_fast:
            time.sleep(0.05)            # let the ack's echo settle
            isp.ser.baudrate = 115200
            isp.ser.reset_input_buffer()
            isp.leaked = b""

        blob = bytearray(isp.leaked)   # the boot message began before the ack
        # one-shot console injections on a schedule: the --send poke at +1 s
        # (probes block on any byte), then each --exec line --exec-wait
        # apart, giving the shell a chance to be up before typing.
        schedule: list[tuple[float, bytes]] = []
        if args.send:
            schedule.append((time.monotonic() + 1.0, args.send.encode()))
        t_next = time.monotonic() + 1.0 + args.exec_wait
        for cmd in args.exec:
            schedule.append((t_next, (cmd + "\r").encode()))
            t_next += args.exec_wait
        t_end = time.monotonic() + args.capture
        while time.monotonic() < t_end:
            while schedule and time.monotonic() >= schedule[0][0]:
                isp.write_str(schedule.pop(0)[1].decode("ascii", "replace"))
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
