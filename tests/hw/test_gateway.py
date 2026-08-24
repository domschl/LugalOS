#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for the RP2350 **gateway** persona over
real Ethernet (N6, plan/phase18_networking_and_auth.md).

This is the sibling of test_rp2350.py and follows its conventions: every test
returns (name, ok, detail), and everything *skips* rather than fails when no
gateway answers, so it is safe to run speculatively and safe to leave out of
CI. What makes it worth having separately is that none of it can run on QEMU
or over a localhost socket: the transport is a W5500 terminating TCP in
silicon, and the failures it has actually produced -- a link that reports UP
and carries nothing, a read pointer that runs away, a session that dies on the
first reply larger than one segment -- are all invisible to a loopback test.

Usage:
    uv run test_gateway.py                          # auto: 192.168.77.2, key from --key-file
    uv run test_gateway.py --host 192.168.77.2 --key 000102...0f
    uv run test_gateway.py --console /dev/ttyACM0   # also check the driver's counters

The board needs an address and a key for this boot:

    lsh> (net-config "192.168.77.2" "255.255.255.0")
    lsh> p9key 000102030405060708090a0b0c0d0e0f

and, for the two-hop test, the downlink mounted (see the plan's N5):

    lsh> (mount-remote "chess" "uart1")
"""

from __future__ import annotations

import argparse
import shutil
import socket
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
sys.path.insert(0, str(REPO_ROOT / "host" / "fuse-p9" / "src"))

import p9lib  # noqa: E402
from p9lib import Session, connect_tcp  # noqa: E402

DEFAULT_HOST = "192.168.77.2"
DEFAULT_PORT = 564


@dataclass
class Gateway:
    host: str
    port: int
    key: bytes | None
    console: str | None
    interactive: bool = False


def session(gw: Gateway, timeout: float = 20.0) -> Session:
    """A fresh authenticated session. Deliberately fresh per test rather than
    shared: a session that dies takes its successors with it, and a suite that
    reports one root cause as eight failures is worse than useless."""
    return Session(connect_tcp(gw.host, gw.port, timeout=timeout), key=gw.key)


def reachable(gw: Gateway) -> bool:
    try:
        s = socket.create_connection((gw.host, gw.port), timeout=6.0)
        s.close()
        return True
    except OSError:
        return False


# --- the tests ----------------------------------------------------------


def test_icmp(gw: Gateway) -> tuple[str, bool, str]:
    """The W5500 answers ping in silicon, with no firmware involved at all.

    First test for a reason: it separates "the board is off, unconfigured or
    unplugged" from "the 9P server is not answering", and those have entirely
    different causes. It is also the measurement that found this phase's real
    fault -- a PHY browning out for want of a bulk capacitor answered no ICMP
    while every register in the chip read correct."""
    name = "icmp / the chip's own stack"
    if shutil.which("ping") is None:
        return name, True, "SKIPPED (no ping binary)"
    r = subprocess.run(["ping", "-c", "3", "-W", "2", gw.host],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return name, False, f"no ICMP reply from {gw.host}:\n{r.stdout.strip()[-300:]}"
    rtt = ""
    for line in r.stdout.splitlines():
        if "rtt" in line or "round-trip" in line:
            rtt = line.strip()
    return name, True, rtt or "3/3"


def test_auth_required(gw: Gateway) -> tuple[str, bool, str]:
    """An attach with no afid must be refused on this link.

    This is the whole point of N2 on the only wire in the tree that is a
    network. A regression here does not look like a failure -- it looks like
    everything working, for anyone at all."""
    name = "auth: unauthenticated attach refused"
    # connect_tcp() already returns a P9Client -- it is the client, not the
    # socket underneath it.
    client = connect_tcp(gw.host, gw.port, timeout=20.0)
    try:
        client.version()
        try:
            client.attach(1, aname="/", uname="lugal")
        except p9lib.P9Error as e:
            if "auth" in str(e).lower():
                return name, True, str(e)[:80]
            return name, False, f"refused, but not for authentication: {e}"
        return name, False, "attach succeeded WITHOUT authentication -- the gate is open"
    finally:
        client.close()


def test_auth_wrong_key(gw: Gateway) -> tuple[str, bool, str]:
    """A wrong key must fail closed, and must fail at Tauth rather than
    somewhere later with a confusing error."""
    name = "auth: wrong key refused"
    if gw.key is None:
        return name, True, "SKIPPED (no key given)"
    wrong = bytes((b + 1) & 0xFF for b in gw.key)
    try:
        s = Session(connect_tcp(gw.host, gw.port, timeout=20.0), key=wrong)
    except Exception as e:
        return name, True, f"{type(e).__name__}: {str(e)[:70]}"
    try:
        s.read("/proc/version")
    except Exception as e:
        return name, True, f"rejected at first use: {str(e)[:70]}"
    finally:
        try:
            s.close()
        except Exception:
            pass
    return name, False, "a wrong key was accepted"


def test_auth_ok(gw: Gateway) -> tuple[str, bool, str]:
    name = "auth: correct key attaches"
    s = session(gw)
    try:
        v = s.read("/proc/version").decode(errors="replace").strip()
        if "LugalOS" not in v:
            return name, False, f"unexpected /proc/version: {v[:80]!r}"
        return name, True, v
    finally:
        s.close()


def test_directory_reads(gw: Gateway) -> tuple[str, bool, str]:
    """Directory reads specifically, because this is the shape of reply that
    broke first: a Tread answered with several hundred bytes of stat entries
    rather than the few dozen a /proc file returns. Under the phase's power
    fault it reset the connection every time while single-file reads kept
    working, which is exactly the sort of size-dependent failure a loopback
    socket cannot reproduce."""
    name = "directory reads (multi-entry Rread)"
    s = session(gw)
    try:
        found = {}
        for path in ("/", "/proc", "/dev"):
            found[path] = [e.name for e in s.listdir(path)]
        if "proc" not in found["/"]:
            return name, False, f"/ has no proc: {found['/']}"
        if "version" not in found["/proc"]:
            return name, False, f"/proc has no version: {found['/proc']}"
        return name, True, f"/ has {len(found['/'])} entries, /proc {len(found['/proc'])}"
    finally:
        s.close()


def test_sd_write_readback(gw: Gateway) -> tuple[str, bool, str]:
    """Create, write, read back, and remove on the gateway's own SD card,
    over Ethernet. A write is the direction that has never been exercised on
    this transport -- everything before N6 read."""
    name = "sd0: create, write, read back, remove"
    s = session(gw, timeout=40.0)
    fname = f"/sd0/N6{uuid.uuid4().hex[:6].upper()}.TXT"
    payload = f"n6 {uuid.uuid4()}\n".encode()
    try:
        if "sd0" not in [e.name for e in s.listdir("/")]:
            return name, True, "SKIPPED (no /sd0 on this board)"
        s.write(fname, payload)
        back = s.read(fname)
        if back != payload:
            return name, False, f"read back {back!r}, wrote {payload!r}"
        names = [e.name for e in s.listdir("/sd0")]
        if Path(fname).name not in names:
            return name, False, f"{fname} not listed in /sd0: {names}"
        s.remove(fname)
        if Path(fname).name in [e.name for e in s.listdir("/sd0")]:
            return name, False, f"{fname} still listed after remove"
        return name, True, f"{len(payload)} B round-tripped and removed"
    except p9lib.P9Error as e:
        return name, False, f"{fname}: {e}"
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_sd_mkdir(gw: Gateway) -> tuple[str, bool, str]:
    """mkdir, then a file inside it, then remove both.

    Tcreate with DMDIR is a different server path from Tcreate for a file,
    and on a FAT32 card it is a different on-disk operation too -- a
    directory entry plus an allocated cluster with . and .. in it. Nothing
    before N6 created a directory over any transport, let alone this one."""
    name = "sd0: mkdir, populate, remove"
    s = session(gw, timeout=40.0)
    d = f"/sd0/N6D{uuid.uuid4().hex[:4].upper()}"
    try:
        if "sd0" not in [e.name for e in s.listdir("/")]:
            return name, True, "SKIPPED (no /sd0 on this board)"
        s.mkdir(d)
        st = s.stat(d)
        if not st.is_dir:
            return name, False, f"{d} was created but does not stat as a directory"
        if Path(d).name not in [e.name for e in s.listdir("/sd0")]:
            return name, False, f"{d} not listed in /sd0 after mkdir"
        inner = f"{d}/INNER.TXT"
        body = b"inside a directory made over ethernet\n"
        s.write(inner, body)
        if s.read(inner) != body:
            return name, False, f"{inner} did not round-trip"
        if "INNER.TXT" not in [e.name for e in s.listdir(d)]:
            return name, False, f"INNER.TXT not listed in {d}"
        s.remove(inner)
        s.remove(d)
        if Path(d).name in [e.name for e in s.listdir("/sd0")]:
            return name, False, f"{d} still listed after remove"
        return name, True, f"{d} created, populated, listed and removed"
    except p9lib.P9Error as e:
        return name, False, f"{d}: {e}"
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_two_hop_write(gw: Gateway) -> tuple[str, bool, str]:
    """A write to the *far* board's SD card, from the host, over Ethernet and
    then down UART1.

    The plan's own N6 verification asks for exactly this: the same operations
    against the gateway's card and then through the gateway to the board's
    namespace. It is the first thing in this tree where one Twrite crosses two
    transports and an auth gate, and where the gateway is simultaneously a 9P
    server (to the host) and a 9P client (to the board) inside one request."""
    name = "two hops: write to /chess/sd0 and read it back"
    s = session(gw, timeout=60.0)
    fname = f"/chess/sd0/N6X{uuid.uuid4().hex[:4].upper()}.TXT"
    body = f"two hops {uuid.uuid4()}\n".encode()
    try:
        if "chess" not in [e.name for e in s.listdir("/")]:
            return name, True, 'SKIPPED (no /chess -- run (mount-remote "chess" "uart1"))'
        if "sd0" not in [e.name for e in s.listdir("/chess")]:
            return name, True, "SKIPPED (no /chess/sd0 -- the far board has no card)"
        s.write(fname, body)
        back = s.read(fname)
        if back != body:
            return name, False, f"read back {back!r}, wrote {body!r}"
        if Path(fname).name not in [e.name for e in s.listdir("/chess/sd0")]:
            return name, False, f"{fname} not listed on the far board"
        s.remove(fname)
        return name, True, f"{len(body)} B written and re-read across two transports"
    except p9lib.P9Error as e:
        return name, False, f"{fname}: {e}"
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_large_transfer(gw: Gateway) -> tuple[str, bool, str]:
    """A payload several times the negotiated msize, so both directions have
    to span multiple frames and multiple TCP segments.

    This is the test that would have caught the read pointer running away:
    the symptom there was byte counts that grew far faster than the data, and
    a single small file never showed it."""
    name = "multi-frame transfer (> msize, both directions)"
    s = session(gw, timeout=60.0)
    fname = f"/sd0/N6BIG{uuid.uuid4().hex[:4].upper()}.BIN"
    # Not random: a positional pattern means a mismatch says *where* it went
    # wrong, which distinguishes a truncation from a duplicated frame.
    body = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(8192))
    try:
        if "sd0" not in [e.name for e in s.listdir("/")]:
            return name, True, "SKIPPED (no /sd0 on this board)"
        t0 = time.time()
        s.write(fname, body)
        back = s.read(fname)
        dt = time.time() - t0
        if len(back) != len(body):
            return name, False, f"wrote {len(body)} B, read back {len(back)} B"
        if back != body:
            first = next(i for i in range(len(body)) if back[i] != body[i])
            return name, False, f"content differs from byte {first}"
        s.remove(fname)
        kbps = (2 * len(body) / 1024.0) / dt if dt else 0
        return name, True, f"{len(body)} B each way in {dt:.1f}s ({kbps:.1f} KB/s)"
    except p9lib.P9Error as e:
        return name, False, f"{fname}: {e}"
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_reconnect(gw: Gateway) -> tuple[str, bool, str]:
    """Five sessions in a row, each closed properly.

    The socket has to return to LISTEN and accept again, which on this driver
    is the whole of its connection management: notice CLOSE_WAIT, disconnect,
    re-listen. It got that wrong once in a way that reconfigured the chip's
    buffer map on every pass and took ICMP down with it."""
    name = "reconnect x5 (socket returns to LISTEN)"
    for i in range(5):
        try:
            s = session(gw)
            s.read("/proc/version")
            s.close()
        except Exception as e:
            return name, False, f"session {i + 1} of 5 failed: {type(e).__name__}: {e}"
    return name, True, "5/5"


def test_abrupt_disconnect(gw: Gateway) -> tuple[str, bool, str]:
    """Drop a connection without clunking anything, then attach again.

    A client that vanishes mid-session is the normal case on a network, not
    the exceptional one -- laptops sleep and cables get kicked. The server has
    to reclaim the fids and the socket without being asked politely."""
    name = "abrupt disconnect, then reconnect"
    try:
        s = session(gw)
        s.read("/proc/version")
        # Straight down to the socket: no Tclunk, no Rflush, nothing.
        s.client._sock.close()  # noqa: SLF001
    except Exception as e:
        return name, False, f"could not set up the abrupt case: {e}"
    time.sleep(2.0)
    try:
        s2 = session(gw, timeout=30.0)
        v = s2.read("/proc/version").decode(errors="replace").strip()
        s2.close()
        return name, True, f"recovered: {v[:40]}"
    except Exception as e:
        return name, False, f"server did not recover from an abrupt drop: {type(e).__name__}: {e}"


def test_pipelined_fill(gw: Gateway) -> tuple[str, bool, str]:
    """Send several requests without reading any reply, so the chip's 8 KB TX
    buffer fills while the server is still writing into it.

    This is the one failure mode a localhost socket genuinely cannot produce:
    the kernel's loopback buffer is large and elastic, while the W5500's is
    8 KB of on-chip RAM with a hardware write pointer. It drives
    w5500_send_locked() into its free_space == 0 branch -- the path that
    exists for "the peer has stopped reading" and that, if it got the pointer
    arithmetic wrong, would corrupt the stream rather than stall it.

    It also exercises the server as a pipelined peer, which the driver's own
    comment calls legal even though this client normally is not: several
    requests arrive in one TCP segment and have to be split by length prefix
    rather than by arrival."""
    name = "pipelined requests fill the chip's TX buffer"
    from p9lib.client import _frame, TREAD, RREAD, TSTAT, OREAD  # noqa: PLC0415

    s = session(gw, timeout=40.0)
    try:
        fid = 40
        s.client.walk(s._ROOT_FID, fid, ["proc", "version"])  # noqa: SLF001
        s.client.open(fid, mode=OREAD)
        sock = s.client._sock  # noqa: SLF001

        import struct  # noqa: PLC0415
        n = 12
        # Interleave Tread and Tstat so the replies differ in size and a
        # mismatched one cannot be confused with its neighbour.
        sent = []
        for i in range(n):
            tag = 900 + i
            if i % 2 == 0:
                body = struct.pack("<IQI", fid, 0, 2000)
                sock.sendall(_frame(TREAD, tag, body))
                sent.append((tag, TREAD))
            else:
                sock.sendall(_frame(TSTAT, tag, struct.pack("<I", fid)))
                sent.append((tag, TSTAT))
        # No reads at all until every request is out: that is what fills the
        # buffer. A short pause makes sure the server has drained its input
        # and is genuinely blocked on TX space rather than just behind.
        time.sleep(1.5)

        seen = {}
        for _ in range(n):
            rtype, rtag, rbody = s.client._recv_one_frame()  # noqa: SLF001
            seen[rtag] = (rtype, rbody)

        missing = [t for t, _ in sent if t not in seen]
        if missing:
            return name, False, f"{len(missing)} of {n} replies never arrived: tags {missing}"
        for tag, ttype in sent:
            rtype, rbody = seen[tag]
            want = RREAD if ttype == TREAD else 125  # RSTAT
            if rtype != want:
                return name, False, f"tag {tag}: expected reply type {want}, got {rtype}"
            if ttype == TREAD:
                (count,) = struct.unpack_from("<I", rbody, 0)
                if b"LugalOS" not in rbody[4:4 + count]:
                    return name, False, f"tag {tag}: Rread payload is not /proc/version"
        s.client.clunk(fid)
        return name, True, f"{n} pipelined requests, {n} correct replies, none lost"
    except Exception as e:
        return name, False, f"{type(e).__name__}: {e}"
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_cable_pull(gw: Gateway) -> tuple[str, bool, str]:
    """Pull the cable mid-session and plug it back in. Opt-in, because it
    needs hands.

    Worth having as a test rather than a note: link loss is the one event
    where the chip's state and the driver's diverge silently, and the recovery
    path -- re-configure the MAC on link-up -- was added late and is the kind
    of thing that rots unnoticed. `--interactive` is the whole gate."""
    name = "cable pulled mid-session, then restored"
    if not gw.interactive:
        return name, True, "SKIPPED (needs hands -- pass --interactive)"
    s = session(gw)
    try:
        s.read("/proc/version")
    except Exception as e:
        return name, False, f"could not establish a session first: {e}"
    print("\n    >>> Unplug the Ethernet cable from the W5500 now.", flush=True)
    deadline = time.time() + 60
    while time.time() < deadline and reachable(gw):
        time.sleep(1.0)
    if reachable(gw):
        return name, False, "still reachable after 60 s -- was the cable pulled?"
    print("    >>> Cable loss seen. Plug it back in.", flush=True)
    deadline = time.time() + 90
    while time.time() < deadline and not reachable(gw):
        time.sleep(1.0)
    if not reachable(gw):
        return name, False, "did not recover within 90 s of the cable being restored"
    try:
        s2 = session(gw, timeout=30.0)
        v = s2.read("/proc/version").decode(errors="replace").strip()
        s2.close()
        return name, True, f"recovered without intervention: {v[:40]}"
    except Exception as e:
        return name, False, f"link returned but 9P did not: {type(e).__name__}: {e}"
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_two_hop(gw: Gateway) -> tuple[str, bool, str]:
    """N5 through N4: the chess board's namespace, over Ethernet, through the
    gateway, down UART1.

    Proven distinct from the gateway by /proc/config rather than by trusting
    the mount -- ENABLE_CHESS is 1 on one board and 0 on the other, so a loop
    back through the gateway would be visible rather than plausible."""
    name = "two hops: /chess through the gateway"
    s = session(gw, timeout=40.0)
    try:
        if "chess" not in [e.name for e in s.listdir("/")]:
            return name, True, 'SKIPPED (no /chess -- run (mount-remote "chess" "uart1"))'
        remote = [e.name for e in s.listdir("/chess")]
        if "proc" not in remote:
            return name, False, f"/chess has no proc: {remote}"
        near = s.read("/proc/config").decode(errors="replace")
        far = s.read("/chess/proc/config").decode(errors="replace")
        if "ENABLE_CHESS=1" not in far:
            return name, False, "/chess/proc/config does not report ENABLE_CHESS=1"
        if "ENABLE_CHESS=1" in near:
            return name, False, "the gateway itself reports ENABLE_CHESS=1 -- wrong persona?"
        ver = s.read("/chess/proc/version").decode(errors="replace").strip()
        return name, True, f"{ver[:40]} ({len(remote)} entries)"
    except p9lib.P9Error as e:
        return name, False, str(e)
    finally:
        try:
            s.close()
        except Exception:
            pass


def test_fuse_mount(gw: Gateway) -> tuple[str, bool, str]:
    """lugal9pfuse over TCP: the board's namespace as a real host directory.

    Runs the CLI as a subprocess rather than importing it, because that is how
    a user meets it -- an argument-parsing or dependency bug that only shows up
    from the command line is exactly the kind this should catch."""
    name = "lugal9pfuse over TCP (ls, cat, write)"
    try:
        import fuse  # noqa: F401
    except Exception:
        return name, True, "SKIPPED (fusepy not installed)"
    if not Path("/dev/fuse").exists():
        return name, True, "SKIPPED (no /dev/fuse)"

    import tempfile
    mnt = Path(tempfile.mkdtemp(prefix="lugal9p-n6-"))
    keyfile = mnt.parent / f"n6key-{uuid.uuid4().hex[:6]}"
    proc = None
    try:
        extra: list[str] = []
        if gw.key is not None:
            keyfile.write_text(gw.key.hex())
            extra = ["--key-file", str(keyfile)]
        # Flags go after the module name, not before it: anything ahead of -m
        # is parsed by the interpreter, not by the program.
        cmd = [sys.executable, "-m", "fuse_p9.cli", *extra,
               "--tcp", f"{gw.host}:{gw.port}", str(mnt)]
        env = {"PYTHONPATH": str(REPO_ROOT / "host" / "p9lib" / "src") + ":"
                             + str(REPO_ROOT / "host" / "fuse-p9" / "src")}
        import os
        env = {**os.environ, **env}
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=env)

        deadline = time.time() + 30.0
        while time.time() < deadline:
            if proc.poll() is not None:
                return name, False, f"mount exited: {(proc.stdout.read() or '')[-300:]}"
            try:
                if (mnt / "proc").is_dir():
                    break
            except OSError:
                pass
            time.sleep(0.5)
        else:
            return name, False, "mount did not become usable within 30 s"

        entries = sorted(p.name for p in mnt.iterdir())
        version = (mnt / "proc" / "version").read_text(errors="replace").strip()
        detail = f"{len(entries)} entries, {version[:34]}"

        # A write through the FUSE layer, if there is a card to write to.
        if (mnt / "sd0").is_dir():
            f = mnt / "sd0" / f"N6F{uuid.uuid4().hex[:5].upper()}.TXT"
            body = f"fuse {uuid.uuid4()}\n"
            f.write_text(body)
            back = f.read_text(errors="replace")
            if back != body:
                return name, False, f"FUSE write/read mismatch: {back!r} != {body!r}"
            f.unlink()
            detail += ", sd0 write round-trip OK"
        return name, True, detail
    except Exception as e:
        return name, False, f"{type(e).__name__}: {e}"
    finally:
        if proc is not None and proc.poll() is None:
            subprocess.run(["fusermount", "-u", str(mnt)], capture_output=True)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        keyfile.unlink(missing_ok=True)
        try:
            mnt.rmdir()
        except OSError:
            pass


def test_driver_counters(gw: Gateway) -> tuple[str, bool, str]:
    """After everything above, ask the driver what it saw.

    The counters are the test: resync discards, command timeouts and RX
    overruns should all be zero on a healthy link, and every one of them was
    non-zero during this phase's fault while the session-level results still
    looked plausible. Needs the console, so it skips without one."""
    name = "driver counters clean after the suite"
    if gw.console is None:
        return name, True, "SKIPPED (no --console given)"
    try:
        import serial
    except Exception:
        return name, True, "SKIPPED (pyserial not available)"
    try:
        ser = serial.Serial(gw.console, 115200, timeout=0.3)
    except Exception as e:
        return name, True, f"SKIPPED ({gw.console}: {e})"
    try:
        time.sleep(0.8)
        ser.reset_input_buffer()
        ser.write(b"net\r\n")
        ser.flush()
        end, buf = time.time() + 6.0, b""
        while time.time() < end:
            buf += ser.read(4096)
        out = buf.decode("utf-8", "replace")
    finally:
        ser.close()

    if "W5500" not in out:
        return name, True, "SKIPPED (console did not answer `net`)"
    bad = []
    for line in out.splitlines():
        line = line.strip()
        if "resync discards" in line or "command timeouts" in line:
            for field, label in ((" resync discards", "resync discards"),
                                 (" command timeouts", "command timeouts")):
                if field in line:
                    n = line.split(field)[0].split()[-1]
                    if n != "0":
                        bad.append(f"{label}={n}")
        if "rx overruns:" in line:
            n = line.split("rx overruns:")[1].split(",")[0].strip()
            if n != "0":
                bad.append(f"rx overruns={n}")
        if "DISAGREES" in line:
            bad.append("a chip register disagrees with what the driver set")
    link = next((l.strip() for l in out.splitlines() if l.strip().startswith("link")), "")
    if bad:
        return name, False, f"{', '.join(bad)}  ({link})"
    return name, True, link or "all counters zero"


# --- runner -------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"gateway address (default {DEFAULT_HOST})")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"9P port (default {DEFAULT_PORT})")
    ap.add_argument("--key", help="pre-shared key, hex")
    ap.add_argument("--key-file", help="read the hex key from a file instead")
    ap.add_argument("--console", help="gateway console port, to check the driver's counters")
    ap.add_argument("--interactive", action="store_true",
                    help="include tests that need hands (pulling the Ethernet cable)")
    args = ap.parse_args()

    key = None
    if args.key_file:
        key = bytes.fromhex(Path(args.key_file).read_text().strip())
    elif args.key:
        key = bytes.fromhex(args.key.strip())

    gw = Gateway(host=args.host, port=args.port, key=key, console=args.console,
                 interactive=args.interactive)

    print("======================================================================")
    print("        LugalOS Gateway Hardware-in-the-Loop Suite (N6, Ethernet)")
    print("======================================================================")

    if not reachable(gw):
        print(f"\n[!] No 9P server answering at {gw.host}:{gw.port}.")
        print("    Nothing to test -- this is not a failure, just nothing to do.")
        print('    On the board: (net-config "192.168.77.2" "255.255.255.0") and p9key <hex>.')
        return 0

    print(f"\nGateway at {gw.host}:{gw.port}, key {'given' if key else 'NOT given'}"
          f"{', console ' + gw.console if gw.console else ''}")

    tests = [
        test_icmp,
        test_auth_required,
        test_auth_wrong_key,
        test_auth_ok,
        test_directory_reads,
        test_sd_write_readback,
        test_sd_mkdir,
        test_large_transfer,
        test_reconnect,
        test_abrupt_disconnect,
        test_pipelined_fill,
        test_cable_pull,
        test_two_hop,
        test_two_hop_write,
        test_fuse_mount,
        # Last: it reads the counters accumulated by everything above.
        test_driver_counters,
    ]

    total = passed = 0
    for t in tests:
        try:
            name, ok, log = t(gw)
        except Exception as e:  # a test that throws is a failed test, not a dead suite
            name, ok, log = t.__name__, False, f"raised {type(e).__name__}: {e}"
        total += 1
        if ok:
            passed += 1
            status = "SKIP" if log.startswith("SKIPPED") else "PASS"
            print(f"  [{status}] {name}" + (f" -- {log}" if log else ""))
        else:
            print(f"  [FAIL] {name}\n    {log}")

    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed} / {total} PASSED")
    print("======================================================================\n")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
