#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/klog.h"
#include "kernel/palloc.h"
#include "kernel/scratch.h"
#include "kernel/balloc.h"
#include "kernel/mem_domain.h"
#include "kernel/chan.h"
#include "kernel/device.h"
#include "kernel/ticker.h"
#include "kernel/line_editor.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "drivers/i2c_rtc.h"
#include "drivers/dcf77_decode.h"
#include "drivers/pico_clock_ui.h"
#include "kernel/timezone.h"
#include "kernel/sha256.h"
#include "kernel/random.h"
#include "kernel/idstore.h"
#include "kernel/identity.h"
#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "drivers/enc28j60.h"
#include "drivers/cyw43.h"
#if defined(CONFIG_BOARD_RP2350)
#include "drivers/spisd.h"
#else
#include "drivers/virtio_blk.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
#include "drivers/st7735.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
#include "drivers/tm1638.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
#include "drivers/pico_clock_green.h"
#endif
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "fs/9p.h"
#include "net/netif.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "net/ntp.h"
#include "arch/elf.h"
#include "kernel/path.h"
#include "arch/pmp.h"
#include "arch/umode.h"
#include "arch/trap.h"
#include "kernel/ipc.h"
#include "lugalos_config.h"
#include "lisp.h"
#if CONFIG_ENABLE_CC
#include "chibicc.h"
#endif
#if CONFIG_ENABLE_ED
#include "ed.h"
#endif
#include <string.h>

/* Bounded output buffer for the POSIX -> S-expression command transformer.
 * Every character bound for `sexpr[]` goes through sb_putc() so a long or
 * adversarial input degrades to a flagged, dropped write instead of walking
 * off the end of a fixed-size stack buffer. */
typedef struct {
    char *buf;
    int idx;
    int cap;         /* buf[cap - 1] is reserved for the terminating NUL */
    bool overflowed;
} sexpr_buf_t;

static void sb_init(sexpr_buf_t *sb, char *buf, int cap) {
    sb->buf = buf;
    sb->idx = 0;
    sb->cap = cap;
    sb->overflowed = false;
}

static void sb_putc(sexpr_buf_t *sb, char c) {
    if (sb->idx >= sb->cap - 1) {
        sb->overflowed = true;
        return;
    }
    sb->buf[sb->idx++] = c;
}

void shell_init(void) {
    printk("[Shell] Interactive Lugal Shell (lsh) initialized with Plan 9 Universal Namespace.\n");
}

static void cmd_help(void) {
    cprintf("\nAvailable LugalOS Shell Commands (Plan 9 Model):\n");
    cprintf("  help            - Display command manual\n");
    cprintf("  uname           - Show OS build target and architecture details\n");
    cprintf("  ps              - Alias for 'cat /proc/ps'\n");
    cprintf("  meminfo         - Alias for 'cat /proc/meminfo'\n");
    cprintf("  df              - Alias for 'cat /proc/df'\n");
    cprintf("  top             - System process, memory & storage monitor\n");
    cprintf("  date [YYYY-MM-DD HH:MM:SS] - Get or set system date and RTC time\n");
    cprintf("  ls [path]       - List directory (/flash0/, /sd0/, /ram0/, /proc/, /dev/, /srv/)\n");
    cprintf("  cat <path>      - Read and display path (/sd0/file, /proc/ps, /dev/uart)\n");
    cprintf("  touch <file>    - Create a new empty file\n");
    cprintf("  mkdir <path>    - Create a new directory\n");
    cprintf("  rmdir <path>    - Remove an empty directory\n");
    cprintf("  cp <src> <dst>  - Copy file from source path to destination path\n");
    cprintf("  write <p> <txt> - Write payload to a file, /dev/uart, or a /srv/ service endpoint\n");
    cprintf("  rm <file>       - Delete file from disk\n");
    cprintf("  format <path>   - Initialize a blank/corrupt volume as FAT32 (/sd0 or /ram0; DESTROYS existing data)\n");
#if CONFIG_ENABLE_CC
    cprintf("  cc <src> <dst>  - Compile C11 source file to native RISC-V ELF binary (chibicc)\n");
#endif
    cprintf("  exec <elf>      - Run a RISC-V ELF binary as a U-mode task, confined to its own pages\n");
    cprintf("  <name>          - Run <vol>/system/bin/<name>.elf from the first volume on the\n");
    cprintf("                    search path that has it (see 'cat /proc/path'); a path with a\n");
    cprintf("                    '/' in it is always taken literally\n");
    cprintf("  e [file]        - Launch Emacs-style full-screen editor\n");
#if CONFIG_ENABLE_ED
    cprintf("  ed [file]       - Launch teletype line editor\n");
#endif
    cprintf("  lisp            - Enter interactive Scheme / Lisp REPL environment\n");
    cprintf("  p9serve         - Headless 9P server over UART/SLIP (does not return; reset to exit)\n");
    cprintf("  p9share [off]   - Share this UART between the console and 9P (SLIP demux)\n");
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_UART1_BASE)
    cprintf("  uart1test [ms]  - Raw UART1 downlink test: registers, a burst, then listen\n");
    cprintf("  uart1pins       - Continuity between the downlink pins, using plain GPIO\n");
#endif
    cprintf("  net             - Network interfaces: MAC, link, frame counters\n");
    cprintf("  net txtest [n]  - Emit n test frames (default 1) on the first interface\n");
    cprintf("  net rxtest [n]  - Wait for n frames (default 1) and report what arrived\n");
    cprintf("  net udpecho [p] - Bind UDP port p (default 7) and echo what arrives\n");
    cprintf("  net listen [p]  - Listen for 9P over TCP on port p (default 564; 0 = stop)\n");
    cprintf("  netcfg [<ip> <mask> [gw]|clear] - Address this board comes up on (kept in the identity record)\n");
    cprintf("  ntp [server]    - Set the clock from an NTP server (default: the gateway)\n");
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_ETH_CS_GPIO)
    cprintf("  net regs        - ENC28J60: raw EIE/EIR/ESTAT/ECON1/2, EPKTCNT, RX pointers\n");
#endif
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_WL_CS_GPIO)
    cprintf("  wifi probe      - CYW43439: bus, firmware upload and CLM -- brings the chip up\n");
    cprintf("  wifi led [on|off] - Blink the user LED (on the wireless chip's own GPIO 0)\n");
    cprintf("  wifi join [<ssid> <psk-hex>] - Join a WPA2 network; no args uses the stored record\n");
    cprintf("  wifi stats      - CYW43439: RX ring high-water mark and drops\n");
    cprintf("  wifi trace [on|off] - Print every decoded firmware event (link, auth, handshake)\n");
#endif
    cprintf("  p9auth [<link> on|off] - Require 9P authentication on a link (no args: list)\n");
    cprintf("  p9key [<hex>|clear] - Set this boot's 9P auth key from the console (no args: status)\n");
    cprintf("  p9authselftest  - The auth gate's pure logic: path guard, MAC binding\n");
    cprintf("  klog [attach|detach <sink>] - Kernel log sinks; read the log via /proc/kmsg\n");
    cprintf("  write /srv/console <txt>    - Emit via the console server (a channel service)\n");
    cprintf("  taskdemo        - Spawn two cooperative tasks and show them interleave\n");
    cprintf("  preempttest     - Prove the timer preempts a task that never yields\n");
    cprintf("  priotest        - Prove a high-priority task wins the next reschedule over hogs\n");
    cprintf("  priostress      - Prove two same-tier busy tasks share the CPU fairly\n");
    cprintf("  pmpinfo         - Report this core's usable PMP regions and granularity\n");
    cprintf("  pmpdump         - Per-register PMP dump (reset value, readback, verdict)\n");
    cprintf("  usertest        - Run a task in U-mode and syscall back into the kernel\n");
    cprintf("  isolationtest   - U-mode task stores into kernel memory; must fault\n");
#if defined(CONFIG_BOARD_RP2350)
    cprintf("  heartbeatisotest - Same, under the real heartbeat driver task's own SIO-window domain\n");
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    cprintf("  tm1638isotest   - Same, under the real tm1638 driver task's own SIO-window domain\n");
#endif
#if defined(CONFIG_BOARD_RP2350)
    cprintf("  i2cisotest      - Same, under the real i2c driver task's own I2C-controller-window domain\n");
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    cprintf("  st7735isotest   - Same, under the real st7735 driver task's own SIO+SPI0-window domain\n");
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_SPISD
    cprintf("  blkisotest      - Same, under the real blk driver task's own SIO+SPI1-window domain\n");
#endif
#if defined(CONFIG_BOARD_RP2350)
    cprintf("  uartisotest     - Same, under the real uart driver task's own UART0-window domain\n");
    cprintf("  usbisotest      - Same, under the real usb_cdc driver task's own DPRAM+USBCTRL+state domain\n");
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    cprintf("  clockisotest    - Same, under the real clock driver task's own SIO+ADC+TIMER0(ro)+state domain\n");
#endif
    cprintf("  deputytest      - U-mode task asks the kernel to WRITE kernel memory; must be refused\n");
    cprintf("  chanechotest    - Client blocks on chan_call() into a real U-mode server; must echo back\n");
    cprintf("  hmacselftest    - SHA-256/HMAC-SHA-256 against the FIPS and RFC 4231 vectors\n");
    cprintf("  randtest [bits] - Measure the raw entropy source (bias, correlation, runs)\n");
    cprintf("  idstoreselftest - Identity record: states, corruption, unknown fields, round trip\n");
    cprintf("  identity [name <name>|provision [--force]|key <hex>|--generate]\n");
    cprintf("                  - report uid/name/mac/key fingerprint, or set/provision/key (no args: report)\n");
    cprintf("  peers [add <name> <hex> [<aname>] [ro|rw]|remove <name>]\n");
    cprintf("                  - list grants (name/fingerprint/aname/mode), or add/remove one\n");
    cprintf("  wlan [<ssid> <psk-hex>]\n");
    cprintf("                  - report ssid/psk fingerprint, or install a credential (derived PSK, not a passphrase)\n");
    cprintf("  dcf77selftest   - DCF-77 frame decoder against synthetic frames (no radio needed)\n");
    cprintf("  clockuiselftest - Pico-Clock-Green menu against synthetic key presses\n");
    cprintf("  date [ISO]      - show or set the clock in local time (kernel keeps UTC)\n");
    cprintf("  tz [POSIX TZ]   - show or set the timezone rule, e.g. CET-1CEST,M3.5.0,M10.5.0/3\n");
    cprintf("  tzselftest      - timezone rules against known DST transitions\n");
    cprintf("  i2c [scan]      - Scan the I2C bus for devices\n");
    cprintf("  time            - Show system uptime\n");
    cprintf("  version         - Alias for 'cat /proc/version'\n");
    cprintf("  (which \"name\")   - Where a bare name resolves to, without running it\n");
    cprintf("  (path-set \"a b\") - Reorder the search path (usually set in system/etc/init.lisp)\n");
    cprintf("  (help)          - List every bound Lisp primitive (works from 'lisp' or as a (...) line here)\n");
    cprintf("  reboot          - Restart the board (works even if the evaluator is inert)\n");
    cprintf("  clear           - Clear terminal screen\n\n");
}

static void cmd_uname(void) {
    char buf[128];
    int len = vfs_read("/proc/version", buf, sizeof(buf));
    if (len >= 0) cprintf("%s", buf);
#if defined(CONFIG_TARGET_RV32)
    cprintf("Architecture: RISC-V 32-bit (RV32IMAC)\n");
#elif defined(CONFIG_TARGET_RV64)
    cprintf("Architecture: RISC-V 64-bit (RV64GC)\n");
#endif

#if defined(CONFIG_NOMMU)
    cprintf("Memory Mode: NOMMU Physical Direct Execution\n");
#elif defined(CONFIG_MMU)
    cprintf("Memory Mode: Sv39 Virtual Memory Page Tables\n");
#endif
    cprintf("Namespace: Universal Path Resolver (/flash0/, /sd0/, /ram0/, /proc/, /dev/, /srv/)\n");
}

/* A3a "headless" 9P mode (plan/phase5_distributed_design.md): dedicates
 * this UART entirely to SLIP-framed 9P traffic and never returns to the
 * shell. This is the low-risk way to get a real 9P wire over a plain UART
 * (QEMU serial, RP2350 UART, a CP2102 dongle) without the RX
 * demultiplexing A3b would need to share the wire with the interactive
 * console -- that demux is deliberately deferred (see the A3 completion
 * notes). Only reachable by explicit user command; a reset is the only way
 * back. */
static void cmd_p9serve(void) {
    cprintf("\n[9P] Entering headless UART/SLIP 9P server mode -- this session will not\n");
    cprintf("     return to the shell. Reset the device to get the console back.\n\n");
    p9_link_t *link = uart_slip_get_link();
    for (;;) {
        p9_link_service(link);
    }
}

/* A3b "shared-wire" mode (plan/phase5_distributed_design.md): unlike
 * p9serve above, this returns to the shell immediately. It arms
 * drivers/uart_net.c's RX demux (off by default -- see its own doc
 * comments for the tradeoff this opts into) and registers the demuxed link
 * as a background 9P server, so SLIP-framed 9P traffic and normal
 * keystrokes can now share this UART: whichever arrives, uart_getc()
 * routes it to the right place. This is the single-cable story A3a's
 * headless mode doesn't cover (a real CP2102/RP2350 deployment with only
 * one wire back to the host). `p9share off` reverses both steps. */
/* N2, plan/phase18_networking_and_auth.md: which links demand a key.
 *
 * Per link, because the policy is a property of the wire: the same server,
 * the same namespace, and a different answer depending on whether the request
 * arrived over a channel inside this address space, a cable on the desk, or
 * an Ethernet jack. With no arguments it lists what each link is currently
 * doing, which is the question anyone debugging a refused attach asks first.
 */
static void cmd_p9auth(const char *arg) {
    if (!arg || !*arg) {
        cprintf("9P authentication policy by link:\n");
        const char *dname, *dkind;
        bool dpresent;
        for (uint32_t i = 0; dev_info(i, &dname, &dkind, &dpresent); i++) {
            p9_link_t *l = (p9_link_t *)dev_get(dname, DEV_KIND_P9LINK);
            if (!l) continue;   /* not a 9P link, or not present */
            cprintf("  %-10s %s\n", dname, l->auth_required ? "REQUIRED" : "not required");
        }
        cprintf("Keys configured: %s\n", p9_auth_have_keys() ? "yes" : "NO -- a link set to "
                "REQUIRED will refuse every attach");
        return;
    }

    char name[16];
    unsigned i = 0;
    while (arg[i] && arg[i] != ' ' && i < sizeof(name) - 1) { name[i] = arg[i]; i++; }
    name[i] = '\0';
    while (arg[i] == ' ') i++;
    const char *state = &arg[i];

    p9_link_t *l = (p9_link_t *)dev_get(name, DEV_KIND_P9LINK);
    if (!l) { cprintf("p9auth: no such 9P link '%s' (see /proc/devices)\n", name); return; }

    if (strcmp(state, "on") == 0)       l->auth_required = true;
    else if (strcmp(state, "off") == 0) l->auth_required = false;
    else { cprintf("usage: p9auth <link> on|off\n"); return; }

    cprintf("p9auth: link '%s' now %s\n", name,
            l->auth_required ? "REQUIRES authentication" : "does not require authentication");
    if (l->auth_required && !p9_auth_have_keys()) {
        cprintf("p9auth: WARNING no keys configured (%s or %s) -- this link will now\n"
                "        refuse every attach, which is the intended failure direction.\n",
                P9_AUTH_KEYS_FILE, P9_AUTH_FALLBACK_KEY_FILE);
    }
}

/* --- `net`: what interfaces this board has, and what they have seen ---
 * R1, plan/phase19_ip_stack_and_ethernet.md.
 *
 * The command name is the one phase 18's W5500 driver used, and it is
 * deliberately reused rather than renamed: `net` answering "no interfaces"
 * on a board that has none is a better answer than an unknown command, and
 * from R4 it answers about a real wire again.
 *
 * txtest/rxtest exist for the same reason their W5500 namesakes did -- a
 * driver that can neither be made to transmit nor observed receiving is
 * debugged by guesswork -- but they cost far less here, because the peer on
 * the other side is a Python script that can say byte-for-byte what it saw
 * (tests/netpeer.py, plan §4a). This is the pair R1 is verified with. */
static void cmd_net_status(void) { net_print_status(); }

/* A frame with somewhere to go and nothing to say: broadcast destination, our
 * own MAC as source, an EtherType of 0x88b5 (IEEE-reserved for local
 * experimental use, so it can never be mistaken for a real protocol), and a
 * payload naming itself and carrying a sequence number. A peer that receives
 * it can assert on every byte. */
static uint32_t build_test_frame(netif_t *nif, uint8_t *out, uint32_t seq) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < 6; i++) out[o++] = 0xff;              /* broadcast */
    for (uint32_t i = 0; i < 6; i++) out[o++] = nif->mac[i];       /* source */
    out[o++] = 0x88; out[o++] = 0xb5;                              /* EtherType */
    const char *tag = "LUGALOS-NETIF-TEST";
    for (const char *p = tag; *p; p++) out[o++] = (uint8_t)*p;
    out[o++] = (uint8_t)(seq >> 24); out[o++] = (uint8_t)(seq >> 16);
    out[o++] = (uint8_t)(seq >> 8);  out[o++] = (uint8_t)seq;
    /* Padded to the 60-byte minimum a real Ethernet segment wants, so the
     * same frame is legal on the ENC28J60 in R4 without a second shape. */
    while (o < 60) out[o++] = 0;
    return o;
}

static void cmd_net_txtest(unsigned count) {
    netif_t *nif = netif_default();
    if (!nif) { cprintf("net: no interfaces.\n"); return; }
    if (count == 0) count = 1;

    uint8_t frame[64];
    unsigned sent = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t len = build_test_frame(nif, frame, i);
        if (netif_send(nif, frame, len) > 0) sent++;
    }
    cprintf("net: %u/%u test frames sent on %s (%lu tx errors total)\n",
            sent, count, nif->name, (unsigned long)nif->tx_errors);
}

static void cmd_net_rxtest(unsigned count) {
    netif_t *nif = netif_default();
    if (!nif) { cprintf("net: no interfaces.\n"); return; }
    if (count == 0) count = 1;

    cprintf("net: waiting for %u frame%s the stack does not claim...\n",
            count, count == 1 ? "" : "s");

    /* Drains net/stack.c's latch. The yield is what lets `netsrv` run at all:
     * without it this loop starves the task that fills the latch. Bounded by
     * iterations rather than by time, because this shell has no sleep that
     * does not also stop being responsive, and a diagnostic that waits
     * forever for a peer that will never speak is worse than one that gives
     * up and says so. */
    unsigned got = 0;
    uint8_t head[NET_UNCLAIMED_HEAD];
    for (unsigned long spin = 0; spin < 200000ul && got < count; spin++) {
        uint32_t n = net_take_unclaimed(head);
        if (n == 0) { sched_yield(); continue; }
        char src[18], dst[18];
        netif_mac_str(head, dst);
        netif_mac_str(head + 6, src);
        cprintf("net: rx %lu bytes, %s -> %s, type 0x%02x%02x\n",
                (unsigned long)n, src, dst, head[12], head[13]);
        got++;
    }
    if (got < count) cprintf("net: gave up after %u/%u frames\n", got, count);
    else cprintf("net: rxtest done, %u frames\n", got);
}

/* `net udpecho [port]` -- binds a UDP port and echoes whatever arrives back
 * to its sender.
 *
 * Seven lines that make the UDP path testable from outside without inventing
 * a protocol: the peer sends a datagram, gets the same bytes back from the
 * same address, and has therefore exercised bind, dispatch, the pseudo-header
 * checksum in both directions, and the ARP resolution behind the reply. Port
 * 7 by default because that is what the echo service has been since RFC 862.
 * There is no unbind command: the binding is meant to outlive the command
 * that made it, and a reboot is the way out. */
static void udp_echo_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port,
                        const uint8_t *data, uint32_t len) {
    uint16_t port = (uint16_t)(uintptr_t)ctx;
    udp_send(src_ip, src_port, port, data, len);
    cprintf("net: udpecho %lu bytes from %u.%u.%u.%u:%u\n",
            (unsigned long)len, src_ip[0], src_ip[1], src_ip[2], src_ip[3], src_port);
}

/* `ntp [server]` -- R6: ask one server what time it is, and believe it.
 *
 * With no argument it asks the configured **gateway**. Not a guess dressed up
 * as a default: a home segment's router is the one address this board already
 * knows, and it is running an NTP server far more often than not. When it is
 * not, the answer is a three-second timeout naming the address it tried,
 * which is a better failure than refusing to act without an argument. A
 * server address is deliberately *not* stored in the identity record -- that
 * is a per-site fact, not a per-board one, and I9 put the address in the
 * record precisely because the address is per-board. */
static void cmd_ntp(const char *arg) {
    uint8_t server[IPV4_LEN];

    while (arg && *arg == ' ') arg++;
    if (arg && *arg) {
        if (!ipv4_parse(arg, server)) {
            cprintf("ntp: '%s' is not a dotted quad\n", arg);
            cprintf("usage: ntp [server-ip]   (no argument asks the gateway)\n");
            return;
        }
    } else {
        const net_state_t *st = net_state();
        if (!st || !st->configured) {
            cprintf("ntp: no address configured -- set one with `netcfg`, or give a server\n");
            return;
        }
        memcpy(server, st->gw, IPV4_LEN);
        if (!server[0] && !server[1] && !server[2] && !server[3]) {
            cprintf("ntp: no gateway configured, so there is nothing to ask by default\n");
            cprintf("usage: ntp <server-ip>\n");
            return;
        }
        cprintf("ntp: asking the gateway, %u.%u.%u.%u\n",
                server[0], server[1], server[2], server[3]);
    }

    console_interrupt_clear();
    ntp_result_t r;
    int rc = ntp_sync(server, 3000u, &r);
    if (rc != 0) {
        cprintf("ntp: %s\n", ntp_err_str(rc));
        if (rc == NTP_ERR_KISS) cprintf("  the server's reason: \"%s\"\n", r.refid);
        return;
    }
    ntp_print_result(&r, true);
}

static void cmd_net_udpecho(unsigned port) {
    if (port == 0 || port > 65535u) port = 7;
    if (udp_bind((uint16_t)port, udp_echo_cb, (void *)(uintptr_t)port) != 0) {
        cprintf("net: could not bind UDP port %u (already bound, or no slots)\n", port);
        return;
    }
    cprintf("net: echoing UDP on port %u\n", port);
}

/* Parses a trailing unsigned decimal argument, or 0 if there is none. */
static unsigned shell_trailing_uint(const char *s) {
    while (*s == ' ') s++;
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (unsigned)(*s++ - '0');
    return v;
}

/* N2: install a key for this boot, from the console.
 *
 * Deliberately not persistent and deliberately console-only -- see
 * p9_auth_set_console_key() in fs/9p.c for why a key that lives in RAM and
 * dies at reboot is the right shape for both bootstrapping a gateway and
 * testing the gate, and why the alternative (a test key in the flash image)
 * would have been a key on every board that image ever reaches. */
static void cmd_p9key(const char *arg) {
    if (!arg || !*arg) {
        cprintf("9P auth key: %s\n", p9_auth_have_keys() ? "configured" : "none");
        cprintf("  console key : set with `p9key <hex>`; this boot only, overrides the files\n");
        cprintf("  key list    : %s   (one \"uname hexkey\" line per identity)\n", P9_AUTH_KEYS_FILE);
        cprintf("  single key  : %s\n", P9_AUTH_FALLBACK_KEY_FILE);
        cprintf("Neither file is servable over 9P, on any transport.\n");
        return;
    }
    if (strcmp(arg, "clear") == 0) {
        p9_auth_set_console_key(NULL, 0);
        cprintf("p9key: console key cleared\n");
        return;
    }

    uint8_t key[64];
    uint32_t len = 0;
    for (const char *h = arg; h[0] && h[1] && len < sizeof(key); h += 2) {
        int hi = -1, lo = -1;
        for (int p = 0; p < 2; p++) {
            char c = h[p];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (p == 0) hi = v; else lo = v;
        }
        if (hi < 0 || lo < 0) break;
        key[len++] = (uint8_t)((hi << 4) | lo);
    }
    if (len == 0) { cprintf("p9key: expected an even-length hex string, or `clear`\n"); return; }
    p9_auth_set_console_key(key, len);
    cprintf("p9key: console key set (%u bytes), this boot only\n", (unsigned)len);
}

/* --- `identity`: I3, plan/phase21_identity_and_authentication.md §6 ---
 *
 * uid, name, both their sources, MAC, and a key fingerprint -- never the key
 * itself. `identity name`/`provision`/`key` are the only things in this
 * shell that write to the identity store; everything else here only reads
 * it. user/lisp/lisp.c's identity/identity-name/identity-provision/
 * identity-key mirror these four exactly, for /sd0/system/etc/usr_init.lisp. */

static void identity_print_report(void) {
    cprintf("name: %s (%s)\n", node_name(), node_name_source());
    char mac[18];
    netif_mac_str(node_mac(), mac);
    cprintf("mac: %s (%s)\n", mac, node_mac_source());

    static const char hex[] = "0123456789abcdef";
    uint8_t uid[NODE_UID_LEN];
    if (node_uid(uid)) {
        char uidhex[NODE_UID_LEN * 2 + 1];
        for (unsigned i = 0; i < NODE_UID_LEN; i++) {
            uidhex[i * 2]     = hex[uid[i] >> 4];
            uidhex[i * 2 + 1] = hex[uid[i] & 0x0f];
        }
        uidhex[NODE_UID_LEN * 2] = '\0';
        cprintf("uid: %s (%s)\n", uidhex, node_uid_source());
    } else {
        cprintf("uid: none (%s)\n", node_uid_source());
    }

    uint8_t key[NODE_DEVKEY_MAX];
    uint32_t key_len = 0;
    if (node_devkey(key, sizeof(key), &key_len)) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(key, key_len, fp);
        cprintf("key fingerprint: %s\n", fp);
    } else {
        cprintf("key fingerprint: none\n");
    }
    memset(key, 0, sizeof(key));
}

/* Parses a run of hex pairs into `out`, the same shape cmd_p9key() already
 * uses -- kept separate rather than shared, matching how that function's own
 * loop is not shared with fs/9p.c's hexval()+p9_auth_key_for() either: three
 * short, independent parsers cost less than the coupling a shared one would
 * add between the auth gate, the console key and this command. */
static uint32_t parse_hex_bytes(const char *s, uint8_t *out, uint32_t cap) {
    uint32_t len = 0;
    for (const char *h = s; h[0] && h[1] && len < cap; h += 2) {
        int hi = -1, lo = -1;
        for (int p = 0; p < 2; p++) {
            char c = h[p];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (p == 0) hi = v; else lo = v;
        }
        if (hi < 0 || lo < 0) break;
        out[len++] = (uint8_t)((hi << 4) | lo);
    }
    return len;
}

static void cmd_identity(const char *arg) {
    if (!arg || !*arg) { identity_print_report(); return; }

    char sub[16];
    unsigned i = 0;
    while (arg[i] && arg[i] != ' ' && i < sizeof(sub) - 1) { sub[i] = arg[i]; i++; }
    sub[i] = '\0';
    while (arg[i] == ' ') i++;
    const char *rest = &arg[i];

    if (strcmp(sub, "name") == 0) {
        if (!*rest) { cprintf("usage: identity name <name>\n"); return; }
        node_id_result_t rc = node_identity_rename_persistent(rest);
        if (rc == NODE_ID_OK) cprintf("identity: renamed to '%s' (persisted)\n", node_name());
        else                  cprintf("identity name: %s\n", node_id_result_str(rc));
        return;
    }

    if (strcmp(sub, "provision") == 0) {
        bool force = strcmp(rest, "--force") == 0;
        node_id_result_t rc = node_identity_provision(force);
        if (rc == NODE_ID_OK) { cprintf("identity: provisioned\n"); identity_print_report(); }
        else                  cprintf("identity provision: %s\n", node_id_result_str(rc));
        return;
    }

    if (strcmp(sub, "key") == 0) {
        node_id_result_t rc;
        if (strcmp(rest, "--generate") == 0) {
            rc = node_identity_generate_key();
        } else if (*rest) {
            uint8_t key[NODE_DEVKEY_MAX];
            uint32_t len = parse_hex_bytes(rest, key, sizeof(key));
            if (len == 0) { cprintf("identity key: expected an even-length hex string, or --generate\n"); return; }
            rc = node_identity_set_key(key, len);
            memset(key, 0, sizeof(key));
        } else {
            cprintf("usage: identity key <hex>|--generate\n");
            return;
        }

        if (rc != NODE_ID_OK) { cprintf("identity key: %s\n", node_id_result_str(rc)); return; }
        cprintf("identity: key installed\n");
        uint8_t k[NODE_DEVKEY_MAX];
        uint32_t klen = 0;
        if (node_devkey(k, sizeof(k), &klen)) {
            char fp[KEY_FINGERPRINT_HEX_LEN + 1];
            key_fingerprint_hex(k, klen, fp);
            cprintf("key fingerprint: %s\n", fp);
        }
        memset(k, 0, sizeof(k));
        return;
    }

    cprintf("usage: identity [name <name> | provision [--force] | key <hex>|--generate]\n");
}

/* --- `peers`: I5, plan/phase21_identity_and_authentication.md §5.2/§6 ---
 *
 * The grants list -- deliberately not built in I3 alongside `identity`,
 * since before I5 there was nothing here for it to enforce (see that
 * milestone's own "scope cut" note). Same fingerprint-only discipline as
 * `identity`: the key never prints, only its fingerprint. Lisp equivalents
 * (peers, peers-add, peers-remove) mirror these three exactly. */

static void peers_print_report(void) {
    p9_grant_t entries[P9_GRANTS_MAX];
    uint32_t count = p9_grants_list(entries, P9_GRANTS_MAX);
    if (count == 0) { cprintf("peers: no grants configured\n"); return; }
    cprintf("%-16s %-16s %-20s mode\n", "name", "fingerprint", "aname");
    for (uint32_t i = 0; i < count; i++) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(entries[i].key, entries[i].key_len, fp);
        cprintf("%-16s %-16s %-20s %s\n", entries[i].name, fp, entries[i].aname,
                entries[i].read_only ? "ro" : "rw");
    }
}

/* Consumes one whitespace-delimited token from `s`, returning the cursor
 * past it (and past any trailing whitespace) so callers can chain calls
 * for a fixed-shape command line -- `peers add`'s four positional
 * pieces -- without hand-tracking an index through each one. */
static const char *next_token(const char *s, char *out, uint32_t cap) {
    uint32_t i = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && i < cap - 1) { out[i++] = *s; s++; }
    out[i] = '\0';
    while (*s == ' ') s++;
    return s;
}

static void cmd_peers(const char *arg) {
    if (!arg || !*arg) { peers_print_report(); return; }

    char sub[16];
    unsigned i = 0;
    while (arg[i] && arg[i] != ' ' && i < sizeof(sub) - 1) { sub[i] = arg[i]; i++; }
    sub[i] = '\0';
    while (arg[i] == ' ') i++;
    const char *rest = &arg[i];

    if (strcmp(sub, "add") == 0) {
        char name[P9_MAX_NAME_LEN], hexstr[160], tok3[P9_MAX_NAME_LEN], tok4[4];
        const char *p = rest;
        p = next_token(p, name, sizeof(name));
        p = next_token(p, hexstr, sizeof(hexstr));
        p = next_token(p, tok3, sizeof(tok3));
        next_token(p, tok4, sizeof(tok4));

        if (name[0] == '\0' || hexstr[0] == '\0') {
            cprintf("usage: peers add <name> <hex> [<aname>] [ro|rw]\n");
            return;
        }

        char aname[P9_MAX_NAME_LEN];
        strncpy(aname, "/", sizeof(aname) - 1);
        aname[sizeof(aname) - 1] = '\0';
        bool read_only = false;
        if (strcmp(tok3, "ro") == 0)      read_only = true;
        else if (strcmp(tok3, "rw") == 0) read_only = false;
        else if (tok3[0])                 strncpy(aname, tok3, sizeof(aname) - 1);
        if (strcmp(tok4, "ro") == 0)      read_only = true;
        else if (strcmp(tok4, "rw") == 0) read_only = false;

        uint8_t key[P9_AUTH_KEY_MAX];
        uint32_t klen = parse_hex_bytes(hexstr, key, sizeof(key));
        if (klen == 0) { cprintf("peers add: expected an even-length hex string for the key\n"); return; }

        p9_grant_result_t rc = p9_grants_add(name, key, klen, aname, read_only);
        memset(key, 0, sizeof(key));
        if (rc != P9_GRANT_OK) { cprintf("peers add: %s\n", p9_grant_result_str(rc)); return; }
        cprintf("peers: granted '%s' at %s (%s)\n", name, aname, read_only ? "ro" : "rw");
        return;
    }

    if (strcmp(sub, "remove") == 0) {
        if (!*rest) { cprintf("usage: peers remove <name>\n"); return; }
        p9_grant_result_t rc = p9_grants_remove(rest);
        if (rc == P9_GRANT_OK) cprintf("peers: removed '%s'\n", rest);
        else                   cprintf("peers remove: %s\n", p9_grant_result_str(rc));
        return;
    }

    cprintf("usage: peers [add <name> <hex> [<aname>] [ro|rw] | remove <name>]\n");
}

/* --- `wlan`: I6, plan/phase21_identity_and_authentication.md §5.3/§6 ---
 *
 * Installs a network credential -- the *derived* PSK
 * (tools/provision.py's derive_wpa2_psk() on the host), never a
 * passphrase; this command has no way to accept one, on purpose. Lands
 * with phase 19's R5 (the CYW43 driver) and is unused before it, same
 * shape as `identity key` was between I3 and I4. Fingerprint-only, same
 * discipline as `identity`/`peers`: the SSID prints in full (it is not a
 * secret -- every AP broadcasts it in its own beacon frames), the PSK
 * never does. */
static void wlan_print_report(void) {
    char ssid[NODE_WLAN_SSID_MAX + 1];
    uint8_t psk[NODE_WLAN_PSK_LEN];
    bool have_ssid = node_wlan_ssid(ssid, sizeof(ssid));
    bool have_psk = node_wlan_psk(psk);
    cprintf("ssid: %s\n", have_ssid ? ssid : "none");
    if (have_psk) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(psk, sizeof(psk), fp);
        cprintf("psk fingerprint: %s\n", fp);
    } else {
        cprintf("psk fingerprint: none\n");
    }
    memset(psk, 0, sizeof(psk));
}

static void netcfg_print_report(void) {
    uint8_t ip[NODE_IPV4_LEN], mask[NODE_IPV4_LEN], gw[NODE_IPV4_LEN];
    if (!node_ipv4(ip, mask, gw)) {
        cprintf("address: none stored -- this board comes up unconfigured\n");
        cprintf("  netcfg <ip> <mask> [gw]  stores one; it is applied at every boot\n");
        return;
    }
    cprintf("address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    cprintf("netmask: %u.%u.%u.%u\n", mask[0], mask[1], mask[2], mask[3]);
    if ((gw[0] | gw[1] | gw[2] | gw[3]) == 0) {
        cprintf("gateway: none (no route off this segment)\n");
    } else {
        cprintf("gateway: %u.%u.%u.%u\n", gw[0], gw[1], gw[2], gw[3]);
    }
}

/* `netcfg` -- the address this board comes up on, stored in the identity
 * record rather than in a boot script. See kernel/include/kernel/idstore.h's
 * IDSTORE_FIELD_IPV4 for why that distinction matters: the filesystem image
 * is identical on every board, and it should stay that way. */
static void cmd_netcfg(const char *arg) {
    if (!arg || !*arg) { netcfg_print_report(); return; }

    if (strcmp(arg, "clear") == 0) {
        node_id_result_t rc = node_identity_clear_ipv4();
        if (rc == NODE_ID_OK) cprintf("netcfg: cleared -- this board will come up unconfigured\n");
        else                  cprintf("netcfg: %s\n", node_id_result_str(rc));
        return;
    }

    char ipstr[24], maskstr[24], gwstr[24];
    const char *p = arg;
    p = next_token(p, ipstr, sizeof(ipstr));
    p = next_token(p, maskstr, sizeof(maskstr));
    next_token(p, gwstr, sizeof(gwstr));

    if (ipstr[0] == '\0' || maskstr[0] == '\0') {
        cprintf("usage: netcfg <ip> <mask> [gw] | netcfg clear | netcfg\n");
        return;
    }

    uint8_t ip[NODE_IPV4_LEN], mask[NODE_IPV4_LEN], gw[NODE_IPV4_LEN] = { 0, 0, 0, 0 };
    if (!ipv4_parse(ipstr, ip))     { cprintf("netcfg: '%s' is not a dotted quad\n", ipstr); return; }
    if (!ipv4_parse(maskstr, mask)) { cprintf("netcfg: '%s' is not a dotted quad\n", maskstr); return; }
    if (gwstr[0] != '\0' && !ipv4_parse(gwstr, gw)) {
        cprintf("netcfg: '%s' is not a dotted quad\n", gwstr);
        return;
    }

    node_id_result_t rc = node_identity_set_ipv4(ip, mask, gw);
    if (rc != NODE_ID_OK) { cprintf("netcfg: %s\n", node_id_result_str(rc)); return; }
    cprintf("netcfg: stored -- applied at every boot, before the stack task starts\n");
    netcfg_print_report();
}

static void cmd_wlan(const char *arg) {
    if (!arg || !*arg) { wlan_print_report(); return; }

    char ssid[NODE_WLAN_SSID_MAX + 1], hexstr[80];
    const char *p = arg;
    p = next_token(p, ssid, sizeof(ssid));
    next_token(p, hexstr, sizeof(hexstr));

    if (ssid[0] == '\0' || hexstr[0] == '\0') {
        cprintf("usage: wlan <ssid> <psk-hex>\n");
        return;
    }

    /* strlen() checked directly, not just parse_hex_bytes()'s return count:
     * that function stops filling at `cap` bytes and would otherwise
     * silently accept (and truncate) a too-long hex string -- e.g. a
     * device key pasted into the wrong command -- as if it were a
     * correctly-sized PSK. */
    if (strlen(hexstr) != NODE_WLAN_PSK_LEN * 2) {
        cprintf("wlan: expected exactly %u hex characters (a derived WPA2 PSK is always 256 bits) -- "
                "derive one with tools/provision.py, not by hand\n", (unsigned)(NODE_WLAN_PSK_LEN * 2));
        return;
    }
    uint8_t psk[NODE_WLAN_PSK_LEN];
    uint32_t psklen = parse_hex_bytes(hexstr, psk, sizeof(psk));
    if (psklen != NODE_WLAN_PSK_LEN) {
        cprintf("wlan: expected an even-length hex string\n");
        memset(psk, 0, sizeof(psk));
        return;
    }

    node_id_result_t rc = node_identity_set_wlan(ssid, (uint32_t)strlen(ssid), psk, psklen);
    memset(psk, 0, sizeof(psk));
    if (rc != NODE_ID_OK) { cprintf("wlan: %s\n", node_id_result_str(rc)); return; }
    cprintf("wlan: credential installed\n");
    wlan_print_report();
}

static void cmd_p9share(const char *arg) {
    bool enable = true;
    if (arg) {
        while (*arg == ' ') arg++;
        if (strcmp(arg, "off") == 0) enable = false;
    }

    p9_link_t *link = uart_demux_get_link();
    uart_demux_set_enabled(enable);
    if (enable) {
        p9_link_register_background(link);
        cprintf("\n[9P] Shared-wire 9P active on this UART alongside the console -- type\n");
        cprintf("     normally; a SLIP-framed 9P peer can attach on the same wire at any\n");
        cprintf("     time. Run 'p9share off' to disable.\n\n");
    } else {
        p9_link_unregister_background(link);
        cprintf("\n[9P] Shared-wire 9P disabled; this UART is console-only again.\n\n");
    }
}

/* B0 (plan/phase5_distributed_design.md §5.4): inspect and rebind kernel log
 * output sinks at runtime. `klog` lists them; `klog detach console` stops
 * kernel output reaching this terminal -- which is what makes handing this
 * UART to 9P (`p9serve`) or, later, to a login shell non-destructive: the
 * ring keeps accumulating regardless, so `cat /proc/kmsg` still has the full
 * log afterwards, and so does a remote node reading it over 9P. */
static void cmd_klog(const char *arg) {
    if (arg) {
        while (*arg == ' ') arg++;
    }

    if (!arg || *arg == '\0') {
        cprintf("\nKernel log sinks:\n");
        const char *name;
        bool attached;
        for (uint32_t i = 0; klog_sink_info(i, &name, &attached); i++) {
            /* No '-' (left-justify) flag in this printk engine -- see
             * kernel/printk.c's format parser, which accepts only '0',
             * width, '.prec' and 'l'. Plain "name: state" instead. */
            cprintf("  %s: %s\n", name, attached ? "attached" : "detached");
        }
        cprintf("\n  Ring: %lu bytes buffered (%lu total written)\n",
               (unsigned long)(klog_total() - klog_oldest()),
               (unsigned long)klog_total());
        cprintf("  Usage: klog [attach|detach] <sink>   (read it with: cat /proc/kmsg)\n\n");
        return;
    }

    bool detach;
    const char *name;
    if (strncmp(arg, "detach ", 7) == 0) {
        detach = true;
        name = arg + 7;
    } else if (strncmp(arg, "attach ", 7) == 0) {
        detach = false;
        name = arg + 7;
    } else {
        printk("[klog] Usage: klog [attach|detach] <sink>\n");
        return;
    }

    while (*name == ' ') name++;
    int rc = detach ? klog_sink_detach(name) : klog_sink_attach(name);
    if (rc < 0) {
        printk("[klog] No such sink: '%s'\n", name);
        return;
    }
    /* If the console sink was just detached this message goes nowhere, which
     * is the point -- it is still recorded in the ring for /proc/kmsg. */
    printk("[klog] Sink '%s' %s\n", name, detach ? "detached" : "attached");
}

/* B2: proves the cooperative scheduler actually switches, rather than
 * merely bookkeeping as the pre-B2 shim did.
 *
 * Two tasks each emit their own marker three times, yielding between each.
 * If switching is real the output interleaves (A1 B1 A2 B2 A3 B3); if
 * sched_yield() were still a no-op the first task would run to completion
 * before the second started (A1 A2 A3 B1 B2 B3). The interleaving is
 * therefore the assertion, not the fact that output appears at all --
 * tests/runner.py checks for exactly that ordering. */
static void taskdemo_body(void *arg) {
    const char *tag = (const char *)arg;
    for (int i = 1; i <= 3; i++) {
        printk("[TaskDemo] %s%d\n", tag, i);
        sched_yield();
    }
}

/* B3: proves the M->U transition and the trap path's stack swap.
 *
 * The user function below runs with the privilege level actually lowered: it
 * cannot touch CSRs, cannot execute privileged instructions, and traps into
 * the kernel for every ecall. Each ecall exercises the scratch-CSR swap added
 * in B3's first commit -- entering the kernel on the task's kernel stack
 * rather than on the user stack it was using.
 *
 * SYS_PUTCHAR is used deliberately: it passes a *value*, not a pointer. The
 * pointer-taking syscalls still dereference user addresses directly, and
 * calling one from here would "work" only because nothing separates the
 * address spaces yet. That is B3's copy-in/copy-out step, not this one. */
/* --- U-mode probe code ---
 *
 * These live in .utext, the one page U-mode is granted execute on, and must
 * be genuinely self-contained: anything they reach outside that page is a
 * fault. Two things break that by default and neither is obvious:
 *
 *   - UBSan instrumentation (on for the QEMU builds) inserts calls to
 *     __ubsan_handle_*_abort in kernel .text. Hence no_sanitize here.
 *   - String literals are emitted into kernel .rodata and loaded from there,
 *     so every message has to be built by explicit assignment instead. Ugly,
 *     and the ugliness is the point: it is visible rather than depending on
 *     what the optimiser happened to do. RV32 inlined the stores and RV64
 *     loaded from .rodata for identical source, which is exactly the kind of
 *     difference that turns into a mystery fault on one target only.
 *
 * B6's ELF work replaces all of this with separately linked user programs,
 * where self-containment is structural rather than hand-maintained. */
#define UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* Value-only syscall: no pointer, so nothing to validate and nothing outside
 * .utext to reach. */
/* always_inline, not merely inline: at -Os the compiler emitted these as real
 * functions in kernel .text, which U-mode cannot execute. A helper that is
 * "obviously" inlined is not a guarantee. */
__attribute__((always_inline)) static inline void usys_putc(char c) {
    __asm__ __volatile__("mv a0, %0\n mv a1, %1\n ecall"
                         :: "r"(12), "r"((uintptr_t)c) : "a0", "a1");
}
__attribute__((always_inline)) static inline long usys_read(const char *path, void *buf, long len) {
    long rc;
    __asm__ __volatile__("mv a0, %1\n mv a1, %2\n mv a2, %3\n mv a3, %4\n"
                         "ecall\n mv %0, a0"
                         : "=r"(rc)
                         : "r"(13), "r"((uintptr_t)path), "r"((uintptr_t)buf), "r"(len)
                         : "a0", "a1", "a2", "a3", "memory");
    return rc;
}
__attribute__((always_inline)) static inline void usys_exit(void) {
    __asm__ __volatile__("mv a0, %0\n ecall" :: "r"(SYS_UEXIT) : "a0");
}

UATTR static void user_probe(void) {
    usys_putc('U'); usys_putc('M'); usys_putc('O'); usys_putc('D');
    usys_putc('E'); usys_putc('_'); usys_putc('O'); usys_putc('K');
    usys_exit();
    for (;;) { }
}

/* The isolation probe: a store into kernel memory the task was never granted.
 * Under a correct domain it faults and the task is terminated. */
static volatile uintptr_t g_kernel_canary = 0xC0FFEE;

UATTR static void user_intruder(void) {
    g_kernel_canary = 0xDEAD;
    /* Only reached if the store was NOT stopped. Report explicitly: silence
     * would look identical to a correctly terminated task. */
    usys_putc('N'); usys_putc('O'); usys_putc('T'); usys_putc('_');
    usys_putc('I'); usys_putc('S'); usys_putc('O');
    usys_exit();
    for (;;) { }
}

/* The confused-deputy probe: can a U-mode task get the KERNEL to write to
 * kernel memory on its behalf? SYS_READ_FILE's `buf` is a destination the
 * kernel writes into, validated by copy_to_user() against this task's domain.
 * Both outcomes are asserted -- a syscall layer refusing every pointer would
 * pass the "refused" half while being useless. */
static volatile uintptr_t g_deputy_target = 0xFEEDFACE;

UATTR static void user_deputy(void) {
    /* volatile, because gcc recognises a run of consecutive char stores and
     * turns it back into a copy from a .rodata blob -- defeating the whole
     * point of writing them out. RV32 did not do this and RV64 did, for
     * identical source. */
    volatile char path[16];
    path[0]='/'; path[1]='p'; path[2]='r'; path[3]='o'; path[4]='c';
    path[5]='/'; path[6]='v'; path[7]='e'; path[8]='r'; path[9]='s';
    path[10]='i'; path[11]='o'; path[12]='n'; path[13]='\0';
    char mybuf[64];

    long rc = usys_read((const char *)path, (void *)&g_deputy_target, 16);
    if (rc < 0) {
        usys_putc('D'); usys_putc('E'); usys_putc('P'); usys_putc('U');
        usys_putc('T'); usys_putc('Y'); usys_putc('_');
        usys_putc('R'); usys_putc('E'); usys_putc('F'); usys_putc('U');
        usys_putc('S'); usys_putc('E'); usys_putc('D');
    } else {
        usys_putc('D'); usys_putc('E'); usys_putc('P'); usys_putc('U');
        usys_putc('T'); usys_putc('Y'); usys_putc('_');
        usys_putc('W'); usys_putc('R'); usys_putc('O'); usys_putc('T'); usys_putc('E');
    }

    rc = usys_read((const char *)path, mybuf, 32);
    usys_putc(' ');
    if (rc >= 0) {
        usys_putc('O'); usys_putc('W'); usys_putc('N'); usys_putc('B');
        usys_putc('U'); usys_putc('F'); usys_putc('_');
        usys_putc('O'); usys_putc('K');
    } else {
        usys_putc('O'); usys_putc('W'); usys_putc('N'); usys_putc('B');
        usys_putc('U'); usys_putc('F'); usys_putc('_');
        usys_putc('B'); usys_putc('A'); usys_putc('D');
    }
    usys_exit();
    for (;;) { }
}

/* A full page, page-aligned: one rule that satisfies both backends. PMP needs
 * power-of-two and self-aligned; Sv39 cannot grant anything finer than a page,
 * so a sub-page region would silently hand U-mode the rest of the page. Taking
 * the stricter of the two keeps a single description working on both.
 *
 * §3.1 (plan/phase15_memory_reclamation.md): taken from the heap for the
 * duration of the probe rather than held in .bss for the life of the board.
 * `usertest` is a diagnostic command; this page spent every boot reserved
 * against someone typing it. palloc_pages() returns page-aligned memory on
 * every target, which is exactly the property the paragraph above needs, so
 * the grant is unchanged -- only where the page comes from moved. */
static scratch_t g_user_stack_sc;
static uint8_t *g_user_stack;
static mem_domain_t g_user_domain;

/* Set only once a task has actually reached U-mode. Without it the isolation
 * report is a false positive on any core where the domain could not be
 * installed: the canary is untouched because nothing ran, which reads
 * identically to "the write was correctly blocked". */
static volatile bool g_user_entered;

static void user_task_common(void (*entry)(void)) {
    mem_domain_init(&g_user_domain);

    /* Order matters: PMP resolves against the lowest-numbered matching
     * region, so the narrow RW grant for this task's own stack must come
     * before the broad RX grant that also covers it. */
    mem_domain_add(&g_user_domain, (uintptr_t)g_user_stack, 4096,
                   MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_user_domain, tbase, tsize, MEM_R | MEM_X);

    if (task_set_domain(sched_current_pid(), &g_user_domain) != 0) {
        /* The hardware did not install the domain as written, so nothing here
         * knows what this task would actually be allowed to touch. Refusing is
         * the only honest option -- entering U-mode anyway would report
         * "isolated" on the strength of a restriction that was never verified. */
        printk("[UserTest] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_user_entered = true;
    /* ra = 0: every probe below ends with usys_exit() and an infinite loop,
     * so none of them ever returns. The ELF loader is what needs a real
     * return address. */
    arch_enter_user(entry, (uintptr_t)g_user_stack + 4096, 0, 0, 0);
}

static void usertest_body(void *arg)  { (void)arg; user_task_common(user_probe); }
static void intruder_body(void *arg)  { (void)arg; user_task_common(user_intruder); }
static void deputy_body(void *arg)    { (void)arg; user_task_common(user_deputy); }

static void run_user_task(const char *name, void (*body)(void *)) {
    g_user_entered = false;

    /* §3.1: the probe's U-mode stack, for exactly as long as the probe. */
    if (!scratch_acquire(&g_user_stack_sc, 4096)) {
        printk("[UserTest] No memory for the U-mode stack\n");
        return;
    }
    g_user_stack = (uint8_t *)g_user_stack_sc.base;

    int pid = task_create(name, body, NULL);
    if (pid < 0) {
        printk("[UserTest] Could not create the task\n");
        scratch_release(&g_user_stack_sc);
        g_user_stack = NULL;
        return;
    }
    /* Polls actual task state rather than spinning a fixed guessed count of
     * yields -- see cmd_taskdemo()'s comment (this same fix, applied
     * earlier) for why a fixed count assumes every yield here hands off
     * directly to the new task, which round-robin among other READY tasks
     * (p9srv above all) does not guarantee. A safety cap still bounds this
     * in case a probe somehow never reaches usys_exit(). */
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }

    /* Only reclaim once the task is genuinely gone. The loop above is capped,
     * so "we stopped waiting" and "it finished" are not the same thing, and
     * handing this page back while U-mode might still be running on it would
     * be far worse than leaking it. */
    if (sched_task_state(pid) == TASK_DEAD) {
        scratch_release(&g_user_stack_sc);
        g_user_stack = NULL;
    } else {
        printk("[UserTest] Task did not exit; keeping its stack rather than "
               "freeing memory still in use\n");
    }
}

static void cmd_usertest(void) {
    if (!mem_domain_enforced()) {
        printk("[UserTest] NOTE: this build cannot enforce domains (S-mode; Sv39 is B5).\n");
    }
    run_user_task("usertest", usertest_body);

    uintptr_t c = arch_last_ecall_cause();
    cprintf("\n[UserTest] Last ecall trap cause: %lu (%s)\n", (unsigned long)c,
           c == 8 ? "U-mode -- privilege level really dropped"
                  : "NOT U-mode -- the transition did not happen");
    printk("[UserTest] Returned to kernel mode; task ended cleanly.\n");
}

static void cmd_deputytest(void) {
    g_deputy_target = 0xFEEDFACE;
    printk("[Deputy] Kernel target at %p holds 0x%lx before.\n",
           (const void *)&g_deputy_target, (unsigned long)g_deputy_target);
    run_user_task("deputy", deputy_body);
    if (!g_user_entered) {
        cprintf("\n[Deputy] INCONCLUSIVE -- the task never entered U-mode.\n");
        return;
    }
    cprintf("\n[Deputy] Kernel target holds 0x%lx after -- %s\n",
           (unsigned long)g_deputy_target,
           g_deputy_target == 0xFEEDFACE ? "UNTOUCHED"
                                         : "OVERWRITTEN VIA THE KERNEL");
}

/* M5 Phase 2, plan/phase12_microkernel_migration.md: does a real client
 * blocking on chan_call() into a real U-mode server -- which itself blocks
 * mid-ecall-trap inside SYS_CHAN_SERVE_WAIT, waiting to be woken by that
 * same call -- actually work? Everything about SYS_CHAN_CALL's own client-
 * side blocking (chan_call_task()) was already proven on real hardware, but
 * that never exercised the *server* side blocking from inside a trap
 * handler: chan.h's only other task-owned-endpoint precedent ("console") is
 * an inline handler with no task_block() involved at all. Built and run
 * here, on every QEMU target, before trusting this mechanism on real
 * hardware -- exactly the "falsify on hardware, not QEMU" discipline this
 * project holds everywhere else, applied to the mechanism itself rather
 * than to a specific driver's PMP grant. */
/* Same treatment as g_user_stack above (§3.1): heap for the duration of the
 * `chanechotest` probe rather than permanent .bss. */
static scratch_t       g_echo_ustack_sc;
static uint8_t        *g_echo_ustack;
static mem_domain_t    g_echo_domain;
static uint8_t         g_echo_req[16];
static uint8_t         g_echo_resp[16];
static chan_endpoint_t *g_echo_ep;
static volatile bool    g_echo_entered;

__attribute__((always_inline)) static inline long echo_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long echo_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

UATTR static void echo_umode_body(void) {
    /* Not a string literal: a literal lands in ordinary .rodata, outside
     * every region this task's domain grants, so strncpy_from_user()
     * (arch/riscv/common/trap.c) correctly refuses to read it -- the same
     * failure user_deputy()'s own path[] comment already documents for
     * this file, applied here for the first time to a chan_serve_wait()/
     * chan_serve_reply() endpoint name. volatile, for the same reason
     * user_deputy()'s path[] is: gcc recognises a run of consecutive
     * stores and turns it back into a copy from a .rodata blob otherwise. */
    volatile char name[9];
    name[0]='c'; name[1]='h'; name[2]='a'; name[3]='n'; name[4]='e';
    name[5]='c'; name[6]='h'; name[7]='o'; name[8]='\0';

    uint8_t buf[16];
    long n = echo_usys_chan_serve_wait((const char *)name, buf, sizeof(buf));
    if (n < 0) n = 0;
    echo_usys_chan_serve_reply((const char *)name, buf, n);
    usys_exit();
    for (;;) { }
}

static void echo_task_body(void *arg) {
    (void)arg;
    /* task_create() can hand control to this task before
     * cmd_chan_echo_test() has gone on to call chan_register_task() --
     * same race drivers/tm1638_rp2350.c's own task_body() already guards
     * against with an identical wait. Without it, chan_lookup() inside
     * SYS_CHAN_SERVE_WAIT would find nothing yet. */
    while (!g_echo_ep) sched_yield();

    mem_domain_init(&g_echo_domain);
    mem_domain_add(&g_echo_domain, (uintptr_t)g_echo_ustack, 4096,
                   MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_echo_domain, tbase, tsize, MEM_R | MEM_X);

    if (task_set_domain(sched_current_pid(), &g_echo_domain) != 0) {
        printk("[ChanEcho] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_echo_entered = true;
    arch_enter_user(echo_umode_body, (uintptr_t)g_echo_ustack + 4096, 0, 0, 0);
}

static void cmd_chan_echo_test(void) {
    /* Refuse a repeat run before committing anything to it.
     *
     * kernel/chan.h has no unregister, so the endpoint this registers below
     * outlives the probe: a second `chanechotest` has always failed at
     * chan_register_task(). What it did *not* used to do was fail after
     * spawning a task and taking a page for its U-mode stack -- which, once
     * that stack became on-demand (§3.1), leaked a page per attempt, since a
     * live task is standing on it and freeing it would be worse than keeping
     * it. Detected here instead, where nothing has been claimed yet, and the
     * message says what is actually true rather than reporting a registration
     * failure the caller cannot act on. */
    if (chan_lookup("chanecho") != NULL) {
        printk("[ChanEcho] Endpoint already registered by an earlier run; "
               "reboot to run this again\n");
        return;
    }

    g_echo_entered = false;

    /* §3.1, same lifetime rule as run_user_task() above. */
    if (!scratch_acquire(&g_echo_ustack_sc, 4096)) {
        printk("[ChanEcho] No memory for the U-mode stack\n");
        return;
    }
    g_echo_ustack = (uint8_t *)g_echo_ustack_sc.base;

    int pid = task_create("chanecho", echo_task_body, NULL);
    if (pid < 0) {
        printk("[ChanEcho] Could not create the server task\n");
        scratch_release(&g_echo_ustack_sc);
        g_echo_ustack = NULL;
        return;
    }
    if (chan_register_task("chanecho", pid, g_echo_req, sizeof(g_echo_req),
                           g_echo_resp, sizeof(g_echo_resp)) != 0) {
        printk("[ChanEcho] Could not register the endpoint\n");
        return;   /* task is live on this stack; leave it held (see above) */
    }
    g_echo_ep = chan_lookup("chanecho");

    /* Give the server every chance to actually reach chan_serve_wait() and
     * block there before we call it -- not required for correctness
     * (chan_serve_wait() returns immediately if the request is already
     * pending either way), but this is deliberately trying to hit the
     * "server genuinely blocked mid-trap, then woken" case, not the race
     * where the request beats the server to its own wait call. */
    for (int i = 0; i < 10000 && !g_echo_entered; i++) sched_yield();
    for (int i = 0; i < 1000; i++) sched_yield();

    const char msg[] = "PING";
    uint8_t resp[16];
    printk("[ChanEcho] Calling chanecho...\n");
    int n = chan_call(g_echo_ep, (const uint8_t *)msg, sizeof(msg), resp, sizeof(resp));
    printk("[ChanEcho] chan_call returned %d\n", n);
    if (n == (int)sizeof(msg) && memcmp(resp, msg, sizeof(msg)) == 0) {
        printk("[ChanEcho] ECHO_OK\n");
    } else {
        printk("[ChanEcho] ECHO_MISMATCH\n");
    }

    /* The server task returns from chan_serve_wait() and exits once the call
     * above completes; give it the turns to do so, then reclaim -- and only
     * if it really finished, per run_user_task()'s reasoning. */
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    if (sched_task_state(pid) == TASK_DEAD) {
        scratch_release(&g_echo_ustack_sc);
        g_echo_ustack = NULL;
    } else {
        printk("[ChanEcho] Task did not exit; keeping its stack\n");
    }
}

static void cmd_usertest_isolation(void) {
    /* State the enforcement capability here, not only in usertest: this is
     * the command whose result is meaningless without it. A "BREACHED" line
     * with no explanation reads as a bug rather than as the documented state
     * of a build whose mechanism (Sv39) is still B5. */
    if (!mem_domain_enforced()) {
        printk("[Isolation] NOTE: this build cannot enforce domains (S-mode; Sv39 is B5),\n");
        printk("[Isolation]       so the write below is EXPECTED to succeed.\n");
    }
    g_kernel_canary = 0xC0FFEE;
    printk("[Isolation] Canary before: 0x%lx\n", (unsigned long)g_kernel_canary);
    run_user_task("intruder", intruder_body);

    if (!g_user_entered) {
        /* The task never reached U-mode, so the canary proves nothing about
         * isolation. Saying "ISOLATED" here would be a false positive of
         * exactly the kind this whole command exists to rule out. */
        printk("[Isolation] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[Isolation] Canary after:  0x%lx -- %s\n",
           (unsigned long)g_kernel_canary,
           g_kernel_canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                                       : "BREACHED (user task wrote kernel memory)");
}

#if defined(CONFIG_BOARD_RP2350)
/* M5 phase 1, plan/phase12_microkernel_migration.md: the same isolation
 * claim as isolationtest above, but against the real heartbeat driver
 * task's own domain shape (drivers/uart_rp2350.c) rather than the generic
 * 2-region usertest domain -- proving the SIO GPIO window a real,
 * long-lived driver task runs under is as narrow as intended, not just
 * that U-mode isolation works in the abstract. */
extern bool heartbeat_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_heartbeat_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[HeartbeatIso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = heartbeat_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[HeartbeatIso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[HeartbeatIso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[HeartbeatIso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
/* M5 Phase 2, plan/phase12_microkernel_migration.md: same idea as
 * heartbeatisotest above, against tm1638's own domain shape
 * (drivers/tm1638_rp2350.c) -- the first driver task converted to U-mode
 * that also serves a real chan_call() endpoint (heartbeat has none). */
extern bool tm1638_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_tm1638_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[TM1638Iso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = tm1638_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[TM1638Iso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[TM1638Iso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[TM1638Iso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

#if defined(CONFIG_BOARD_RP2350)
/* M5 Phase 3, plan/phase12_microkernel_migration.md: same idea as
 * heartbeatisotest/tm1638isotest above, against i2c's own domain shape
 * (drivers/i2c_rtc.c) -- the first driver task converted to U-mode that
 * talks to a real hardware controller (I2C0/I2C1) rather than bit-banged
 * GPIO. Not CONFIG_ENABLE_TM1638-gated like tm1638isotest: i2c_rtc.c is
 * built unconditionally for every RP2350 persona. */
extern bool i2c_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_i2c_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[I2CIso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = i2c_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[I2CIso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[I2CIso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[I2CIso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
/* Phase 17b, plan/phase17b_clock_task_split.md: same idea as
 * heartbeatisotest/tm1638isotest/i2cisotest above, against the clock
 * server's own domain shape (drivers/pico_clock_green_rp2350.c) -- the
 * last RP2350 driver task to reach U-mode, and the one that had to be
 * split in two before it could. Its domain is the only one in this list
 * whose stack shares a region with driver state (the RP2350 five-region
 * budget, with three MMIO windows already spoken for) and the only one
 * granting an MMIO window read-only (TIMER0: the server needs to know the
 * time, never to set it). */
extern bool clock_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_clock_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[ClockIso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = clock_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[ClockIso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[ClockIso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[ClockIso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
/* M5 Phase 4, plan/phase12_microkernel_migration.md: same idea as
 * heartbeatisotest/tm1638isotest/i2cisotest above, against st7735's own
 * domain shape (drivers/st7735_rp2350.c) -- the first driver task
 * converted to U-mode needing both an SIO grant (CS/DC/RST) and a
 * hardware-controller grant (SPI0) at once. */
extern bool st7735_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_st7735_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[ST7735Iso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = st7735_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[ST7735Iso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[ST7735Iso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[ST7735Iso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_SPISD
/* M5 Phase 5, plan/phase12_microkernel_migration.md: same idea as
 * heartbeatisotest/tm1638isotest/i2cisotest/st7735isotest above, against
 * blk's own domain shape (drivers/spisd_rp2350.c) -- SIO (CS) + SPI1
 * (the SD card's own PL022 controller, a second independent instance
 * from st7735's SPI0). */
extern bool blk_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_blk_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[BlkIso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = blk_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[BlkIso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[BlkIso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[BlkIso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

#if defined(CONFIG_BOARD_RP2350)
/* M5 Phase 6, plan/phase12_microkernel_migration.md: same idea as
 * heartbeatisotest/tm1638isotest/i2cisotest/st7735isotest/blkisotest
 * above, against uart's own domain shape (drivers/uart_rp2350.c) --
 * stack + text + UART0_BASE, no SIO grant (uart's runtime hot path
 * never touches GPIO). Not gated on a CONFIG_ENABLE_* flag: uart is
 * always built for RP2350, unlike the optional peripherals above. */
extern bool uart_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_uart_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[UartIso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = uart_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[UartIso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[UartIso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[UartIso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}

/* M5 Phase 7, plan/phase12_microkernel_migration.md: same idea as
 * heartbeatisotest/tm1638isotest/i2cisotest/st7735isotest/blkisotest/
 * uartisotest above, against usb_cdc's own domain shape
 * (drivers/usb_cdc.c) -- stack + text + USB_DPRAM_BASE + USB_BASE + the
 * combined g_usb_region state, the MEM_DOMAIN_MAX_REGIONS cap with zero
 * headroom left. Not gated on a CONFIG_ENABLE_* flag: usb_cdc is always
 * built for RP2350, same as uart. */
extern bool usb_cdc_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);

static void cmd_usb_isolation_test(void) {
    if (!mem_domain_enforced()) {
        printk("[UsbIso] NOTE: this build cannot enforce domains; the write below is EXPECTED to succeed.\n");
    }
    uintptr_t canary = 0;
    bool exited_clean = true;
    bool entered = usb_cdc_isolation_test(&canary, &exited_clean);
    if (!entered) {
        printk("[UsbIso] INCONCLUSIVE -- the task never entered U-mode; "
               "the canary was never at risk.\n");
        return;
    }
    printk("[UsbIso] Canary after: 0x%lx -- %s\n", (unsigned long)canary,
           canary == 0xC0FFEE ? "ISOLATED (kernel memory untouched)"
                              : "BREACHED (task wrote kernel memory)");
    printk("[UsbIso] Task exited cleanly: %s (expected: no -- the store should fault)\n",
           exited_clean ? "yes" : "no");
}
#endif

/* B6: preemption, tested by something that cannot work without it.
 *
 * The spinner never yields. The waiter never yields either. Under cooperative
 * scheduling whichever starts first runs to completion and the other never
 * advances -- so if the flag is ever observed set, a timer interrupt must have
 * switched tasks at an arbitrary instruction. That is the whole claim, and it
 * is not something a passing "tasks interleave" test could already cover:
 * taskdemo's tasks yield explicitly.
 *
 * Both loops are bounded, so a failure reports rather than wedging the
 * machine and taking every later test down with it. */
static volatile uint32_t g_preempt_flag;

static void preempt_spinner(void *arg) {
    (void)arg;
    for (volatile uint32_t i = 0; i < 300000u; i++) { /* no yield, on purpose */ }
    g_preempt_flag = 1;
}

static void cmd_preempttest(void) {
    if (!ticker_enabled()) {
        cprintf("[Preempt] No preemption timer on this build -- cannot test.\n");
        return;
    }
    g_preempt_flag = 0;
    uint64_t t0 = ticker_ticks();

    if (task_create("spinner", preempt_spinner, NULL) < 0) {
        cprintf("[Preempt] Could not create the spinner task\n");
        return;
    }

    /* Deliberately no sched_yield() in this loop. */
    volatile uint64_t spins = 0;
    while (!g_preempt_flag && spins < 400000000ULL) spins++;

    uint64_t ticks = ticker_ticks() - t0;
    cprintf("[Preempt] ticks=%lu flag=%u -- %s\n",
            (unsigned long)ticks, (unsigned)g_preempt_flag,
            g_preempt_flag ? "PREEMPTED (a task ran without anyone yielding)"
                           : "NOT PREEMPTED");
}

/* M3, plan/phase12_microkernel_migration.md: proves next_runnable() picks by
 * priority tier, not merely by ring position. Four never-yielding hogs are
 * created first specifically to sit in the ring between the boot task and
 * "urgent" -- under the old plain round-robin, urgent would have to wait its
 * turn behind however many hogs came first; under tier-aware selection its
 * TASK_PRIO_INTERRUPT must win the very next reschedule regardless of how
 * many NORMAL-tier tasks are ahead of it. Both hog and urgent loops are
 * bounded, same reason as preempttest's: a failure reports rather than
 * wedging the machine.
 *
 * g_urgent_ran is uint32_t, not bool -- found the hard way while building
 * this test. A `volatile bool` flag set by urgent and polled by a
 * non-yielding busy-wait in the boot task was not reliably observed as
 * updated after the boot task was preempted mid-spin and later resumed:
 * the task genuinely ran (visible in the log) and the flag was genuinely
 * set, but the resumed spin loop kept spinning to its full bound as though
 * it never had been. Switching only the flag's type to `uint32_t` --
 * nothing else, verified by isolating every other variable one at a time --
 * made it reliable. preempttest's own g_preempt_flag already happened to be
 * uint32_t, which is presumably why B6 never hit this. Not chased to a root
 * cause in the toolchain; recorded as a real constraint instead of
 * papered over: a volatile flag polled across a preemption boundary in this
 * tree should be word-sized, not bool. */
static volatile uint32_t g_urgent_ran;
static volatile uint64_t g_urgent_woken_tick;

static void priotest_hog_body(void *arg) {
    (void)arg;
    /* No yield, on purpose -- these exist purely to occupy ring slots a
     * plain round-robin would have to cycle through, and must still be
     * READY (not already exited) at the moment urgent is unblocked, or this
     * test would pass without ever actually exercising the tie-break it
     * claims to. Bounded so they don't outlive the command by much and
     * dilute round-robin for whatever runs next. */
    for (volatile uint32_t i = 0; i < 20000000u; i++) { }
}

static void priotest_urgent_body(void *arg) {
    (void)arg;
    task_block(); /* parks until cmd_priotest() unblocks it below */
    g_urgent_woken_tick = ticker_ticks();
    g_urgent_ran = 1;
}

static void cmd_priotest(void) {
    if (!ticker_enabled()) {
        cprintf("[PrioTest] No preemption timer on this build -- cannot test.\n");
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (task_create("hog", priotest_hog_body, NULL) < 0) {
            cprintf("[PrioTest] Could not create hog tasks\n");
            return;
        }
    }

    int urgent = task_create("urgent", priotest_urgent_body, NULL);
    if (urgent < 0 || task_set_priority(urgent, TASK_PRIO_INTERRUPT) < 0) {
        cprintf("[PrioTest] Could not create/prioritize the urgent task\n");
        return;
    }

    /* Let urgent actually reach its task_block() -- it must be BLOCKED, not
     * merely READY, before task_unblock() below means anything. Urgent's
     * TASK_PRIO_INTERRUPT wins it the very first of these yields regardless
     * of the hogs, so a handful is generous margin, not a real wait. */
    for (int i = 0; i < 8; i++) sched_yield();

    g_urgent_ran = 0;
    uint64_t before = ticker_ticks();
    task_unblock(urgent);

    /* Deliberately no sched_yield() here: the hogs never yield either, so
     * only the preemption timer -- and now, which tier it picks -- can give
     * urgent a chance to run. */
    volatile uint64_t spins = 0;
    while (!g_urgent_ran && spins < 400000000ULL) spins++;

    uint64_t ticks_to_run = g_urgent_ran ? (g_urgent_woken_tick - before) : 0;
    cprintf("[PrioTest] ran=%u ticks_to_run=%lu -- %s\n",
            (unsigned)g_urgent_ran, (unsigned long)ticks_to_run,
            (g_urgent_ran && ticks_to_run <= 2) ? "BOUNDED (priority won)"
                                                : "NOT BOUNDED");
}

/* M4.5, plan/phase12_microkernel_migration.md, Part A: priotest above only
 * ever proves one TASK_PRIO_INTERRUPT task against a field of NORMAL-tier
 * hogs -- it says nothing about what happens when *two* INTERRUPT-tier
 * tasks are both genuinely busy at once, which M4's driver-task
 * conversions are about to make a real, not hypothetical, situation (more
 * than one driver task plausibly wants this tier). That case has never
 * actually been exercised: `uart` is the only TASK_PRIO_INTERRUPT task
 * that has ever existed in this tree.
 *
 * Same self-measurement idea user/progs/uspin.c uses (read the tick
 * counter, spin with no yields and no syscalls, read it again), run by two
 * same-tier tasks concurrently instead of one task measuring itself in
 * isolation. Deliberately does NOT add any bookkeeping to kernel/sched.c
 * to measure this -- the whole point is to find out whether the tiers
 * already built are sufficient using only what a task can observe about
 * itself, not to build new scheduler introspection for the occasion. If
 * next_runnable()'s plain round-robin tie-break shares the CPU between two
 * equal-tier ready tasks the way it is supposed to, both should finish
 * their equal, fixed amount of work at close to the same tick -- each
 * effectively getting about half of it, interleaved. If one task-table
 * slot is favoured over the other (the specific failure shape table
 * position bias took before, when a repeatedly-reused server task's own
 * index was the scan-start -- see kernel/sched.c's next_runnable()
 * comment), the favoured task finishes quickly while the other is starved
 * until it, which shows up as a large gap between the two finish ticks
 * instead of a small one. */
static volatile uint32_t g_stress_done[2];
static volatile uint64_t g_stress_finish_tick[2];

/* Same order of magnitude as uspin.c's SPIN_ITERATIONS, not copied
 * verbatim: this runs in kernel mode (no syscall trap overhead at all,
 * unlike a U-mode program) and must comfortably span dozens of ticks even
 * when *sharing* the CPU with an equally-busy sibling, not just when
 * running alone -- calibrated on QEMU, see cmd_priostress()'s own
 * completion notes for measured values before trusting this margin on
 * real RP2350 hardware too. */
#define PRIOSTRESS_ITERATIONS 150000000L

static volatile long g_stress_sink[2]; /* per-task: keeps the optimizer from eliminating the loop below without sharing a write target between the two concurrently-running tasks */

static void priostress_body(void *arg) {
    int id = (int)(intptr_t)arg;
    /* Overwrite, not accumulate: `long` is 32 bits on rv32, and summing i
     * across PRIOSTRESS_ITERATIONS iterations overflows it almost
     * immediately -- UBSan caught this as a real fault (signed integer
     * addition overflow) the first time this ran on rv32. Only a volatile
     * write is actually needed to keep the optimizer from eliminating the
     * loop; the value itself was never meaningful. */
    for (long i = 0; i < PRIOSTRESS_ITERATIONS; i++) g_stress_sink[id] = i;
    g_stress_finish_tick[id] = ticker_ticks();
    g_stress_done[id] = 1;
}

static void cmd_priostress(void) {
    if (!ticker_enabled()) {
        cprintf("[PrioStress] No preemption timer on this build -- cannot test.\n");
        return;
    }

    g_stress_done[0] = 0;
    g_stress_done[1] = 0;
    g_stress_finish_tick[0] = 0;
    g_stress_finish_tick[1] = 0;

    int a = task_create("stressA", priostress_body, (void *)(intptr_t)0);
    int b = task_create("stressB", priostress_body, (void *)(intptr_t)1);
    if (a < 0 || b < 0 ||
        task_set_priority(a, TASK_PRIO_INTERRUPT) < 0 ||
        task_set_priority(b, TASK_PRIO_INTERRUPT) < 0) {
        cprintf("[PrioStress] Could not create/prioritize both stress tasks\n");
        return;
    }

    uint64_t start = ticker_ticks();

    /* Bounded the same way preempttest/priotest are: a failure reports
     * rather than wedging the machine. Generous relative to either task's
     * own expected duration -- this is a safety cap on the *test*, not
     * part of the fairness measurement itself, which is the tick gap
     * computed below. */
    for (int i = 0; i < 20000 &&
                    (sched_task_state(a) != TASK_DEAD || sched_task_state(b) != TASK_DEAD);
         i++) {
        sched_yield();
    }

    uint64_t total_a = g_stress_done[0] ? g_stress_finish_tick[0] - start : 0;
    uint64_t total_b = g_stress_done[1] ? g_stress_finish_tick[1] - start : 0;
    uint64_t lo = total_a < total_b ? total_a : total_b;
    uint64_t hi = total_a < total_b ? total_b : total_a;
    /* Fair sharing: both finish within a small factor of each other (some
     * skew is expected -- they don't start in exactly the same tick, and
     * ties still resolve by scan order). One task starved until the other
     * finishes looks like hi ~= 2*lo (the starved task waits out ~all of
     * the other's run, then does its own on top) -- comfortably outside
     * this bound. */
    bool fair = g_stress_done[0] && g_stress_done[1] && lo > 0 && hi <= lo + lo / 2;
    cprintf("[PrioStress] done=%u,%u total_ticks=%lu,%lu -- %s\n",
            (unsigned)g_stress_done[0], (unsigned)g_stress_done[1],
            (unsigned long)total_a, (unsigned long)total_b,
            fair ? "FAIR (both same-tier tasks shared the CPU)"
                 : "UNFAIR (one task-table slot starved the other)");
}

static void cmd_taskdemo(void) {
    uint32_t before_total = 0, before_free = 0;
    palloc_stats(&before_total, &before_free);

    int a = task_create("demoA", taskdemo_body, (void *)"A");
    int b = task_create("demoB", taskdemo_body, (void *)"B");
    if (a < 0 || b < 0) {
        printk("[TaskDemo] Could not create both tasks\n");
        return;
    }

    /* Drive the demo from this (the boot) task by yielding until both have
     * actually exited, not for a fixed guessed number of turns -- a fixed
     * count assumes every yield from here hands off directly to demoA/demoB,
     * which round-robin among other READY tasks (p9srv above all) does not
     * guarantee. With a fixed count, "Done" could fire while both tasks were
     * still alive and mid-exchange, reporting pages still legitimately in
     * use by their live stacks as though they'd leaked. A safety cap still
     * bounds this -- if task creation somehow left one of them permanently
     * unschedulable, this must not hang the shell. */
    for (int spin = 0;
         spin < 10000 &&
         (sched_task_state(a) != TASK_DEAD || sched_task_state(b) != TASK_DEAD);
         spin++) {
        sched_yield();
    }

    uint32_t after_total = 0, after_free = 0;
    palloc_stats(&after_total, &after_free);
    printk("[TaskDemo] Done. Heap pages free before=%u after=%u\n",
           before_free, after_free);
}

/* M0, plan/phase12_microkernel_migration.md: proves task_create_sized() works
 * at a stack size other than TASK_STACK_PAGES, and that palloc_free() is
 * called for exactly the number of pages that were actually allocated (not
 * TASK_STACK_PAGES, which sched.c's task_exit()/sched_reap() path used to be
 * able to assume back when every task had the same size). One page is
 * enough for this body -- one printk/sched_yield frame, nothing recursive. */
static void sizedtaskdemo_body(void *arg) {
    (void)arg;
    printk("[SizedTaskDemo] Running on a 1-page stack\n");
    sched_yield();
}

static void cmd_sizedtaskdemo(void) {
    uint32_t before_total = 0, before_free = 0;
    palloc_stats(&before_total, &before_free);

    int pid = task_create_sized("sized1", sizedtaskdemo_body, NULL, 1);
    if (pid < 0) {
        printk("[SizedTaskDemo] Could not create the task\n");
        return;
    }

    /* Drive it to completion the same way cmd_taskdemo() does -- see that
     * function's comment for why this polls sched_task_state() rather than
     * spinning a fixed count of yields. before_free/after_free matching
     * (asserted by tests/runner.py) is the actual proof that a non-default
     * page count was both allocated and freed correctly -- a leak here
     * would show up as a page short on the "after" count. */
    for (int spin = 0; spin < 10000 && sched_task_state(pid) != TASK_DEAD; spin++) {
        sched_yield();
    }

    uint32_t after_total = 0, after_free = 0;
    palloc_stats(&after_total, &after_free);
    printk("[SizedTaskDemo] Done. Heap pages free before=%u after=%u\n",
           before_free, after_free);
}

/* M1, plan/phase12_microkernel_migration.md: exercises balloc_alloc()/
 * balloc_free() -- rounding to the next power of two, self-alignment to
 * that size (Rule 6: this is what makes a block usable as a PMP region
 * later), and coalescing all the way back to one arena-sized free block
 * after a mixed-size churn. A leftover fragment at the end would mean
 * split or merge lost track of a block somewhere in the run. */
static void cmd_ballocdemo(void) {
    uint32_t arena_bytes = 0, free_before = 0;
    balloc_stats(&arena_bytes, &free_before);

    void *a = balloc_alloc(33);   /* rounds up to 64 B */
    void *b = balloc_alloc(65);   /* rounds up to 128 B */
    void *c = balloc_alloc(1025); /* rounds up to 2048 B */
    if (!a || !b || !c) {
        printk("[BAllocDemo] Allocation failed\n");
        return;
    }
    bool aligned = ((uintptr_t)a % 64) == 0 &&
                  ((uintptr_t)b % 128) == 0 &&
                  ((uintptr_t)c % 2048) == 0;
    printk("[BAllocDemo] Rounding/alignment: %s\n", aligned ? "OK" : "FAIL");

    balloc_free(a);
    balloc_free(b);
    balloc_free(c);

    /* Churn: repeatedly allocate and free a mix of sizes across a fixed set
     * of slots, so split and merge are stressed together rather than one
     * pair at a time. */
    static const uint32_t sizes[8] = {32, 96, 200, 500, 1500, 4000, 100, 60};
    void *slots[8] = {0};
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 8; i++) {
            if (slots[i]) {
                balloc_free(slots[i]);
                slots[i] = NULL;
            } else {
                slots[i] = balloc_alloc(sizes[(i + round) % 8]);
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        if (slots[i]) { balloc_free(slots[i]); slots[i] = NULL; }
    }

    uint32_t free_after = 0;
    balloc_stats(NULL, &free_after);
    printk("[BAllocDemo] Done. After churn: largest=%u arena=%u\n",
           free_after, arena_bytes);
}

/* `e`'s edit buffer used to be a fixed 2 KB stack array (bug: init.lisp is
 * 4378 bytes and silently lost its tail). Heap-on-demand instead, sized to
 * the file it is about to load rather than a guess -- room for the file to
 * roughly double under editing, rounded up to a page, with a floor for new
 * files. Refuses outright on allocation failure rather than falling back to
 * a smaller buffer that would just reintroduce the truncation. */
#define EDITOR_MIN_CAPACITY  8192u
static void shell_run_editor(const char *filename) {
    vfs_stat_t st;
    uint32_t existing = (vfs_stat(filename, &st) == 0) ? st.size : 0;
    uint32_t want = existing * 2;
    if (want < EDITOR_MIN_CAPACITY) want = EDITOR_MIN_CAPACITY;

    uint32_t pages = (want + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    char *edit_buf = (char *)palloc_pages(pages);
    if (!edit_buf) {
        cprintf("e: no memory for a %u KB edit buffer (file is %u bytes)\n",
                pages * (uint32_t)PAGE_SIZE / 1024, existing);
        return;
    }

    int mlen = edit_multiline_box(filename, edit_buf, (int)(pages * PAGE_SIZE));
    if (mlen > 0) {
        lisp_val_t *res = lisp_eval_string(edit_buf);
        if (res) {
            if (res->type != LISP_NIL) {
                cprintf("=> ");
                lisp_print(res);
                cprintf("\n");
            } else {
                cprintf("\n");
            }
        }
    }
    palloc_free(edit_buf, pages);
}

static void parse_and_eval_cmd(const char *cmd_line) {
    while (*cmd_line == ' ' || *cmd_line == '\t') cmd_line++;
    if (*cmd_line == '\0') return;

    if (strcmp(cmd_line, "help") == 0) {
        cmd_help();
        return;
    } else if (strcmp(cmd_line, "uname") == 0) {
        cmd_uname();
        return;
    } else if (strcmp(cmd_line, "reboot") == 0) {
        /* A builtin, and its position in this chain is the point: it is
         * handled here, before the Lisp fallthrough, so it still works when
         * the evaluator has stopped answering. That is exactly the state the
         * hardware suite's node-pool test leaves the board in, and rebooting
         * out of it is the whole reason this command exists (C5). */
        cprintf("[Reboot] Restarting...\n");
        if (!board_reset()) {
            cprintf("reboot: not supported on this target\n");
        }
        return;
    } else if (strcmp(cmd_line, "clear") == 0) {
        uart_puts("\033[2J\033[H");
        return;
#if CONFIG_ENABLE_ED
    } else if (strcmp(cmd_line, "ed") == 0) {
        ed_main(NULL);
        return;
    } else if (strncmp(cmd_line, "ed ", 3) == 0) {
        ed_main(&cmd_line[3]);
        return;
#endif
    } else if (strcmp(cmd_line, "e") == 0) {
        shell_run_editor("/ram0/system/scratch.lisp");
        return;
    } else if (strncmp(cmd_line, "e ", 2) == 0) {
        const char *fn = &cmd_line[2];
        while (*fn == ' ') fn++;
        shell_run_editor(fn);
        return;
    } else if (strcmp(cmd_line, "hmacselftest") == 0) {
        sha256_selftest();
        return;
    } else if (strcmp(cmd_line, "randtest") == 0) {
        random_selftest(4096);
        return;
    } else if (strncmp(cmd_line, "randtest ", 9) == 0) {
        unsigned bits = 0;
        for (const char *d = &cmd_line[9]; *d >= '0' && *d <= '9'; d++)
            bits = bits * 10u + (unsigned)(*d - '0');
        random_selftest(bits);
        return;
    } else if (strcmp(cmd_line, "idstoreselftest") == 0) {
        idstore_selftest();
        return;
    } else if (strcmp(cmd_line, "identity") == 0) {
        cmd_identity(NULL);
        return;
    } else if (strncmp(cmd_line, "identity ", 9) == 0) {
        cmd_identity(&cmd_line[9]);
        return;
    } else if (strcmp(cmd_line, "peers") == 0) {
        cmd_peers(NULL);
        return;
    } else if (strncmp(cmd_line, "peers ", 6) == 0) {
        cmd_peers(&cmd_line[6]);
        return;
    } else if (strcmp(cmd_line, "wlan") == 0) {
        cmd_wlan(NULL);
        return;
    } else if (strncmp(cmd_line, "wlan ", 5) == 0) {
        cmd_wlan(&cmd_line[5]);
        return;
    } else if (strcmp(cmd_line, "netcfg") == 0) {
        cmd_netcfg(NULL);
        return;
    } else if (strncmp(cmd_line, "netcfg ", 7) == 0) {
        cmd_netcfg(&cmd_line[7]);
        return;
    } else if (strcmp(cmd_line, "dcf77selftest") == 0) {
        dcf77_selftest();
        return;
    } else if (strcmp(cmd_line, "clockuiselftest") == 0) {
        clock_ui_selftest();
        return;
    } else if (strcmp(cmd_line, "i2c") == 0 || strcmp(cmd_line, "i2c scan") == 0) {
        i2c_scan_bus();
        return;
    } else if (strcmp(cmd_line, "date") == 0) {
        /* Local first, because that is what the question means, and UTC
         * underneath, because that is what the clock actually holds. */
        rtc_time_t utc, loc;
        char isostr[32];
        const char *abbrev = "";
        time_get_utc(&utc);
        tz_utc_to_local(&utc, &loc);
        tz_offset_min(&utc, &abbrev, NULL);
        time_format_iso(&loc, isostr, sizeof(isostr));
        cprintf("%s %s\n", isostr, abbrev);
        time_format_iso(&utc, isostr, sizeof(isostr));
        cprintf("%s UTC  (TZ=%s)\n", isostr, tz_get());
        return;
    } else if (strncmp(cmd_line, "date ", 5) == 0) {
        const char *arg = &cmd_line[5];
        while (*arg == ' ') arg++;
        rtc_time_t tm;
        if (time_parse_iso(arg, &tm)) {
            /* Typed by a human, so it is local time. The DS3231 gets UTC --
             * it is storage, not a display. */
            rtc_time_t utc;
            tz_local_to_utc(&tm, &utc);
            time_set_utc(&utc);
            i2c_rtc_write_time(&utc);
            char isostr[32];
            time_format_iso(&utc, isostr, sizeof(isostr));
            cprintf("System clock updated: %s local = %s UTC\n", arg, isostr);
        } else {
            cprintf("Invalid date format. Expected: YYYY-MM-DD HH:MM:SS\n");
        }
        return;
    } else if (strcmp(cmd_line, "tz") == 0) {
        rtc_time_t utc;
        const char *abbrev = "";
        bool dst = false;
        time_get_utc(&utc);
        int off = tz_offset_min(&utc, &abbrev, &dst);
        cprintf("TZ=%s\n", tz_get());
        cprintf("  now %s, UTC%c%02d:%02d, %s\n", abbrev,
                off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60,
                (off < 0 ? -off : off) % 60,
                dst ? "summer time" : "standard time");
        return;
    } else if (strncmp(cmd_line, "tz ", 3) == 0) {
        const char *arg = &cmd_line[3];
        while (*arg == ' ') arg++;
        if (tz_set(arg)) cprintf("Timezone set: %s\n", tz_get());
        else cprintf("Not a POSIX TZ rule: '%s' (kept %s).\n"
                     "Expected e.g. CET-1CEST,M3.5.0,M10.5.0/3 or UTC0\n", arg, tz_get());
        return;
    } else if (strcmp(cmd_line, "tzselftest") == 0) {
        tz_selftest();
        return;
    } else if (strcmp(cmd_line, "lisp") == 0) {
        lisp_repl();
        return;
    } else if (strcmp(cmd_line, "p9serve") == 0) {
        cmd_p9serve();
        return;
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_UART1_BASE)
    } else if (strcmp(cmd_line, "uart1pins") == 0) {
        uart1_pin_test();
        return;
    } else if (strncmp(cmd_line, "uart1test", 9) == 0) {
        unsigned ms = 0;
        for (const char *d = &cmd_line[9]; ; d++) {
            if (*d == ' ') continue;
            if (*d < '0' || *d > '9') break;
            ms = ms * 10u + (unsigned)(*d - '0');
        }
        uart1_wire_test(ms);
        return;
#endif
    } else if (strcmp(cmd_line, "net") == 0) {
        cmd_net_status();
        return;
    } else if (strcmp(cmd_line, "ntp") == 0) {
        cmd_ntp(NULL);
        return;
    } else if (strncmp(cmd_line, "ntp ", 4) == 0) {
        cmd_ntp(&cmd_line[4]);
        return;
    } else if (strncmp(cmd_line, "net txtest", 10) == 0) {
        cmd_net_txtest(shell_trailing_uint(&cmd_line[10]));
        return;
    } else if (strncmp(cmd_line, "net listen", 10) == 0) {
        unsigned port = shell_trailing_uint(&cmd_line[10]);
        const char *rest = &cmd_line[10];
        while (*rest == ' ') rest++;
        if (*rest == '0') {
            tcp_unlisten();
            cprintf("net: no longer listening\n");
        } else if (tcp_listen((uint16_t)(port ? port : 564u)) == 0) {
            cprintf("net: listening for 9P on tcp/%u\n", port ? port : 564u);
        } else {
            cprintf("net: could not listen\n");
        }
        return;
    } else if (strncmp(cmd_line, "net udpecho", 11) == 0) {
        cmd_net_udpecho(shell_trailing_uint(&cmd_line[11]));
        return;
    } else if (strncmp(cmd_line, "net rxtest", 10) == 0) {
        cmd_net_rxtest(shell_trailing_uint(&cmd_line[10]));
        return;
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_ETH_CS_GPIO)
    } else if (strcmp(cmd_line, "net regs") == 0) {
        enc28j60_dump_regs();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_WL_CS_GPIO)
    } else if (strcmp(cmd_line, "wifi trace on") == 0 || strcmp(cmd_line, "wifi trace off") == 0) {
        extern bool g_event_trace;
        g_event_trace = (cmd_line[11] == 'o' && cmd_line[12] == 'n');
        cprintf("wifi: event trace %s\n", g_event_trace ? "on" : "off");
        return;
    } else if (strcmp(cmd_line, "wifi probe") == 0) {
        cprintf(cyw43_gspi_probe() ? "wifi: ready\n" : "wifi: no answer\n");
        return;
    } else if (strncmp(cmd_line, "wifi ", 5) == 0 && !cyw43_is_ready() &&
               strcmp(cmd_line, "wifi probe") != 0) {
        /* Everything except `wifi probe` needs the firmware running --
         * the radio does nothing until ~230 KB has been uploaded to it.
         * Say so once, here, rather than letting each subcommand fail
         * with a bus-level timeout the reader then has to interpret. */
        cprintf("wifi: the radio is not up yet -- run `wifi probe` first "
                "(it resets the chip and uploads its firmware)\n");
        return;
    } else if (strncmp(cmd_line, "wifi join ", 10) == 0) {
        /* Explicit form: `wifi join <ssid> <64-hex-psk>`. Takes the
         * *derived* PSK, never a passphrase -- same rule as the stored
         * record (I6), so nothing here has to hash and nothing a
         * passphrase could leak is ever typed. Exists because the
         * identity store has no RP2350 backend yet (I7), so the stored
         * form below cannot work on this board.  */
        const char *a = &cmd_line[10];
        while (*a == ' ') a++;
        char ssid[NODE_WLAN_SSID_MAX + 1];
        uint32_t sl = 0;
        while (*a && *a != ' ' && sl < NODE_WLAN_SSID_MAX) ssid[sl++] = *a++;
        ssid[sl] = '\0';
        while (*a == ' ') a++;

        uint8_t psk[NODE_WLAN_PSK_LEN];
        uint32_t got = parse_hex_bytes(a, psk, sizeof(psk));
        if (sl == 0 || got != NODE_WLAN_PSK_LEN) {
            cprintf("usage: wifi join <ssid> <64-hex-char psk>\n");
            return;
        }
        bool ok = cyw43_join_wpa2(ssid, psk);
        memset(psk, 0, sizeof(psk));
        cprintf(ok ? "wifi: joined\n" : "wifi: join failed\n");
        return;
    } else if (strcmp(cmd_line, "wifi join") == 0) {
        /* Credentials come from the identity record (I6): the SSID and a
         * *derived* PSK. The passphrase was never stored, so there is
         * nothing here to leak and nothing to prompt for. */
        char ssid[NODE_WLAN_SSID_MAX + 1];
        uint8_t psk[NODE_WLAN_PSK_LEN];
        if (!node_wlan_ssid(ssid, sizeof(ssid))) {
            cprintf("wifi: no SSID stored (this board has no identity store yet -- "
                    "phase 21's I7). Use: wifi join <ssid> <64-hex psk>\n");
            return;
        }
        if (!node_wlan_psk(psk)) {
            cprintf("wifi: no PSK stored for \"%s\"\n", ssid);
            return;
        }
        bool ok = cyw43_join_wpa2(ssid, psk);
        memset(psk, 0, sizeof(psk));
        cprintf(ok ? "wifi: joined\n" : "wifi: join failed\n");
        return;
    } else if (strcmp(cmd_line, "wifi stats") == 0) {
        cprintf("wifi: rx ring high-water %u, %u frames dropped (ring full)\n",
                cyw43_rx_high_water(), cyw43_rx_overruns());
        return;
    } else if (strncmp(cmd_line, "wifi led", 8) == 0) {
        /* The LED is on the wireless chip's own GPIO 0, so this only does
         * anything once `wifi probe` has the firmware running -- which is
         * the point: it is the one check that exercises the whole stack,
         * bus through firmware through ioctl, and reports by lighting up. */
        const char *arg = &cmd_line[8];
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0) {
            cprintf(cyw43_led_set(true) ? "wifi: led on\n" : "wifi: led failed\n");
        } else if (strcmp(arg, "off") == 0) {
            cprintf(cyw43_led_set(false) ? "wifi: led off\n" : "wifi: led failed\n");
        } else {
            for (int i = 0; i < 6; i++) {
                if (!cyw43_led_set((i & 1) == 0)) {
                    cprintf("wifi: led failed\n");
                    return;
                }
                time_delay_us(300000);
            }
            cprintf("wifi: blinked\n");
        }
        return;
#endif
    } else if (strcmp(cmd_line, "p9key") == 0) {
        cmd_p9key(NULL);
        return;
    } else if (strncmp(cmd_line, "p9key ", 6) == 0) {
        cmd_p9key(&cmd_line[6]);
        return;
    } else if (strcmp(cmd_line, "p9authselftest") == 0) {
        p9_auth_selftest();
        return;
    } else if (strcmp(cmd_line, "p9auth") == 0) {
        cmd_p9auth(NULL);
        return;
    } else if (strncmp(cmd_line, "p9auth ", 7) == 0) {
        cmd_p9auth(&cmd_line[7]);
        return;
    } else if (strcmp(cmd_line, "p9share") == 0) {
        cmd_p9share(NULL);
        return;
    } else if (strncmp(cmd_line, "p9share ", 8) == 0) {
        cmd_p9share(&cmd_line[8]);
        return;
    } else if (strcmp(cmd_line, "pmpinfo") == 0) {
        pmp_report();
        return;
    } else if (strcmp(cmd_line, "usertest") == 0) {
        cmd_usertest();
        return;
    } else if (strcmp(cmd_line, "isolationtest") == 0) {
        cmd_usertest_isolation();
        return;
#if defined(CONFIG_BOARD_RP2350)
    } else if (strcmp(cmd_line, "heartbeatisotest") == 0) {
        cmd_heartbeat_isolation_test();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    } else if (strcmp(cmd_line, "tm1638isotest") == 0) {
        cmd_tm1638_isolation_test();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350)
    } else if (strcmp(cmd_line, "i2cisotest") == 0) {
        cmd_i2c_isolation_test();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    } else if (strcmp(cmd_line, "st7735isotest") == 0) {
        cmd_st7735_isolation_test();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_SPISD
    } else if (strcmp(cmd_line, "blkisotest") == 0) {
        cmd_blk_isolation_test();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350)
    } else if (strcmp(cmd_line, "uartisotest") == 0) {
        cmd_uart_isolation_test();
        return;
    } else if (strcmp(cmd_line, "usbisotest") == 0) {
        cmd_usb_isolation_test();
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    } else if (strcmp(cmd_line, "clockisotest") == 0) {
        cmd_clock_isolation_test();
        return;
#endif
    } else if (strcmp(cmd_line, "deputytest") == 0) {
        cmd_deputytest();
        return;
    } else if (strcmp(cmd_line, "chanechotest") == 0) {
        cmd_chan_echo_test();
        return;
    } else if (strcmp(cmd_line, "pmpdump") == 0) {
        pmp_dump();
        return;
    } else if (strcmp(cmd_line, "preempttest") == 0) {
        cmd_preempttest();
        return;
    } else if (strcmp(cmd_line, "priotest") == 0) {
        cmd_priotest();
        return;
    } else if (strcmp(cmd_line, "priostress") == 0) {
        cmd_priostress();
        return;
    } else if (strcmp(cmd_line, "taskdemo") == 0) {
        cmd_taskdemo();
        return;
    } else if (strcmp(cmd_line, "sizedtaskdemo") == 0) {
        cmd_sizedtaskdemo();
        return;
    } else if (strcmp(cmd_line, "ballocdemo") == 0) {
        cmd_ballocdemo();
        return;
    } else if (strcmp(cmd_line, "uartstats") == 0) {
        /* M4 verify, plan/phase12_microkernel_migration.md: exposes
         * uart_write_call_count() so a test can confirm console output
         * generates chan_call() traffic on the order of messages/lines,
         * not characters -- the property the batching redesign exists to
         * guarantee. */
        printk("[UartStats] write_calls=%u\n", uart_write_call_count());
        return;
    } else if (strcmp(cmd_line, "blkstats") == 0) {
        /* M4.5 verify, plan/phase12_microkernel_migration.md, Part B:
         * exposes blk_task_call_count() so a test can confirm the sdblk/blk
         * task is genuinely serving requests (a nonzero, growing count)
         * rather than every caller silently using the direct-hardware
         * fallback the whole time. */
        printk("[BlkStats] calls=%u\n", blk_task_call_count());
        return;
    } else if (strcmp(cmd_line, "i2cstats") == 0) {
        /* M4.5 verify, plan/phase12_microkernel_migration.md, Part B:
         * exposes i2c_task_call_count() so a test can confirm the shared
         * RTC/EEPROM task is genuinely serving requests, same reasoning as
         * blkstats above. */
        printk("[I2cStats] calls=%u\n", i2c_task_call_count());
        return;
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    } else if (strcmp(cmd_line, "st7735stats") == 0) {
        // M4.5 verify, plan/phase12_microkernel_migration.md, Part B: same
        // reasoning as blkstats/i2cstats above.
        printk("[St7735Stats] calls=%u\n", st7735_task_call_count());
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    } else if (strcmp(cmd_line, "tm1638stats") == 0) {
        printk("[Tm1638Stats] calls=%u\n", tm1638_task_call_count());
        return;
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    } else if (strcmp(cmd_line, "clockstats") == 0) {
        printk("[ClockStats] calls=%u\n", pico_clock_green_task_call_count());
        return;
#endif
    } else if (strcmp(cmd_line, "klog") == 0) {
        cmd_klog(NULL);
        return;
    } else if (strncmp(cmd_line, "klog ", 5) == 0) {
        cmd_klog(&cmd_line[5]);
        return;
    }


    /* Direct S-Expression evaluation if line starts with '(' */
    if (*cmd_line == '(') {
        lisp_val_t *res = lisp_eval_string(cmd_line);
        if (res) {
            if (res->type != LISP_NIL) {
                cprintf("=> ");
                lisp_print(res);
                cprintf("\n");
            } else {
                cprintf("\n");
            }
        }
        return;
    }


    /* Single token variable or evaluation lookup */
    bool has_space = false;
    for (const char *c = cmd_line; *c; c++) {
        if (*c == ' ' || *c == '\t') { has_space = true; break; }
    }

    /* A bare word that names a program on the search path runs it (C1).
     *
     * Precedence is builtins, then the path, then Lisp -- and the order is
     * the interesting part:
     *
     *   Builtins first, so that until `cc` and `e` actually become
     *   executables (C6/C7) the compiled-in versions keep working. When a
     *   builtin is replaced by a binary its `else if` arm has to go in the
     *   same change, or the builtin silently shadows the new file and the
     *   extraction looks like it did nothing.
     *
     *   The path before Lisp, because at a shell prompt a bare word means
     *   "run this". The cost is that a program shadows a Lisp variable of the
     *   same name, which is the usual shell trade and only bites if someone
     *   ships a binary named after their variable.
     *
     *   Anything containing a '/' is skipped entirely: a path typed out in
     *   full is an instruction to run exactly that file, never a name to be
     *   searched for. That is what makes a specific build always reachable
     *   even when a higher-priority volume shadows its name.
     */
    if (cmd_line[0] != '(') {
        /* Split off the first word and treat the rest as arguments (C3).
         * argv[0] is the name as typed, which is what a program expects and
         * what the loader deliberately does not invent for itself. */
        char argbuf[256];
        char *argp[USER_ARGV_LIMIT];
        int argn = 0;
        {
            uint32_t n = 0;
            while (cmd_line[n] && n < sizeof(argbuf) - 1) { argbuf[n] = cmd_line[n]; n++; }
            argbuf[n] = '\0';
        }
        char *q = argbuf;
        while (*q && argn < USER_ARGV_LIMIT) {
            while (*q == ' ' || *q == '\t') *q++ = '\0';
            if (!*q) break;
            argp[argn++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
        }

        bool name_has_slash = false;
        for (const char *c = argn ? argp[0] : ""; *c; c++) {
            if (*c == '/') { name_has_slash = true; break; }
        }

        if (argn > 0 && !name_has_slash) {
            char prog[128];
            if (path_resolve("bin", argp[0], ".elf", prog, sizeof(prog)) == 0) {
                elf_load_and_run_argv(prog, argn, (const char *const *)argp);
                return;
            }
        }
    }

    if (!has_space && strcmp(cmd_line, "ls") != 0 &&
        strcmp(cmd_line, "ps") != 0 &&
        strcmp(cmd_line, "meminfo") != 0 &&
        strcmp(cmd_line, "version") != 0 &&
        strcmp(cmd_line, "df") != 0 &&
        strcmp(cmd_line, "top") != 0 &&
        strcmp(cmd_line, "date") != 0 &&
        strcmp(cmd_line, "time") != 0 &&
        strcmp(cmd_line, "i2c") != 0) {
        lisp_val_t *res = lisp_eval_string(cmd_line);
        /* A bare name that resolves to something callable (a primitive
         * or a user-defined lambda) rather than a plain value -- found
         * live, not theoretical: a zero-argument command with no
         * dedicated shell alias (e.g. `chess-console`) evaluated as a
         * bare symbol lookup and printed the closure object itself
         * (`=> <#primitive>`) instead of ever running, silently doing
         * nothing. Standard Lisp semantics (a bare symbol is just a
         * reference, not a call) are exactly right for a plain value --
         * typing a variable's name to see what it holds is a real,
         * intentional REPL feature this must not break -- but for
         * something callable the user is at a *shell* prompt and almost
         * certainly meant to invoke it, the same as every other bare
         * command does via the POSIX->S-expr wrap below. The lookup
         * above has no side effects (it never called anything, only
         * looked up the binding), so re-evaluating as a proper zero-
         * argument call is safe -- nothing runs twice. */
        if (res && (res->type == LISP_PRIMITIVE || res->type == LISP_LAMBDA)) {
            char call[600];
            int n = 0;
            call[n++] = '(';
            for (const char *p = cmd_line; *p && n < (int)sizeof(call) - 2; p++) {
                call[n++] = *p;
            }
            call[n++] = ')';
            call[n] = '\0';
            res = lisp_eval_string(call);
        }
        if (res && res->type != LISP_NIL) {
            cprintf("=> ");
            lisp_print(res);
            cprintf("\n");
            return;
        }
    }



    /* POSIX/Plan9 command -> S-Expression transformer.
     *
     * Every write goes through sb_putc() so a long or adversarial command
     * line degrades to a clean "too long" rejection instead of walking off
     * the end of sexpr[] -- the delimiter/quote writes below used to be
     * unguarded while only content-character writes were bounds-checked,
     * which let e.g. `ls` followed by ~160 short tokens smash the stack
     * (see B2 in plan/completed/2026-08-07_review_and_remediation.md). */
    char sexpr[512];
    sexpr_buf_t sb;
    sb_init(&sb, sexpr, sizeof(sexpr));
    sb_putc(&sb, '(');

    const char *p = cmd_line;
    /* Verb */
    while (*p && *p != ' ' && *p != '\t') {
        sb_putc(&sb, *p);
        p++;
    }

    /* Special case: write <path> <payload...> */
    if (strncmp(cmd_line, "write ", 6) == 0) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p) {
            sb_putc(&sb, ' ');
            sb_putc(&sb, '"');
            while (*p && *p != ' ' && *p != '\t') {
                sb_putc(&sb, *p);
                p++;
            }
            sb_putc(&sb, '"');
            while (*p == ' ' || *p == '\t') p++;
            if (*p) {
                sb_putc(&sb, ' ');
                sb_putc(&sb, '"');
                while (*p) {
                    if (*p == '"') {
                        sb_putc(&sb, '\\');
                        sb_putc(&sb, '"');
                    } else {
                        sb_putc(&sb, *p);
                    }
                    p++;
                }
                sb_putc(&sb, '"');
            }
        }
    } else {
        /* General argument tokenizer */
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;

            sb_putc(&sb, ' ');
            bool quoted = (*p == '"');
            if (!quoted) sb_putc(&sb, '"');

            while (*p) {
                if (quoted && *p == '"') {
                    sb_putc(&sb, *p++);
                    break;
                } else if (!quoted && (*p == ' ' || *p == '\t')) {
                    break;
                } else {
                    sb_putc(&sb, *p);
                    p++;
                }
            }
            if (!quoted) sb_putc(&sb, '"');
        }
    }

    sb_putc(&sb, ')');
    sexpr[sb.idx] = '\0';

    if (sb.overflowed) {
        cprintf("lsh: command line too long, ignored\n");
        return;
    }

    /* Evaluate translated S-Expression in Lisp engine */
    lisp_val_t *res = lisp_eval_string(sexpr);
    if (res) {
        if (res->type != LISP_NIL) {
            cprintf("=> ");
            lisp_print(res);
            cprintf("\n");
        } else {
            cprintf("\n");
        }
    }
}


void shell_run(void) {
    char buf[512];
    line_editor_init();
    cprintf("\nLugalOS Interactive Console Shell (`lsh`)\n");
    cprintf("Type 'help' for commands, 'cat /proc/ps' for tasks, 'ls /dev/' for devices.\n");

    while (1) {
        int idx = readline_interactive("lsh> ", buf, sizeof(buf));
        if (idx == 0) continue;
        /* S3 (plan/phase13_lisp_engine_extensions.md): the Lisp engine's
         * one safe point to collect garbage at -- see lisp_gc_safepoint()'s
         * declaration in user/lisp/include/lisp.h. This loop qualifies for
         * the same reason lisp_repl()'s own loop does: a fresh top-level
         * command each iteration, driven by raw interactive input, never
         * itself nested inside another expression's still-in-progress
         * evaluation. */
        lisp_gc_safepoint();
        parse_and_eval_cmd(buf);
    }
}


