"""Which subsystem a source file belongs to, for the RAM ledger (phase 66.1).

Shared by tools/sizereport.py (static RAM) and tools/ramledger.py (pages at
runtime), so both group the same files under the same name. The first rule
that matches wins; a file no rule names is reported under its directory, so
a new file is never silently dropped from the totals.
"""

import re

# (subsystem, regex on the repository-relative path), first match wins.
RULES: list[tuple[str, str]] = [
    # Code that tests the system rather than being part of it. 66.2 moves it
    # behind a dev flag; until then the ledger shows what it costs.
    ("selftest", r"(selftest|_utest|stress|umode_probe|radio_osi_test|zonetest|audiotest)[^/]*\.c$"),
    ("lisp", r"^user/lisp/"),
    ("chess", r"^user/chess/"),
    ("app:moonbase", r"^user/moonbase/"),
    ("app:ceres", r"^user/ceres/"),
    ("app:zmachine", r"^user/zmachine/"),
    ("app:cc", r"^user/chibicc/"),
    ("app:ed", r"^user/ed/"),
    ("9p", r"^fs/(9p|p9_)|^drivers/(loopback_net|uart_net|slip)\.c$"),
    ("vfs", r"^fs/vfs_server\.c$"),
    ("fs", r"^fs/|^drivers/(flashdisk|ramdisk)\.c$"),
    ("crypto", r"^net/tls\.c$|^kernel/(crypto|sha256|x509|der|tls_cert|ota_sig|random)\.c$|^third_party/"),
    ("agent", r"^kernel/ea\.c$|^net/llm\.c$"),
    ("httpd", r"^net/(httpd|http|websocket|spartan)\.c$"),
    ("mqtt", r"^net/mqtt"),
    ("net", r"^net/|^drivers/(virtio_net|enc28j60_rp2350|emac_esp32p4)\.c$"),
    ("radio", r"^drivers/radio/|^kernel/radio_|^drivers/cyw43|esp-idf/|^drivers/sdio_|wifi"),
    ("llp", r"^kernel/llp"),
    ("ota", r"^kernel/ota|^drivers/ota_"),
    ("identity", r"^kernel/(identity|idstore|setup|setup_ap|setup_button|privilege)\.c$|^drivers/idstore_"),
    ("log", r"^kernel/klog"),
    ("shell", r"^kernel/(shell|line_editor|vterm|console|editor|keyseq|clipboard|discipline|path|scratch)\.c$"),
    ("display", r"^drivers/(lcdterm|lcd7_|dsi_|ppa_|reader_view|screen|ramscreen|st7735|ribbon|canvas|fbtext|font|vtterm|latex_layout|touch_)"
                r"|^fs/(markdown|gemtext|latex_math)\.c$|^kernel/(screenshot|orient)\.c$"),
    ("sensors", r"^drivers/(sensor|bme|bmi|ccs811|hdc1080|ina226|mcp9808|mhz19b|mics6814|sgp30|tmp117|tsl2|adc|battery|charger)"),
    ("usb", r"^drivers/(usb|piousb)"),
    ("audio", r"^drivers/audio"),
    ("storage", r"^drivers/(spisd|sdmmc|virtio_blk|flash_|psram_|xip_cache)"),
    ("clock-app", r"^drivers/(pico_clock|dcf77|gps_pps|tm1638)"),
    ("kernel", r"^kernel/|^arch/|^libc/"),
    ("drivers", r"^drivers/"),
]

_COMPILED = [(name, re.compile(rx)) for name, rx in RULES]


def subsystem(path: str) -> str:
    """The subsystem for a repository-relative source path (or a pseudo-path
    such as "(no line info)", which is returned unchanged)."""
    if path.startswith("(no line info: .radio"):
        return "radio"
    if path.startswith("("):
        return path
    for name, rx in _COMPILED:
        if rx.search(path):
            return name
    head = path.split("/", 1)[0]
    return head or path


def group(per_file: dict[str, int]) -> dict[str, int]:
    """Sums a {path: bytes} map by subsystem."""
    out: dict[str, int] = {}
    for path, n in per_file.items():
        key = subsystem(path)
        out[key] = out.get(key, 0) + n
    return out
