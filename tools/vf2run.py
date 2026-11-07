#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial"]
# ///
"""Load a LugalOS RAM image onto a StarFive VisionFive 2 via U-Boot and watch its console.

The VisionFive 2's onboard QSPI NOR flash holds SPL + OpenSBI + U-Boot 2021.10
and is never modified by LugalOS. Instead, vf2run.py talks to U-Boot on UART0
(/dev/ttyUSB0, 115200 8N1), transfers build/visionfive2/lugalos.bin into DRAM
at 0x40200000 via an embedded UDP TFTP server over Gigabit Ethernet (or YMODEM
over UART as a zero-network fallback), and boots it in S-mode via:

    booti 0x40200000 - ${fdtcontroladdr}

If the board is already running LugalOS (at the `/> ` prompt), vf2run.py issues
`reboot` (which triggers OpenSBI SRST cold reset) and catches U-Boot's autoboot
prompt automatically.

Usage:
    tools/vf2run.py --listen
    tools/vf2run.py --kernel build/visionfive2/lugalos.bin --cmd "uname; ps" --reboot
"""

import argparse
import glob
import os
import socket
import struct
import subprocess
import sys
import threading
import time

import serial

BYID_GLOB = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
LOAD_ADDR = 0x40200000
DEFAULT_BAUD = 115200


def find_port(explicit: str | None) -> str:
    p = explicit or os.environ.get("LUGALOS_VF2_PORT")
    if p:
        return p
    found = sorted(glob.glob(BYID_GLOB))
    if found:
        return found[0]
    if os.path.exists("/dev/ttyUSB0"):
        return "/dev/ttyUSB0"
    sys.exit(f"no VisionFive 2 UART found (checked {BYID_GLOB} and /dev/ttyUSB0)")


def resolve_kernel_bin(path: str) -> str:
    path = os.path.abspath(path)
    if os.path.isdir(path):
        elf = os.path.join(path, "lugalos.elf")
        bin_path = os.path.join(path, "lugalos.bin")
        if os.path.exists(elf):
            path = elf
        elif os.path.exists(bin_path):
            return bin_path
    if path.endswith(".elf"):
        bin_path = path[:-4] + ".bin"
        objcopy = "riscv64-elf-objcopy"
        subprocess.run([objcopy, "-O", "binary", path, bin_path], check=True)
        return bin_path
    if not os.path.exists(path):
        sys.exit(f"kernel image not found: {path}")
    return path


def detect_host_ip(target_subnet_hint: str = "192.168.178.1") -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target_subnet_hint, 80))
        return s.getsockname()[0]
    finally:
        s.close()


class MiniTFTPServer:
    """Minimal read-only RFC 1350 + RFC 2347/2348 (blksize/tsize) UDP TFTP server."""

    def __init__(self, file_bytes: bytes, host_ip: str = "0.0.0.0", port: int = 6969) -> None:
        self.data = file_bytes
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host_ip, port))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1.0)
        self.sock.close()

    def _serve(self) -> None:
        while not self._stop.is_set():
            try:
                pkt, addr = self.sock.recvfrom(2048)
            except TimeoutError:
                continue
            except OSError:
                break
            if len(pkt) < 4:
                continue
            opcode = struct.unpack("!H", pkt[:2])[0]
            if opcode == 1:  # RRQ
                threading.Thread(
                    target=self._handle_rrq, args=(pkt[2:], addr), daemon=True
                ).start()

    def _handle_rrq(self, payload: bytes, client_addr: tuple[str, int]) -> None:
        parts = payload.split(b"\x00")
        opts: dict[str, str] = {}
        i = 2
        while i + 1 < len(parts) and parts[i]:
            opts[parts[i].decode("ascii", errors="ignore").lower()] = (
                parts[i + 1].decode("ascii", errors="ignore")
            )
            i += 2

        blksize = 512
        oack_parts: list[bytes] = []
        if "blksize" in opts:
            try:
                blksize = max(8, min(1468, int(opts["blksize"])))
                oack_parts.extend([b"blksize", str(blksize).encode("ascii")])
            except ValueError:
                blksize = 512
        if "tsize" in opts:
            oack_parts.extend([b"tsize", str(len(self.data)).encode("ascii")])

        tsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tsock.settimeout(1.0)
        try:
            if oack_parts:
                oack = struct.pack("!H", 6) + b"\x00".join(oack_parts) + b"\x00"
                for _ in range(5):
                    tsock.sendto(oack, client_addr)
                    try:
                        ack, raddr = tsock.recvfrom(512)
                        if (
                            raddr == client_addr
                            and len(ack) >= 4
                            and struct.unpack("!HH", ack[:4]) == (4, 0)
                        ):
                            break
                    except TimeoutError:
                        continue
                else:
                    return

            total_len = len(self.data)
            num_blocks = (total_len // blksize) + 1
            for blk in range(1, num_blocks + 1):
                start = (blk - 1) * blksize
                chunk = self.data[start : start + blksize]
                dpkt = struct.pack("!HH", 3, blk & 0xFFFF) + chunk
                for _ in range(8):
                    tsock.sendto(dpkt, client_addr)
                    try:
                        ack, raddr = tsock.recvfrom(512)
                        if (
                            raddr == client_addr
                            and len(ack) >= 4
                            and struct.unpack("!HH", ack[:4]) == (4, blk & 0xFFFF)
                        ):
                            break
                    except TimeoutError:
                        continue
                else:
                    return
        finally:
            tsock.close()


def open_serial(port: str, baud: int = DEFAULT_BAUD) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.rtscts = False
    ser.dsrdtr = False
    ser.open()
    return ser


def read_until_any(
    ser: serial.Serial,
    markers: list[str],
    timeout_s: float,
    echo: bool = True,
) -> tuple[int, str]:
    deadline = time.monotonic() + timeout_s
    buf = ""
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            if echo:
                sys.stdout.write(text)
                sys.stdout.flush()
            buf += text
            for idx, m in enumerate(markers):
                if m in buf:
                    return idx, buf
    return -1, buf


def ensure_uboot_prompt(ser: serial.Serial, timeout_s: float = 120.0) -> None:
    """Bring the VisionFive 2 to the `StarFive #` U-Boot prompt."""
    ser.reset_input_buffer()
    ser.write(b"\x03\r")
    ser.flush()
    idx, _ = read_until_any(ser, ["StarFive #", "lsh> ", "/> ", "login:", "# "], 1.5, echo=False)
    if idx == 0:
        return

    if idx in (1, 2):
        print("[vf2run] Board is running LugalOS; issuing 'reboot' via OpenSBI SRST...")
        write_paced(ser, "reboot\r")
    else:
        print("[vf2run] Waiting for U-Boot prompt (interrupting autoboot if resetting)...")
        write_paced(ser, "reboot\r")

    deadline = time.monotonic() + timeout_s
    buf = ""
    while time.monotonic() < deadline:
        ser.write(b" ")
        ser.flush()
        chunk = ser.read(4096)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            buf += text
            if "StarFive #" in buf:
                ser.write(b"\r\n")
                ser.flush()
                read_until_any(ser, ["StarFive #"], 1.0, echo=False)
                return
        time.sleep(0.05)

    sys.exit("[vf2run] Timed out waiting for 'StarFive #' U-Boot prompt.")


def write_paced(ser: serial.Serial, text: str, delay_s: float = 0.002) -> None:
    """Send characters with slight pacing so the 16-byte dw-apb-uart RX FIFO never overflows."""
    for ch in text:
        ser.write(ch.encode("utf-8"))
        ser.flush()
        time.sleep(delay_s)


def uboot_cmd(ser: serial.Serial, cmd: str, timeout_s: float = 15.0, echo: bool = True) -> str:
    ser.reset_input_buffer()
    write_paced(ser, cmd + "\r")
    idx, out = read_until_any(ser, ["StarFive #"], timeout_s, echo=echo)
    if idx < 0:
        sys.exit(f"[vf2run] U-Boot command timed out: {cmd}")
    return out


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def load_via_ymodem(ser: serial.Serial, file_bytes: bytes, addr: int = LOAD_ADDR) -> None:
    import gzip

    gz_bytes = gzip.compress(file_bytes, compresslevel=9)
    scratch_addr = 0x44000000
    fast_baud = 1500000
    print(
        f"[vf2run] Loading {len(gz_bytes)} bytes (gzip of {len(file_bytes)} bytes) "
        f"to 0x{scratch_addr:08x} via YMODEM @ {fast_baud} baud..."
    )
    ser.reset_input_buffer()
    write_paced(ser, f"loady 0x{scratch_addr:08x} {fast_baud}\r")
    read_until_any(ser, ["press ENTER ...\r\n"], 5.0, echo=True)
    time.sleep(0.15)
    ser.baudrate = fast_baud
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.write(b"\r")
    ser.flush()
    idx, _ = read_until_any(ser, ["C"], 10.0, echo=True)
    if idx < 0:
        ser.baudrate = DEFAULT_BAUD
        sys.exit("[vf2run] U-Boot loady did not start (no 'C' handshake)")

    def wait_ack(timeout: float = 5.0) -> int:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            b = ser.read(1)
            if b in (b"\x06", b"\x15", b"\x18"):
                return b[0]
        return -1

    # Block 0: filename + file size
    meta = b"lugalos.bin.gz\x00" + str(len(gz_bytes)).encode("ascii") + b"\x00"
    meta_padded = meta.ljust(128, b"\x00")
    pkt0 = bytes([0x01, 0x00, 0xFF]) + meta_padded + struct.pack("!H", crc16_xmodem(meta_padded))
    ser.write(pkt0)
    ser.flush()
    if wait_ack() != 0x06:
        ser.baudrate = DEFAULT_BAUD
        sys.exit("[vf2run] YMODEM block 0 NAK")
    read_until_any(ser, ["C"], 5.0, echo=False)

    num_blocks = (len(gz_bytes) + 1023) // 1024
    for blk in range(1, num_blocks + 1):
        chunk = gz_bytes[(blk - 1) * 1024 : blk * 1024].ljust(1024, b"\x1a")
        seq = blk & 0xFF
        pkt = bytes([0x02, seq, 0xFF - seq]) + chunk + struct.pack("!H", crc16_xmodem(chunk))
        for _ in range(5):
            ser.write(pkt)
            ser.flush()
            if wait_ack() == 0x06:
                break
        else:
            ser.baudrate = DEFAULT_BAUD
            sys.exit(f"[vf2run] YMODEM block {blk}/{num_blocks} failed")
        if blk % 64 == 0 or blk == num_blocks:
            print(f"\r[vf2run] YMODEM {blk}/{num_blocks} KB", end="", flush=True)
    print()

    # EOT
    ser.write(b"\x04")
    ser.flush()
    wait_ack()
    read_until_any(ser, ["C"], 3.0, echo=False)

    # Empty block 0 to close batch
    empty0 = bytes([0x01, 0x00, 0xFF]) + (b"\x00" * 128) + struct.pack("!H", 0)
    ser.write(empty0)
    ser.flush()
    wait_ack()
    read_until_any(ser, ["press ESC ...\r\n"], 5.0, echo=True)
    time.sleep(0.15)
    ser.baudrate = DEFAULT_BAUD
    time.sleep(0.1)
    ser.write(b"\x1b\r")
    ser.flush()
    read_until_any(ser, ["StarFive #"], 5.0, echo=True)
    uboot_cmd(ser, f"unzip 0x{scratch_addr:08x} 0x{addr:08x}", timeout_s=10.0, echo=True)


def load_via_tftp(
    ser: serial.Serial,
    file_bytes: bytes,
    host_ip: str,
    addr: int = LOAD_ADDR,
) -> bool:
    tftp = MiniTFTPServer(file_bytes, host_ip="0.0.0.0", port=6969)
    tftp.start()
    try:
        print(
            f"[vf2run] Serving {len(file_bytes)} bytes via TFTP on {host_ip}:{tftp.port}..."
        )
        uboot_cmd(ser, "setenv autoload no; dhcp", timeout_s=15.0, echo=True)
        uboot_cmd(
            ser,
            f"setenv serverip {host_ip}; setenv tftpdstp {tftp.port}",
            timeout_s=5.0,
            echo=False,
        )
        ser.reset_input_buffer()
        write_paced(ser, f"tftpboot 0x{addr:08x} {host_ip}:lugalos.bin\r\n")
        idx, out = read_until_any(ser, ["StarFive #", "T T"], timeout_s=15.0, echo=True)
        if idx == 1 or "Bytes transferred =" not in out:
            ser.write(b"\x03\r\n")
            ser.flush()
            read_until_any(ser, ["StarFive #"], timeout_s=3.0, echo=False)
            return False
        return True
    finally:
        tftp.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="VisionFive 2 RAM loader and console runner")
    ap.add_argument("--port", help="Serial port (default: auto-detect VisionFive 2 UART0)")
    ap.add_argument("--kernel", help="Path to lugalos.bin, lugalos.elf, or build directory")
    ap.add_argument(
        "--method",
        choices=["ymodem", "tftp"],
        default="ymodem",
        help="RAM upload transport (default: ymodem @ 1.5 Mbps + gzip)",
    )
    ap.add_argument("--host-ip", help="Override host LAN IP for TFTP")
    ap.add_argument("--listen", action="store_true", help="Listen to serial console")
    ap.add_argument(
        "--listen-secs",
        type=float,
        default=10.0,
        help="Seconds to listen when --listen is used (default: 10)",
    )
    ap.add_argument("--cmd", help="Shell command(s) to run once LugalOS reaches `/> `")
    ap.add_argument(
        "--cmd-wait",
        type=float,
        default=10.0,
        help="Seconds to wait for --cmd completion (default: 10)",
    )
    ap.add_argument(
        "--reboot",
        action="store_true",
        help="Send 'reboot' after --cmd and verify return to U-Boot 'StarFive #'",
    )
    args = ap.parse_args()

    port = find_port(args.port)
    ser = open_serial(port)
    try:
        if args.listen and not args.kernel:
            read_until_any(ser, [], args.listen_secs, echo=True)
            return 0

        if args.kernel:
            bin_path = resolve_kernel_bin(args.kernel)
            with open(bin_path, "rb") as f:
                file_bytes = f.read()

            ensure_uboot_prompt(ser)
            host_ip = args.host_ip or detect_host_ip()

            loaded = False
            if args.method == "tftp":
                loaded = load_via_tftp(ser, file_bytes, host_ip, LOAD_ADDR)
                if not loaded:
                    print("[vf2run] TFTP did not complete; falling back to YMODEM...")
            if not loaded:
                load_via_ymodem(ser, file_bytes, LOAD_ADDR)

            print("[vf2run] Booting LugalOS via 'booti 0x40200000 - ${fdtcontroladdr}'...")
            write_paced(ser, f"booti 0x{LOAD_ADDR:08x} - ${{fdtcontroladdr}}\r")

            idx, _ = read_until_any(ser, ["lsh> ", "/> "], timeout_s=20.0, echo=True)
            if idx < 0:
                sys.exit("\n[vf2run] Timed out waiting for LugalOS `lsh> ` shell prompt.")

        if args.cmd:
            for single_cmd in [c.strip() for c in args.cmd.split(";") if c.strip()]:
                write_paced(ser, single_cmd + "\r")
                read_until_any(ser, ["lsh> ", "/> "], timeout_s=args.cmd_wait, echo=True)

        if args.reboot:
            print("\n[vf2run] Rebooting back to U-Boot via OpenSBI SRST...")
            ensure_uboot_prompt(ser)
            print("\n[vf2run] Board is back at 'StarFive #' prompt.")
        elif not args.cmd and args.kernel:
            read_until_any(ser, [], args.listen_secs, echo=True)

        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
