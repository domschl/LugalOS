import sys, time, json, serial
import rp2350

def connect(retries=25):
    for _ in range(retries):
        try:
            p = rp2350.discover_ports()
            if p and p.console:
                return serial.Serial(p.console, 115200, timeout=1)
        except Exception:
            pass
        time.sleep(1.0)
    raise SystemExit("no console")

def run(cmds):
    s = connect()
    s.write(b'\x03'); s.flush(); time.sleep(0.8); rp2350.drain(s)
    try:
        for cmd, wait in cmds:
            s.write((cmd + '\n').encode()); s.flush(); time.sleep(wait)
            print(f"##### {cmd}")
            try:
                print(rp2350.drain(s, quiet=0.5, deadline=wait+5.0).decode(errors='replace'))
            except Exception as e:
                print("  (port dropped: %s)" % e)
                try: s.close()
                except Exception: pass
                time.sleep(4.0); s = connect()
                s.write(b'\x03'); s.flush(); time.sleep(0.8); rp2350.drain(s)
    finally:
        try: s.close()
        except Exception: pass

if __name__ == "__main__":
    run(json.loads(sys.argv[1]))
