#!/usr/bin/env python3
"""Prove R5's netif over the air: an authenticated 9P session to a board
that is on the network via WiFi, not a wire.

The gateway suite (test_gateway.py) already covers 9P-over-TCP in depth;
this deliberately does not repeat it. What is new here is the transport
underneath -- PIO gSPI, an uploaded firmware blob, WPA2 -- so this asks the
questions that distinguish "the radio carries frames" from "the radio
carries a few frames":

  * ICMP at all, and under a burst
  * an authenticated 9P session (auth is the same gate as on the wire)
  * a directory read, which is the multi-hundred-byte reply shape that
    breaks first when a transport is marginal
  * a sustained soak, because a radio that works for a second and a radio
    that works for fifteen minutes are different claims
  * with --update BUILD_DIR: a whole firmware image pushed to the board over
    the air. This is the one test that has the board *receive* a megabyte of
    TCP; reads and pings do not (a bus transfer that an interrupt interrupts
    corrupts the chip's receive FIFO, and only sustained receive finds it)

Run it against a board that has already been brought up by hand:

    lsh> wifi probe
    lsh> wifi join <ssid> <psk>
    lsh> lisp
         (net-config "192.168.178.21" "255.255.255.0" "192.168.178.1")
    lsh> p9key 000102030405060708090a0b0c0d0e0f
    lsh> net listen 564

    uv run test_wifi.py 192.168.178.21 [--soak-minutes 15]

Or store the credentials once (`wlan <ssid> <psk-hex>`, the PSK derived on
the host as doc/admin/identity.md shows): the board then joins, takes a
DHCP lease and listens on 9P by itself at every boot, and only the
per-boot `p9key` above remains to type.

There is no console interaction here on purpose: everything below goes over
the air, so a pass means the network path worked, not that a serial cable
did.

Setup mode (doc/admin/identity.md, ADR 006). On a claimed node, always run and harmless: the
board facts are public (`GET /board`), `/dev/setup` says `claimed`, a claim off the setup network is
refused over TCP and over the WebSocket, and neither a secret (`wlan` into `/dev/owner`) nor an owner
reset goes over a link anyone on the LAN can read.

With --claim-cycle the phone is played by this machine, end to end. **It wipes the node's owner part**
(its grants, the owner key it was claimed with, its Wi-Fi), so it is never run without the flag:

    uv run test_wifi.py 192.168.178.171 --key HEX --claim-cycle \
        --console /dev/serial/by-id/usb-LugalOS_...-if00 --wifi-iface wlp6s0

It stores a fresh setup secret and resets the owner at the console, opens the setup window, joins the
node's access point (LugalOS-XXXX) on the given spare Wi-Fi interface (NetworkManager; the profile
takes no DNS and no default route from the node, so the host's own network is untouched), refuses a
wrong secret, claims with a new owner key and the Wi-Fi in ~/.config/lugalos/wifi.env, says
`/setup-done`, and waits for the node back on the home network, claimed by the new key. The new key
is written to ~/.config/lugalos/keys/NAME.p9key (mode 0600) and never printed.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import os
import re
import secrets
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))

import p9lib  # noqa: E402
from p9lib import Session, connect_tcp, connect_ws  # noqa: E402

DEFAULT_PORT = 564
DEFAULT_KEY = "000102030405060708090a0b0c0d0e0f"


class Board:
    def __init__(self, host: str, port: int, key: str):
        self.host = host
        self.port = port
        # p9lib wants the raw key, not its hex spelling.
        self.key = bytes.fromhex(key)


def session(b: Board, timeout: float = 20.0) -> Session:
    return Session(connect_tcp(b.host, b.port, timeout=timeout), key=b.key)


def ping(host: str, count: int, interval: str = "0.3") -> tuple[int, str]:
    """Returns (loss_percent, summary_line)."""
    r = subprocess.run(
        ["ping", "-c", str(count), "-i", interval, "-W", "2000", host],
        capture_output=True, text=True,
    )
    loss, rtt = 100, ""
    for line in r.stdout.splitlines():
        if "packet loss" in line:
            try:
                loss = int(float(line.split("%")[0].split()[-1]))
            except (ValueError, IndexError):
                pass
        if "round-trip" in line or "rtt" in line:
            rtt = line.strip()
    return loss, rtt


def test_icmp(b: Board) -> tuple[str, bool, str]:
    name = "icmp: 20 echoes"
    loss, rtt = ping(b.host, 20)
    if loss != 0:
        return name, False, f"{loss}% loss"
    return name, True, rtt or "20/20"


def test_auth_required(b: Board) -> tuple[str, bool, str]:
    """The auth gate is not transport-specific, but a driver that mangles
    payloads could make a refusal look like a success. Cheap to confirm."""
    name = "auth: unauthenticated attach refused"
    c = connect_tcp(b.host, b.port, timeout=20.0)
    try:
        c.version()
        try:
            c.attach(1, aname="/", uname="lugal")
        except p9lib.P9Error as e:
            if "auth" in str(e).lower():
                return name, True, str(e)[:80]
            return name, False, f"refused, but not for authentication: {e}"
        return name, False, "attach succeeded WITHOUT authentication"
    finally:
        c.close()


def test_9p_read(b: Board) -> tuple[str, bool, str]:
    name = "9p: authenticated session, /proc/version"
    s = session(b)
    try:
        v = s.read("/proc/version").decode(errors="replace").strip()
        if "LugalOS" not in v:
            return name, False, f"unexpected /proc/version: {v[:80]!r}"
        return name, True, v
    finally:
        s.close()


def test_directory_read(b: Board) -> tuple[str, bool, str]:
    """Several hundred bytes of stat entries in one reply -- the size that
    exposes a transport which is fine for short messages."""
    name = "9p: directory read (multi-entry Rread)"
    s = session(b)
    try:
        root = [e.name for e in s.listdir("/")]
        proc = [e.name for e in s.listdir("/proc")]
        if "proc" not in root:
            return name, False, f"/ has no proc: {root}"
        if "version" not in proc:
            return name, False, f"/proc has no version: {proc}"
        return name, True, f"/ has {len(root)} entries, /proc {len(proc)}"
    finally:
        s.close()


def test_repeated_sessions(b: Board) -> tuple[str, bool, str]:
    """Ten sessions in a row. A transport that leaks a buffer or loses a
    reply usually survives one and not ten.

    Paced deliberately. net/tcp.c keeps TCP_MAX_CONNS = 2 slots and holds a
    closed connection in TIME_WAIT for TCP_TIME_WAIT_MS = 2000 ms, so
    hammering connect() in a loop exhausts both slots and the board
    correctly refuses -- which is the stack behaving as designed, not the
    radio failing. An earlier version of this test did exactly that and
    reported a ConnectionRefusedError as a transport fault."""
    name = "9p: 10 consecutive sessions"
    gap = 2.2   # just over TIME_WAIT, so a slot is always free
    for i in range(10):
        try:
            s = session(b)
        except ConnectionRefusedError:
            # Both slots still in TIME_WAIT; give them a moment rather
            # than calling it a failure.
            time.sleep(gap)
            s = session(b)
        try:
            s.read("/proc/version")
        except Exception as e:  # noqa: BLE001
            return name, False, f"session {i + 1} failed: {e}"
        finally:
            s.close()
        if i < 9:
            time.sleep(gap)
    return name, True, "10/10, paced past TIME_WAIT"


def http_get(host: str, path: str, port: int = 80, timeout: float = 10.0,
             host_header: str | None = None) -> tuple[int, str]:
    c = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        c.request("GET", path, headers={"Host": host_header or host})
        r = c.getresponse()
        return r.status, r.read(8192).decode(errors="replace")
    finally:
        c.close()


def kv(text: str) -> dict[str, str]:
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def test_board_public(b: Board) -> tuple[str, bool, str]:
    """Anyone may read the board's public facts, before signing in; nothing else."""
    name = "setup: GET /board answers without credentials"
    try:
        status, body = http_get(b.host, "/board")
    except OSError as e:
        return name, True, f"skipped: no web server on port 80 ({e})"
    facts = kv(body)
    if status != 200 or "board" not in facts:
        return name, False, f"HTTP {status}: {body[:80]!r}"
    return name, True, f"board={facts['board']}"


def test_setup_state(b: Board) -> tuple[str, bool, str]:
    name = "setup: /dev/setup says claimed"
    s = session(b)
    try:
        st = kv(s.read("/dev/setup").decode(errors="replace"))
    finally:
        s.close()
    if st.get("state") != "claimed":
        return name, False, f"state={st.get('state')!r}: this suite expects a provisioned node"
    return name, True, f"failed_attempts={st.get('failed_attempts')}"


def _unauth_setup_attach(c: p9lib.P9Client) -> str | None:
    """None if an unauthenticated attach to /dev/setup was refused, else what happened."""
    try:
        c.version()
        try:
            c.attach(1, aname="/dev/setup", uname="setup")
        except p9lib.P9Error:
            return None
        return "attach to /dev/setup succeeded WITHOUT authentication"
    finally:
        c.close()


def test_claim_refused_off_ap(b: Board) -> tuple[str, bool, str]:
    """The unauthenticated attach that claims a node exists only on its setup access point, and only while
    unclaimed: on the home network it is refused over TCP and over the admin page's WebSocket."""
    name = "setup: no claim off the setup network (TCP, WebSocket)"
    bad = _unauth_setup_attach(connect_tcp(b.host, b.port, timeout=20.0, protect=False))
    if bad:
        return name, False, "TCP: " + bad
    try:
        ws = connect_ws(b.host, timeout=20.0, protect=False)
    except OSError as e:
        return name, True, f"TCP refused; WebSocket skipped (no web server: {e})"
    bad = _unauth_setup_attach(ws)
    if bad:
        return name, False, "WebSocket: " + bad
    return name, True, "refused on both"


def test_no_secrets_on_plain_links(b: Board) -> tuple[str, bool, str]:
    """A Wi-Fi key and an owner reset do not cross a link the LAN can read, protected or not: the console,
    a cable, TLS or the setup access point only. The owner part is unchanged afterwards."""
    name = "setup: no Wi-Fi key or owner reset over plain 9P"
    s = session(b)
    try:
        before = kv(s.read("/dev/setup").decode(errors="replace")).get("state")
        refused = []
        for path, line in (("/dev/owner", "wlan test-ssid " + "00" * 32), ("/dev/setup", "reset-owner")):
            try:
                s.write(path, line.encode(), create=False, truncate=False)
            except p9lib.P9Error as e:
                refused.append(f"{path}: {str(e)[:40]}")
                continue
            return name, False, f"{path} took {line.split()[0]!r} over a plain link"
        after = kv(s.read("/dev/setup").decode(errors="replace")).get("state")
    finally:
        s.close()
    if after != before:
        return name, False, f"state changed from {before} to {after}"
    return name, True, "; ".join(refused)


class NodeConsole:
    """The node's USB console (an RP2350's CDC if00: opening it does not reset the board)."""

    def __init__(self, port: str):
        import serial  # pyserial: only the claim cycle needs it
        self.ser = serial.Serial(port, 115200, timeout=0.3)
        self.ser.write(b"\r")
        time.sleep(0.3)
        self.ser.read(65536)

    def run(self, cmd: str, wait: float = 2.0) -> str:
        self.ser.write(cmd.encode() + b"\r")
        end, out = time.time() + wait, b""
        while time.time() < end:
            out += self.ser.read(4096)
        return out.decode(errors="replace")

    def close(self) -> None:
        self.ser.close()


def _wifi_credentials() -> tuple[str, str] | None:
    """(ssid, 64-hex PSK) from ~/.config/lugalos/wifi.env (wpa_supplicant.conf style); a quoted psk is a
    passphrase and is derived here. The same file test_esp32c6.py reads."""
    try:
        text = (Path.home() / ".config" / "lugalos" / "wifi.env").read_text()
    except OSError:
        return None
    ssid = re.search(r'ssid\s*=\s*"([^"]*)"', text)
    psk = re.search(r'psk\s*=\s*("[^"]*"|[0-9a-fA-F]{64})', text)
    if not ssid or not psk:
        return None
    key = psk.group(1)
    if key.startswith('"'):
        key = hashlib.pbkdf2_hmac("sha1", key.strip('"').encode(), ssid.group(1).encode(), 4096, 32).hex()
    return ssid.group(1), key


def _nmcli(*args: str, check: bool = True) -> str:
    r = subprocess.run(["nmcli", *args], capture_output=True, text=True, timeout=90)
    if check and r.returncode != 0:
        raise RuntimeError(f"nmcli {args[0]} {args[1] if len(args) > 1 else ''}: {r.stderr.strip()[:120]}")
    return r.stdout


def _setup_link(timeout: float = 20.0) -> tuple[p9lib.P9Client, int]:
    """As the setup page does it: version, an unauthenticated attach to /dev/setup, the file open."""
    c = connect_ws("192.168.4.1", timeout=timeout, protect=False)
    c.version()
    c.attach(1, aname="/dev/setup", uname="setup")
    c.walk(1, 2, [])
    c.open(2, p9lib.client.ORDWR)
    return c, 2


def _setup_claim(line: str) -> tuple[str, dict[str, str]]:
    """Writes one claim line and waits for its result (kernel/setup.c works it off on its own task)."""
    c, fid = _setup_link()
    try:
        try:
            c.write(fid, 0, line.encode())
        except p9lib.P9Error as e:
            return f"write refused: {e}", {}
        st: dict[str, str] = {}
        for _ in range(60):
            time.sleep(0.5)
            st = kv(c.read(fid, 0, 1000).decode(errors="replace"))
            if st.get("result") not in (None, "", "pending"):
                break
        return st.get("result", ""), st
    finally:
        c.close()


def test_claim_cycle(b: Board, console_port: str, iface: str) -> tuple[str, bool, str]:
    name = "setup: claimed from scratch through the setup access point (owner part WIPED)"
    cred = _wifi_credentials()
    if cred is None:
        return name, False, "no ~/.config/lugalos/wifi.env: the claim would leave the node off the network"
    con = NodeConsole(console_port)
    profile = "lugalos-setup-test"
    try:
        board = kv(con.run("cat /proc/board"))
        uid, node_name = board.get("uid", "").strip(), board.get("name", "").strip() or "lugal-node"
        if len(uid) < 4:
            return name, False, "could not read the node's uid at the console"
        ssid_ap = "LugalOS-" + uid[-4:].upper()
        secret = secrets.token_urlsafe(12)[:16]
        if "stored" not in con.run(f"setup secret {secret}"):
            return name, False, "the console did not store the setup secret"
        if "wiped" not in con.run("setup reset-owner", wait=4):
            return name, False, "the console did not reset the owner part"
        con.run("setup start")
        # The access point needs the radio reloaded in AP mode (about 30 s).
        _nmcli("connection", "delete", profile, check=False)
        _nmcli("connection", "add", "type", "wifi", "ifname", iface, "con-name", profile, "ssid", ssid_ap,
               "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", secret, "ipv4.ignore-auto-dns", "yes",
               "ipv4.never-default", "yes", "ipv6.method", "disabled", "connection.autoconnect", "no")
        joined, last = False, ""
        for _ in range(12):
            time.sleep(5)
            _nmcli("device", "wifi", "rescan", "ifname", iface, check=False)
            try:
                _nmcli("connection", "up", profile)
                joined = True
                break
            except RuntimeError as e:
                last = str(e)
        if not joined:
            return name, False, f"could not join {ssid_ap}: {last}"
        status, body = http_get("192.168.4.1", "/board")
        if status != 200 or "board" not in kv(body):
            return name, False, f"GET /board on the access point: HTTP {status}"
        res, st = _setup_claim(f"claim {secret[::-1]}x {'00' * 32}")
        if res.startswith("ok") or st.get("failed_attempts") != "1":
            return name, False, f"a wrong secret: result {res!r}, failed_attempts={st.get('failed_attempts')}"
        owner_key = secrets.token_bytes(32)
        ssid, psk = cred
        res, st = _setup_claim(f"claim {secret} {owner_key.hex()} name {node_name} wlanhex {ssid.encode().hex()} {psk}")
        if not res.startswith("ok"):
            return name, False, f"the claim: {res!r}"
        keyfile = Path.home() / ".config" / "lugalos" / "keys" / f"{node_name}.p9key"
        keyfile.parent.mkdir(parents=True, exist_ok=True)
        old = os.umask(0o077)
        try:
            keyfile.write_text(owner_key.hex() + "\n")
        finally:
            os.umask(old)
        try:
            http_get("192.168.4.1", "/setup-done", host_header="192.168.4.1")
        except OSError:
            pass  # the node may already be leaving
    finally:
        _nmcli("connection", "down", profile, check=False)
        _nmcli("connection", "delete", profile, check=False)
        con.close()
    # Back on the home network, claimed by the new key (closing the AP and joining take about 30 s).
    owner = Board(b.host, b.port, owner_key.hex())
    deadline, err = time.time() + 180, ""
    while time.time() < deadline:
        time.sleep(5)
        try:
            s = Session(connect_tcp(owner.host, owner.port, timeout=10.0), key=owner.key, uname="owner")
            try:
                st = kv(s.read("/dev/setup").decode(errors="replace"))
            finally:
                s.close()
            if st.get("state") == "claimed":
                # Now with an admin key: the refusal must be the secret rule, not a missing grant.
                s = Session(connect_tcp(owner.host, owner.port, timeout=10.0), key=owner.key, uname="owner")
                try:
                    try:
                        s.write("/dev/owner", ("wlan test-ssid " + "00" * 32).encode(), create=False, truncate=False)
                        return name, False, "claimed, but /dev/owner took a Wi-Fi key over plain 9P from the owner"
                    except p9lib.P9Error:
                        pass
                    why = kv(s.read("/dev/setup").decode(errors="replace")).get("result", "")
                finally:
                    s.close()
                if "secret" not in why:
                    return name, False, f"claimed, but the owner's Wi-Fi key was refused for another reason: {why!r}"
                return name, True, (f"wrong secret refused, claimed, back at {b.host} as owner, the owner's Wi-Fi "
                                    f"key refused over plain 9P; key in {keyfile}")
            err = f"state={st.get('state')}"
        except Exception as e:  # noqa: BLE001
            err = str(e)[:80]
    return name, False, f"the node did not come back claimed at {b.host}: {err}"


def test_update_over_wifi(b: Board, build_dir: Path) -> tuple[str, bool, str]:
    """A whole image over TCP into the inactive slot (tools/lota.py push, no reboot): it must reach
    "committed", and the radio must be unharmed after it -- no loss on a ping burst, a session that
    still answers. Pushes the board's own build, so the pending slot it leaves behind is the same
    firmware (a reboot would boot it).

    This is what failed until the bus transfer masked interrupts: 7 of 7 updates died 130-450 KB in,
    the radio silent from then on and only `wifi probe` bringing it back."""
    name = "ota: a whole image over the air, committed, radio unharmed"
    cmd = [sys.executable, str(REPO_ROOT / "tools" / "lota.py"), "push", str(build_dir),
           "--tcp", f"{b.host}:{b.port}", "--key", b.key.hex(), "--no-sign"]
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    took = time.monotonic() - t0
    out = (r.stdout + r.stderr).replace("\r", "\n")
    if r.returncode != 0 or "state=committed" not in out:
        return name, False, f"the update did not commit (exit {r.returncode}, {took:.0f} s): {out.strip()[-300:]}"
    loss, _ = ping(b.host, 20)
    if loss > 10:
        return name, False, f"committed in {took:.0f} s, but the radio then lost {loss}% of 20 echoes"
    try:
        s = session(b)
        try:
            s.read("/proc/version")
        finally:
            s.close()
    except Exception as e:  # noqa: BLE001
        return name, False, f"committed in {took:.0f} s, but the session after it failed: {e}"
    return name, True, f"committed in {took:.0f} s; {loss}% loss after"


def test_soak(b: Board, minutes: float) -> tuple[str, bool, str]:
    """R5's own criterion. Alternates ICMP and a real 9P read so that both
    the data path and the stack above it stay exercised, rather than
    proving only that the radio still associates."""
    name = f"soak: {minutes:g} minutes of traffic"
    deadline = time.time() + minutes * 60
    pings = pings_lost = sessions = failures = 0
    last_err = ""
    while time.time() < deadline:
        loss, _ = ping(b.host, 3, interval="0.2")
        pings += 3
        pings_lost += 3 * loss // 100
        try:
            s = session(b, timeout=25.0)
            try:
                s.read("/proc/version")
                sessions += 1
            finally:
                s.close()
        except Exception as e:  # noqa: BLE001
            failures += 1
            last_err = str(e)[:120]
        time.sleep(5)
    detail = (f"{pings - pings_lost}/{pings} echoes, "
              f"{sessions} sessions, {failures} failed")
    if failures or pings_lost:
        return name, False, detail + (f"; last error: {last_err}" if last_err else "")
    return name, True, detail


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="the board's IP address on the WiFi network")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--key", default=DEFAULT_KEY, help="9P auth key, as hex")
    ap.add_argument("--update", metavar="BUILD_DIR", type=Path,
                    help="also push this build over the air into the inactive slot (changes the "
                         "board's pending slot to a copy of that build)")
    ap.add_argument("--soak-minutes", type=float, default=0,
                    help="also run the sustained soak, for this many minutes")
    ap.add_argument("--claim-cycle", action="store_true",
                    help="WIPES THE OWNER PART: reset the owner at the console and claim the node again "
                         "through its setup access point (needs --console and --wifi-iface)")
    ap.add_argument("--console", help="the node's USB console (for --claim-cycle)")
    ap.add_argument("--wifi-iface", help="a spare Wi-Fi interface on this machine (for --claim-cycle)")
    args = ap.parse_args()

    b = Board(args.host, args.port, args.key)

    try:
        socket.create_connection((b.host, b.port), timeout=6.0).close()
    except OSError as e:
        print(f"[!] {b.host}:{b.port} is not accepting connections ({e}).")
        print("    On the board: wifi probe / wifi join / (net-config ...) /")
        print("                  p9key <hex> / net listen 564")
        return 1

    tests = [test_icmp, test_auth_required, test_9p_read,
             test_directory_read, test_repeated_sessions, test_board_public, test_setup_state,
             test_claim_refused_off_ap, test_no_secrets_on_plain_links]

    failed = 0
    for t in tests:
        try:
            name, ok, detail = t(b)
        except Exception as e:  # noqa: BLE001
            name, ok, detail = t.__name__, False, f"raised {type(e).__name__}: {e}"
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))
        failed += not ok

    extra = 0
    if args.update:
        extra += 1
        name, ok, detail = test_update_over_wifi(b, args.update)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} -- {detail}")
        failed += not ok

    if args.claim_cycle:
        extra += 1
        if not args.console or not args.wifi_iface:
            name, ok, detail = "setup: claim cycle", False, "needs --console and --wifi-iface"
        else:
            try:
                name, ok, detail = test_claim_cycle(b, args.console, args.wifi_iface)
            except Exception as e:  # noqa: BLE001
                name, ok, detail = "setup: claim cycle", False, f"raised {type(e).__name__}: {e}"
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} -- {detail}")
        failed += not ok

    if args.soak_minutes > 0:
        name, ok, detail = test_soak(b, args.soak_minutes)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} -- {detail}")
        failed += not ok

    total = len(tests) + extra + (1 if args.soak_minutes > 0 else 0)
    print(f"\nResult: {total - failed} / {total} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
