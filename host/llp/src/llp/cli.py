"""`llp host`: be the host for a LugalOS radio on a serial port (49.3).

    uv run --extra serial --extra p9 llp host --port /dev/serial/by-id/usb-Espressif_..._AC:EB:...-if00 --arm

--arm types `llp radio` at the radio's console first (the C6-Zero shares its
one USB port between console and LLP). Then: the handshake, a DHCP lease
with the radio's MAC, a ping to the gateway, and -- with --cat -- a file read
from the radio's own namespace over 9P on the same link. Exit status 0 only if
every requested step worked; the last line is a one-line summary for tests.
"""

from __future__ import annotations

import argparse
import sys
import time

from .net import Host, ip_str
from .node import SerialNode


def _open_esp(port: str):  # noqa: ANN202 -- pyserial is optional
    """The way tools/c6run.py opens a C6's USB-Serial/JTAG: DTR high and RTS
    low *before* opening -- DTR low on open resets the chip (45.1)."""
    import serial
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.02
    s.dtr = True
    s.rts = False
    s.open()
    return s


def cmd_host(a: argparse.Namespace) -> int:
    ser = _open_esp(a.port)
    node = SerialNode(ser, echo_console=a.echo)
    summary: list[str] = []
    ok = True
    try:
        if a.arm:
            node.type_line("")
            node.type_line("llp radio")
            if not node.wait_console("llp: radio", 5.0):
                print("the radio did not arm LLP; console said:\n" + node.console_text()[-600:])
                return 1
        lk = node.start()
        if not node.wait_up(a.timeout):
            print(f"no LLP handshake in {a.timeout:.0f} s; console said:\n" + node.console_text()[-600:])
            return 1
        mac = lk.peer_mac
        summary.append(f"up mac={mac.hex(':')}")
        print(f"LLP up: peer role {lk.peer_role}, LLP {lk.peer_major}.{lk.peer_minor}, "
              f"max {lk.peer_max_payload}, mac {mac.hex(':')}")
        t = time.monotonic() + 10
        while not lk.peer_link and time.monotonic() < t:
            time.sleep(0.05)
        print(f"radio link: {'up' if lk.peer_link else 'down'}, rssi {lk.peer_rssi}")
        summary.append(f"wifi={'up' if lk.peer_link else 'down'}")

        host = Host(mac, node.send_eth, node.recv_eth)
        if a.dhcp:
            lease = host.dhcp()
            if lease is None:
                print("DHCP: no lease")
                ok = False
                summary.append("dhcp=FAIL")
            else:
                print(f"DHCP: {ip_str(lease.ip)}/{ip_str(lease.mask)} gw {ip_str(lease.gw)} "
                      f"server {ip_str(lease.server)} lease {lease.lease_s}s")
                summary.append(f"ip={ip_str(lease.ip)}")
                if a.ping:
                    rtts = host.ping(lease.gw, count=a.ping)
                    print(f"ping {ip_str(lease.gw)}: {len(rtts)}/{a.ping} replies"
                          + (f", rtt {min(rtts):.1f}..{max(rtts):.1f} ms" if rtts else ""))
                    summary.append(f"ping={len(rtts)}/{a.ping}")
                    ok = ok and len(rtts) > 0
        for path in a.cat or []:
            import p9lib
            client = p9lib.P9Client(node.p9_socket(), framing="raw")
            client.version()
            client.attach(1)
            data = client.cat(path)
            print(f"--- {path} ({len(data)} bytes over LLP 9P)\n{data.decode('utf-8', 'replace').rstrip()}")
            summary.append(f"cat={len(data)}")
        for line in a.cmd or []:
            mark = len(node.console)
            node.type_line(line)
            time.sleep(a.cmd_wait)
            out = node.console[mark:].decode("utf-8", "replace")
            print(f"--- radio console: {line}\n{out.rstrip()}")
        st = lk.st
        print(f"llp: tx {st.tx_frames} rx {st.rx_frames} data, lost {st.rx_lost}, held for credit "
              f"{st.tx_no_credit}, crc {node._rx.crc_err}")
        if a.console:
            print("--- radio console:\n" + node.console_text())
        print(("PASS " if ok else "FAIL ") + " ".join(summary))
        return 0 if ok else 1
    finally:
        node.close()
        ser.close()


def main() -> None:
    p = argparse.ArgumentParser(prog="llp")
    sub = p.add_subparsers(dest="command", required=True)
    h = sub.add_parser("host", help="be the host for a radio on a serial port")
    h.add_argument("--port", required=True)
    h.add_argument("--arm", action="store_true", help="type `llp radio` at its console first")
    h.add_argument("--dhcp", action="store_true", help="take a lease with the radio's MAC")
    h.add_argument("--ping", type=int, default=0, metavar="N", help="ping the gateway N times (needs --dhcp)")
    h.add_argument("--cat", action="append", metavar="PATH", help="read a file from the radio over 9P")
    h.add_argument("--cmd", action="append", metavar="LINE", help="type LINE at the radio's console at the end")
    h.add_argument("--cmd-wait", type=float, default=1.5, help="seconds to collect each --cmd's output")
    h.add_argument("--timeout", type=float, default=10.0)
    h.add_argument("--echo", action="store_true", help="echo the radio's console as it arrives")
    h.add_argument("--console", action="store_true", help="print the radio's console at the end")
    a = p.parse_args()
    if a.command == "host":
        sys.exit(cmd_host(a))


if __name__ == "__main__":
    main()
