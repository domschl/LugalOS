/* The USB keyboard host above a transaction port: enumeration, a hub, the
 * boot keyboard behind it, the keyboard as console input, and the reports --
 * 36.8/36.9 (plan/phase36_rp2350_lcd7_terminal.md §3.2), shared since 47.9
 * (plan/phase47_esp32p4_lcd7b_ribbon.md) between the RP2350's PIO-USB engine
 * (drivers/usbkbd_rp2350.c) and the ESP32-P4's OTG HS core
 * (drivers/usbhost_esp32p4.c). The port interface is
 * drivers/include/drivers/usbkbd_task.h.
 *
 * What a session does, in order, and again after every detach:
 *   reset the root port; enumerate what is there at address 1;
 *   if it is a hub: read its descriptor, power its ports, and from then on
 *   poll its status-change endpoint, resetting and enumerating each device
 *   that appears on a port (one hub level; a hub behind the hub is noted,
 *   not followed); for each device with a boot keyboard interface:
 *   SET_PROTOCOL(boot), SET_IDLE(0), and poll its interrupt endpoint at
 *   bInterval, turning each report into make/break events (drivers/usbkbd.c)
 *   in a ring the kernel reads (`kbdlog`, and 36.9's console input).
 *
 * A NAK is "no news", never an error.
 *
 * The task's half is KBD_UTEXT: on the RP2350 it runs in U-mode from
 * .kbdtext, so it calls nothing outside that section but the port's hooks
 * (themselves in .kbdtext), and reads no .rodata (no initialised local
 * arrays, no jump tables: CMakeLists.txt builds this file with
 * -fno-jump-tables). On the P4 it is ordinary kernel code. */

#include "drivers/usbkbd.h"
#include "drivers/usbkbd_task.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(USBKBD_HOST_PRESENT)

#define HOST_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* --- The task's half ------------------------------------------------------ */

/* Setup packets are built byte by byte: an initialised local array would
 * come from .rodata, outside this domain. */
KBD_UTEXT static void setup(uint8_t *s, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len) {
    s[0] = type; s[1] = req;
    s[2] = (uint8_t)val; s[3] = (uint8_t)(val >> 8);
    s[4] = (uint8_t)idx; s[5] = (uint8_t)(idx >> 8);
    s[6] = (uint8_t)len; s[7] = (uint8_t)(len >> 8);
}

/* A control transfer: SETUP, the data stage if there is one (IN only --
 * nothing here writes a data stage), and the status stage in the opposite
 * direction. NAKs are retried for up to half a second per stage. */
KBD_UTEXT static uint32_t control(usbkbd_state_t *k, uint32_t addr, uint32_t mps, const uint8_t *su,
                                  uint8_t *buf, uint32_t want, uint32_t *got) {
    uint32_t st = PIOUSB_TIMEOUT, n = 0;
    if (got) *got = 0;
    for (int tries = 0; tries < 3 && st != PIOUSB_OK; tries++)
        st = usbkbd_port_xfer(k, PIOUSB_OP_SETUP, addr, 0, 0, su, 8, 0, 0, 0);
    if (st != PIOUSB_OK) return st;
    uint32_t have = 0;
    bool toggle = true, in_dir = (su[0] & 0x80u) != 0;
    if (mps == 0) mps = 8;
    uint32_t t0 = usbkbd_port_ms();
    while (in_dir && have < want) {
        if (usbkbd_port_ms() - t0 > 500u) return PIOUSB_TIMEOUT;
        st = usbkbd_port_xfer(k, PIOUSB_OP_IN, addr, 0, toggle, 0, 0, buf + have, want - have, &n);
        if (st == PIOUSB_NAK || st == PIOUSB_TIMEOUT || st == PIOUSB_CRC || st == PIOUSB_TOGGLE) continue;
        if (st != PIOUSB_OK) return st;
        have += n;
        toggle = !toggle;
        if (n < mps) break;                     /* a short packet ends the stage */
    }
    if (got) *got = have;
    t0 = usbkbd_port_ms();
    for (;;) {
        if (usbkbd_port_ms() - t0 > 500u) return PIOUSB_TIMEOUT;
        if (in_dir) st = usbkbd_port_xfer(k, PIOUSB_OP_OUT, addr, 0, 1, 0, 0, 0, 0, 0);
        else        st = usbkbd_port_xfer(k, PIOUSB_OP_IN, addr, 0, 1, 0, 0, buf, 0, &n);
        if (st == PIOUSB_NAK || st == PIOUSB_TIMEOUT) continue;
        return st;
    }
}

KBD_UTEXT static void note_error(usbkbd_state_t *k, uint32_t phase, uint32_t st, uint32_t addr) {
    k->last_error = (phase << 16) | (st << 8) | addr;
    k->failures++;
}

KBD_UTEXT static usbkbd_dev_t *dev_on_port(usbkbd_state_t *k, uint32_t port) {
    for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++)
        if (k->dev[i].addr && k->dev[i].hub_port == port) return &k->dev[i];
    return 0;
}

KBD_UTEXT static usbkbd_dev_t *dev_free(usbkbd_state_t *k) {
    for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++)
        if (!k->dev[i].addr) return &k->dev[i];
    return 0;
}

KBD_UTEXT static void dev_clear(usbkbd_dev_t *d) {
    uint8_t *p = (uint8_t *)d;
    for (uint32_t i = 0; i < sizeof(*d); i++) p[i] = 0;
}

/* Who the next transactions are for: its speed, and the hub port in front of
 * it (0 for the root device and the hub itself). */
KBD_UTEXT static void talk_to(usbkbd_state_t *k, uint32_t ls, uint32_t hs, uint32_t port) {
    k->ls = (uint8_t)ls;
    k->hs = (uint8_t)hs;
    k->tt_port = (uint8_t)port;
}

/* Enumerate whatever answers at address 0 -- the root device, or the one a
 * hub port was just reset for -- and give it `addr`. */
KBD_UTEXT static uint32_t enumerate(usbkbd_state_t *k, usbkbd_dev_t *d, uint32_t addr, uint32_t port) {
    uint8_t su[8];
    uint32_t got = 0, st;
    dev_clear(d);
    k->phase = USBKBD_PH_DESC8;
    setup(su, 0x80, 6, 0x0100, 0, 8);
    st = control(k, 0, 8, su, k->buf, 8, &got);
    if (st != PIOUSB_OK || got < 8) { note_error(k, k->phase, st, 0); return st ? st : PIOUSB_PROTOCOL; }
    uint32_t mps0 = k->buf[7] ? k->buf[7] : 8;
    k->phase = USBKBD_PH_ADDRESS;
    setup(su, 0x00, 5, (uint16_t)addr, 0, 0);
    st = control(k, 0, mps0, su, k->buf, 0, 0);
    if (st != PIOUSB_OK) { note_error(k, k->phase, st, 0); return st; }
    usbkbd_port_sleep(5);                                 /* SET_ADDRESS recovery: 2 ms */
    k->phase = USBKBD_PH_DESC18;
    setup(su, 0x80, 6, 0x0100, 0, 18);
    st = control(k, addr, mps0, su, k->buf, 18, &got);
    if (st != PIOUSB_OK || got < 18) { note_error(k, k->phase, st, addr); return st ? st : PIOUSB_PROTOCOL; }
    for (uint32_t i = 0; i < 18; i++) k->desc18[i] = k->buf[i];
    d->addr = (uint8_t)addr;
    d->hub_port = (uint8_t)port;
    d->full_speed = (uint8_t)!k->ls;
    d->high_speed = k->hs;
    d->mps0 = (uint8_t)mps0;
    d->dev_class = k->buf[4];
    d->vid = (uint16_t)(k->buf[8] | (k->buf[9] << 8));
    d->pid = (uint16_t)(k->buf[10] | (k->buf[11] << 8));
    k->phase = USBKBD_PH_CONFIG;
    setup(su, 0x80, 6, 0x0200, 0, 9);
    st = control(k, addr, mps0, su, k->buf, 9, &got);
    if (st != PIOUSB_OK || got < 9) { note_error(k, k->phase, st, addr); return st ? st : PIOUSB_PROTOCOL; }
    uint32_t total = (uint32_t)(k->buf[2] | (k->buf[3] << 8));
    if (total > sizeof(k->buf)) total = sizeof(k->buf);
    setup(su, 0x80, 6, 0x0200, 0, (uint16_t)total);
    st = control(k, addr, mps0, su, k->buf, total, &got);
    if (st != PIOUSB_OK) { note_error(k, k->phase, st, addr); return st; }
    usb_cfg_info_t ci;
    usb_parse_config(k->buf, got, &ci);
    d->cfg_value = ci.cfg_value;
    d->is_hub = (uint8_t)(ci.is_hub || d->dev_class == 9);
    d->hub_ep = ci.hub_ep;
    k->phase = USBKBD_PH_SETCONFIG;
    setup(su, 0x00, 9, ci.cfg_value, 0, 0);
    st = control(k, addr, mps0, su, k->buf, 0, 0);
    if (st != PIOUSB_OK) { note_error(k, k->phase, st, addr); return st; }
    if (ci.kbd_found) {
        d->is_kbd = 1;
        d->kbd_iface = ci.kbd_iface;
        d->kbd_ep = ci.kbd_ep;
        d->kbd_mps = ci.kbd_mps;
        d->kbd_interval = ci.kbd_interval ? ci.kbd_interval : 10;
        d->toggle = 0;
        /* Both may STALL on a keyboard that only does boot protocol, or
         * that does not implement idle; neither is fatal. */
        k->phase = USBKBD_PH_PROTOCOL;
        setup(su, 0x21, 0x0B, 0, ci.kbd_iface, 0);  /* SET_PROTOCOL(boot) */
        (void)control(k, addr, mps0, su, k->buf, 0, 0);
        setup(su, 0x21, 0x0A, 0, ci.kbd_iface, 0);  /* SET_IDLE(0): report on change */
        (void)control(k, addr, mps0, su, k->buf, 0, 0);
        for (int i = 0; i < 8; i++) k->report[i] = 0;
    }
    k->enumerations++;
    return PIOUSB_OK;
}

KBD_UTEXT static uint32_t hub_req(usbkbd_state_t *k, uint32_t type, uint32_t req, uint32_t val, uint32_t port) {
    uint8_t su[8];
    talk_to(k, 0, 0, 0);                        /* the hub itself */
    setup(su, (uint8_t)type, (uint8_t)req, (uint16_t)val, (uint16_t)port, 0);
    return control(k, k->hub_addr, 64, su, k->buf, 0, 0);
}

KBD_UTEXT static uint32_t port_status(usbkbd_state_t *k, uint32_t port, uint32_t *status, uint32_t *change) {
    uint8_t su[8], b[4];
    uint32_t got = 0;
    talk_to(k, 0, 0, 0);
    setup(su, 0xA3, 0, 0, (uint16_t)port, 4);   /* GET_STATUS(port) */
    uint32_t st = control(k, k->hub_addr, 64, su, b, 4, &got);
    if (st != PIOUSB_OK || got < 4) return st ? st : PIOUSB_PROTOCOL;
    *status = (uint32_t)(b[0] | (b[1] << 8));
    *change = (uint32_t)(b[2] | (b[3] << 8));
    return PIOUSB_OK;
}

/* Something changed on a hub port (or this is the first look at it). */
KBD_UTEXT static void hub_port(usbkbd_state_t *k, uint32_t port) {
    uint32_t status = 0, change = 0;
    if (port_status(k, port, &status, &change) != PIOUSB_OK) return;
    /* Acknowledge every change bit: C_PORT_CONNECTION 16, _ENABLE 17,
     * _SUSPEND 18, _OVER_CURRENT 19, _RESET 20. */
    for (uint32_t b = 0; b < 5; b++)
        if (change & (1u << b)) (void)hub_req(k, 0x23, 1, 16u + b, port);
    k->port_status[port - 1] = (uint16_t)status;
    usbkbd_dev_t *d = dev_on_port(k, port);
    bool connected = (status & 1u) != 0;
    if (!connected) {
        if (d) { dev_clear(d); k->detaches++; }
        return;
    }
    /* Enumerated and still enabled: nothing to do. A port the hub disabled
     * under a device (it went away and came back, or babbled) is started
     * over from a port reset -- this keyboard drops off on its own when it
     * sleeps, and comes back through here. */
    if (d && (status & 2u)) return;
    if (d) { dev_clear(d); k->detaches++; }
    k->phase = USBKBD_PH_PORTRESET;
    (void)hub_req(k, 0x23, 3, 4, port);         /* SET_FEATURE(PORT_RESET) */
    uint32_t t0 = usbkbd_port_ms();
    do {
        usbkbd_port_sleep(10);
        if (port_status(k, port, &status, &change) != PIOUSB_OK) return;
    } while (!(change & (1u << 4)) && usbkbd_port_ms() - t0 < 500u);
    (void)hub_req(k, 0x23, 1, 20, port);        /* CLEAR_FEATURE(C_PORT_RESET) */
    k->port_status[port - 1] = (uint16_t)status;
    if (!(status & 2u)) { note_error(k, USBKBD_PH_PORTRESET, PIOUSB_NODEV, port); return; }
    usbkbd_port_sleep(20);                                /* reset recovery: 10 ms */
    usbkbd_dev_t *nd = dev_free(k);
    if (!nd) return;
    /* Low speed (bit 9): every host packet to it goes out behind a PRE, which
     * the engine does when the slot says low_speed. */
    talk_to(k, (status >> 9) & 1u, (status >> 10) & 1u, port);
    if (k->ls) k->lowspeed_seen++;
    uint32_t addr = k->next_addr;
    k->next_addr = (uint8_t)(addr >= 126u ? 2u : addr + 1u);
    /* A failed enumeration leaves nothing behind: the slot is freed, and the
     * main loop's once-a-second retry takes the port from the top. */
    if (enumerate(k, nd, addr, port) != PIOUSB_OK) dev_clear(nd);
    talk_to(k, 0, 0, 0);
}

KBD_UTEXT static void hub_setup(usbkbd_state_t *k, usbkbd_dev_t *hub) {
    uint8_t su[8];
    uint32_t got = 0;
    k->hub_addr = hub->addr;
    k->phase = USBKBD_PH_HUBDESC;
    setup(su, 0xA0, 6, 0x2900, 0, 16);          /* GET_DESCRIPTOR(hub) */
    uint32_t st = control(k, hub->addr, hub->mps0, su, k->buf, 16, &got);
    if (st != PIOUSB_OK || got < 7) { note_error(k, k->phase, st, hub->addr); return; }
    uint32_t n = k->buf[2];
    k->hub_ports = (uint8_t)(n > USBKBD_MAX_PORTS ? USBKBD_MAX_PORTS : n);
    uint32_t pwr_good_ms = (uint32_t)k->buf[5] * 2u;
    k->phase = USBKBD_PH_HUBPOWER;
    for (uint32_t p = 1; p <= k->hub_ports; p++)
        (void)hub_req(k, 0x23, 3, 8, p);        /* SET_FEATURE(PORT_POWER) */
    usbkbd_port_sleep(pwr_good_ms + 100u);
    for (uint32_t p = 1; p <= k->hub_ports; p++) hub_port(k, p);
}

/* The hub's status-change endpoint: one bit per port that changed (bit 0 is
 * the hub itself). NAK is "nothing changed". */
KBD_UTEXT static void hub_poll(usbkbd_state_t *k, usbkbd_dev_t *hub) {
    uint8_t b[2] = { 0, 0 };
    uint32_t n = 0;
    talk_to(k, 0, 0, 0);
    uint32_t st = usbkbd_port_xfer(k, PIOUSB_OP_IN, hub->addr, hub->hub_ep, hub->toggle, 0, 0, b, 2, &n);
    if (st == PIOUSB_TOGGLE) return;
    if (st != PIOUSB_OK) return;
    hub->toggle ^= 1u;
    uint32_t bits = (uint32_t)(b[0] | (n > 1 ? (b[1] << 8) : 0));
    for (uint32_t p = 1; p <= k->hub_ports; p++)
        if (bits & (1u << p)) hub_port(k, p);
}

KBD_UTEXT static void kbd_poll(usbkbd_state_t *k, usbkbd_dev_t *d) {
    uint8_t r[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t n = 0;
    k->phase = USBKBD_PH_RUNNING;
    talk_to(k, !d->full_speed, d->high_speed, d->hub_port);
    uint32_t st = usbkbd_port_xfer(k, PIOUSB_OP_IN, d->addr, d->kbd_ep, d->toggle, 0, 0, r, 8, &n);
    talk_to(k, 0, 0, 0);
    k->polls++;
    if (st == PIOUSB_NAK) { k->naks++; return; }
    if (st == PIOUSB_TOGGLE) return;            /* a repeat of what we have */
    if (st != PIOUSB_OK) { note_error(k, USBKBD_PH_RUNNING, st, d->addr); return; }
    d->toggle ^= 1u;
    if (n < 3) return;
    k->reports++;
    uint32_t ev[16];
    int m = usbkbd_diff(k->report, r, ev, 16);
    if (m < 0) { k->rollovers++; return; }
    for (int i = 0; i < m; i++) {
        k->ev[k->ev_head & (USBKBD_EV_RING - 1u)] = ev[i];
        HOST_FENCE();
        k->ev_head = k->ev_head + 1u;
    }
    for (int i = 0; i < 8; i++) k->report[i] = r[i];
}

KBD_UTEXT void usbkbd_host_forget(usbkbd_state_t *k) {
    for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++) dev_clear(&k->dev[i]);
    for (uint32_t p = 0; p < USBKBD_MAX_PORTS; p++) k->port_status[p] = 0;
    k->hub_ports = 0;
    k->hub_addr = 0;
    k->next_addr = 2;
    talk_to(k, 0, 0, 0);
}

KBD_UTEXT bool usbkbd_host_session(usbkbd_state_t *k) {
    k->phase = USBKBD_PH_ROOTRESET;
    talk_to(k, 0, 0, 0);                        /* the port's RESET may say otherwise */
    uint32_t st = usbkbd_port_xfer(k, PIOUSB_OP_RESET, 0, 0, 0, 0, 0, 0, 0, 0);
    k->root_resets++;
    if (st != PIOUSB_OK) { note_error(k, USBKBD_PH_ROOTRESET, st, 0); usbkbd_port_sleep(1000); return false; }
    usbkbd_port_sleep(20);
    usbkbd_dev_t *root = &k->dev[0];
    if (enumerate(k, root, 1, 0) != PIOUSB_OK) { usbkbd_port_sleep(1000); return false; }
    if (root->is_hub) hub_setup(k, root);
    uint32_t last_hub = usbkbd_port_ms(), last_retry = last_hub;
    while (usbkbd_port_up(k)) {
        uint32_t now = usbkbd_port_ms();
        if (root->is_hub && root->hub_ep && now - last_hub >= 100u) {
            hub_poll(k, root);
            last_hub = now;
        }
        /* A port that is connected but has no working device -- its
         * enumeration failed, or it came back without a status change
         * we saw -- is tried again once a second. */
        if (root->is_hub && now - last_retry >= 1000u) {
            last_retry = now;
            for (uint32_t p = 1; p <= k->hub_ports; p++)
                if ((k->port_status[p - 1] & 1u) && !dev_on_port(k, p)) { k->retries++; hub_port(k, p); }
        }
        for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++) {
            usbkbd_dev_t *d = &k->dev[i];
            if (d->addr && d->is_kbd &&
                now - d->last_poll >= d->kbd_interval) {
                d->last_poll = now;
                kbd_poll(k, d);
            }
        }
        usbkbd_port_sleep(1);
    }
    return true;
}

/* --- Kernel side ---------------------------------------------------------- */

static usbkbd_state_t *g_k;

/* --- The keyboard as a console input source (36.9) -----------------------
 *
 * The kernel side of the event ring: events are translated into bytes
 * (drivers/usbkbd.c: US keymap, VT sequences, typematic) into a small queue
 * the console pump reads like any other source. Typematic runs here, on the
 * pump's polls, because the keyboard reports only on change. */
#define KQ_SIZE 64u
static usbkbd_xlate_t g_xl;
static uint32_t       g_ev_tail;
static uint8_t        g_kq[KQ_SIZE];
static uint32_t       g_kq_head, g_kq_tail;

static bool kbd_present(void) {
    for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++)
        if (g_k->dev[i].addr && g_k->dev[i].is_kbd) return true;
    return false;
}

static void kq_put(const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (g_kq_head - g_kq_tail >= KQ_SIZE) return;   /* full: drop */
        g_kq[g_kq_head % KQ_SIZE] = b[i];
        g_kq_head++;
    }
}

static void kbd_fill(void) {
    uint32_t now = (uint32_t)time_get_ms();
    uint32_t head = g_k->ev_head;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (head - g_ev_tail > USBKBD_EV_RING) g_ev_tail = head - USBKBD_EV_RING;  /* lapped */
    uint8_t out[USBKBD_SEQ_MAX];
    while (g_ev_tail != head && KQ_SIZE - (g_kq_head - g_kq_tail) >= USBKBD_SEQ_MAX) {
        uint32_t e = g_k->ev[g_ev_tail % USBKBD_EV_RING];
        g_ev_tail++;
        kq_put(out, usbkbd_translate(&g_xl, e, now, out));
        if (g_xl.hotkey) {                      /* 37.5a: the screen's, not a program's */
            console_hotkey(g_xl.hotkey);        /* USBKBD_HOTKEY_* == CONSOLE_HOTKEY_* */
            g_xl.hotkey = USBKBD_HOTKEY_NONE;
        }
    }
    /* A key held on a keyboard that has gone away must not repeat forever. */
    if (!kbd_present()) g_xl.repeat_usage = 0;
    if (KQ_SIZE - (g_kq_head - g_kq_tail) >= USBKBD_SEQ_MAX)
        kq_put(out, usbkbd_repeat(&g_xl, now, out));
}

static bool kbd_has_char(void) {
    kbd_fill();
    return g_kq_head != g_kq_tail;
}

static int kbd_getc(void) {
    if (g_kq_head == g_kq_tail) return -1;
    return g_kq[g_kq_tail++ % KQ_SIZE];
}

static bool kbd_peek_interrupt(void) {
    kbd_fill();
    for (uint32_t i = g_kq_tail; i != g_kq_head; i++)
        if (g_kq[i % KQ_SIZE] == 0x03) return true;
    return false;
}

/* 37.2: `Compose` / `Compose "` while a compose sequence is open. */
static char g_kbd_hint[12];

static const char *kbd_indicator(void) {
    usbkbd_compose_hint(&g_xl, g_kbd_hint, sizeof(g_kbd_hint));
    return g_kbd_hint;
}

static const console_input_t g_in_kbd = { "kbd", kbd_has_char, kbd_getc, kbd_peek_interrupt, kbd_indicator };

void usbkbd_host_attach(usbkbd_state_t *k) {
    g_k = k;
    g_ev_tail = k->ev_head;
    if (console_input_register(&g_in_kbd) != 0)
        printk("[KBD] the console has no room for a third input source\n");
}

const usbkbd_state_t *usbkbd_state(void) { return g_k; }

#else

const usbkbd_state_t *usbkbd_state(void) { return 0; }

#endif

/* --- Reporting (every target; nothing to report without the port) -------- */

static const char *phase_name(uint32_t p) {
    static const char *const n[] = { "waiting for a device", "resetting the port",
        "GET_DESCRIPTOR(device, 8)", "SET_ADDRESS", "GET_DESCRIPTOR(device)",
        "GET_DESCRIPTOR(configuration)", "SET_CONFIGURATION", "SET_PROTOCOL/SET_IDLE",
        "GET_DESCRIPTOR(hub)", "powering hub ports", "resetting a hub port", "running",
        "off (the port is the kernel's)" };
    return p < sizeof(n) / sizeof(n[0]) ? n[p] : "?";
}

void usbkbd_report(void) {
    const usbkbd_state_t *k = usbkbd_state();
    if (!k) { cprintf("kbd: this board has no USB keyboard port\n"); return; }
    cprintf("kbd: %s; %lu enumerations, %lu failures, %lu root resets, %lu detaches (root %lu)\n",
            phase_name(k->phase), (unsigned long)k->enumerations, (unsigned long)k->failures,
            (unsigned long)k->root_resets, (unsigned long)k->detaches, (unsigned long)k->root_detaches);
    if (k->failures)
        cprintf("kbd: last error in %s: status %lu at address/port %lu\n",
                phase_name(k->last_error >> 16), (unsigned long)((k->last_error >> 8) & 0xff),
                (unsigned long)(k->last_error & 0xff));
    for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++) {
        const usbkbd_dev_t *d = &k->dev[i];
        if (!d->addr) continue;
        if (d->hub_port) cprintf("kbd:   addr %u on hub port %u:", d->addr, d->hub_port);
        else             cprintf("kbd:   addr %u at the root:", d->addr);
        cprintf(" %04x:%04x class %02x, %s speed%s%s", d->vid, d->pid, d->dev_class,
                d->high_speed ? "high" : d->full_speed ? "full" : "low", d->is_hub ? " (hub)" : "", d->is_kbd ? " BOOT KEYBOARD" : "");
        if (d->is_kbd) cprintf(" iface %u ep %u, %u bytes every %u ms", d->kbd_iface, d->kbd_ep, d->kbd_mps, d->kbd_interval);
        cprintf("\n");
    }
    if (k->hub_ports) {
        cprintf("kbd:   hub ports:");
        for (uint32_t p = 0; p < k->hub_ports; p++) cprintf(" %lu:%04x", (unsigned long)(p + 1), k->port_status[p]);
        cprintf("  (bit 0 connected, 1 enabled, 9 low speed)\n");
    }
    cprintf("kbd: %lu polls, %lu reports, %lu NAKs, %lu rollovers, %lu events, %lu transactions, %lu port retries\n",
            (unsigned long)k->polls, (unsigned long)k->reports, (unsigned long)k->naks,
            (unsigned long)k->rollovers, (unsigned long)k->ev_head, (unsigned long)k->xfers,
            (unsigned long)k->retries);
}

/* `kbd desc`: the task's descriptor scratch, which holds the last
 * configuration descriptor it read -- the one to look at when a device is
 * found but not recognised. */
void usbkbd_dump_desc(void) {
    const usbkbd_state_t *k = usbkbd_state();
    if (!k) return;
    uint32_t total = (uint32_t)(k->buf[2] | (k->buf[3] << 8));
    if (k->buf[1] != 2 || total > sizeof(k->buf)) total = 64;
    cprintf("kbd: last descriptor read (%lu bytes):", (unsigned long)total);
    for (uint32_t i = 0; i < total; i++) cprintf("%s%02x", (i % 16) ? " " : "\n  ", k->buf[i]);
    cprintf("\n");
}

static const char *usage_name(uint32_t u, char *tmp) {
    static const char *const mods[8] = { "LCtrl", "LShift", "LAlt", "LGui", "RCtrl", "RShift", "RAlt", "RGui" };
    static const char *const misc[] = { "Enter", "Esc", "Backspace", "Tab", "Space" };
    if (u >= 0xE0 && u <= 0xE7) return mods[u - 0xE0];
    if (u >= 0x04 && u <= 0x1D) { tmp[0] = (char)('a' + u - 0x04); tmp[1] = 0; return tmp; }
    if (u >= 0x1E && u <= 0x26) { tmp[0] = (char)('1' + u - 0x1E); tmp[1] = 0; return tmp; }
    if (u == 0x27) return "0";
    if (u >= 0x28 && u <= 0x2C) return misc[u - 0x28];
    if (u >= 0x3A && u <= 0x45) { ksnprintf(tmp, 8, "F%u", (unsigned)(u - 0x3A + 1)); return tmp; }
    ksnprintf(tmp, 8, "0x%02x", (unsigned)u);
    return tmp;
}

/* `kbdlog [s]`: print key events as they happen, for `s` seconds (default
 * 30) or until a key arrives on the console. */
void usbkbd_log(uint32_t seconds) {
    const usbkbd_state_t *k = usbkbd_state();
    if (!k) { cprintf("kbdlog: this board has no USB keyboard port\n"); return; }
    /* The ring's recent past first (up to 32 events), then live. */
    uint32_t head = k->ev_head;
    uint32_t back = head < 32u ? head : 32u;
    uint32_t tail = head - back;
    uint64_t end = time_get_ms() + (uint64_t)seconds * 1000u;
    cprintf("kbdlog: the last %lu events, then live for %lu s (Ctrl-C stops)\n",
            (unsigned long)back, (unsigned long)seconds);
    while (time_get_ms() < end) {
        /* Only Ctrl-C stops the log: since 36.9 the keyboard being logged is
         * console input too, so "any key" would stop it at the first key.
         * Everything else typed meanwhile is discarded. */
        if (console_has_char() && console_getc() == 0x03) break;
        while (tail != k->ev_head) {
            uint32_t e = k->ev[tail & (USBKBD_EV_RING - 1u)];
            tail++;
            char tmp[8];
            cprintf("kbdlog: %-5s %-9s mods %02lx\n", (e & USBKBD_EV_MAKE) ? "make" : "break",
                    usage_name(USBKBD_EV_USAGE(e), tmp), (unsigned long)USBKBD_EV_MODS(e));
        }
        task_sleep_ms(10);
    }
    cprintf("kbdlog: done\n");
}
