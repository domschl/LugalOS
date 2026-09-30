/* The `kbd` task: USB enumeration, a hub, and the boot keyboard behind it --
 * 36.8, plan/phase36_rp2350_lcd7_terminal.md §3.2.
 *
 * A U-mode task, PMP-confined to four regions:
 *   its stack; .kbdtext (this file's U-mode half and drivers/usbkbd.c);
 *   its own state block below (1 KB); and the PIO-USB engine's shared block
 *   (drivers/piousb.h, 512 bytes: the transaction ring and the engine status).
 * No MMIO: the task talks to the bus only by filling ring slots, which core 1
 * executes. A wild pointer here can corrupt the ring, which the engine
 * tolerates (lengths are clamped, unknown ops refused) -- and nothing else.
 *
 * What it does, in order, and again after every detach:
 *   wait for something on the port; reset it; enumerate it at address 1;
 *   if it is a hub: read its descriptor, power its ports, and from then on
 *   poll its status-change endpoint, resetting and enumerating each device
 *   that appears on a port (one hub level; a hub behind the hub is noted,
 *   not followed); for each device with a boot keyboard interface:
 *   SET_PROTOCOL(boot), SET_IDLE(0), and poll its interrupt endpoint at
 *   bInterval, turning each report into make/break events (drivers/usbkbd.c)
 *   in a ring the kernel reads (`kbdlog`, and 36.9's console input).
 *
 * A NAK is "no news", never an error. Low-speed devices behind the hub need
 * PRE (plan §3.2) which the engine does not have yet: they are recorded, not
 * enumerated. */

#include "drivers/usbkbd.h"
#include "drivers/piousb.h"
#include "drivers/driver_task.h"
#include "kernel/console.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/mem_domain.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "arch/umode.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#include "drivers/usbkbd_task.h"

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)

static union {
    usbkbd_state_t s;
    uint8_t        raw[1024];
} g_kbd_mem __attribute__((aligned(1024)));
_Static_assert(sizeof(usbkbd_state_t) <= 1024, "usbkbd: the state outgrew its 1 KB grant");
#define g_kbd g_kbd_mem.s

/* --- U-mode -------------------------------------------------------------- */

__attribute__((always_inline)) static inline long usys(long n, long a1) {
    register long r_a0 __asm__("a0") = n;
    register long r_a1 __asm__("a1") = a1;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
    return r_a0;
}
#define U_TIME_MS()   ((uint32_t)usys(SYS_TIME_MS, 0))
#define U_YIELD()     ((void)usys(SYS_YIELD, 0))
#define U_SLEEP(ms)   ((void)usys(SYS_SLEEP_MS, (long)(ms)))
#define U_FENCE()     __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Setup packets are built byte by byte: an initialised local array would
 * come from .rodata, outside this domain. */
KBD_UTEXT static void setup(uint8_t *s, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len) {
    s[0] = type; s[1] = req;
    s[2] = (uint8_t)val; s[3] = (uint8_t)(val >> 8);
    s[4] = (uint8_t)idx; s[5] = (uint8_t)(idx >> 8);
    s[6] = (uint8_t)len; s[7] = (uint8_t)(len >> 8);
}

/* One transaction through the engine's ring. Returns PIOUSB_*; for IN, the
 * payload lands in in[] (up to in_max) and *in_len says how much came. */
KBD_UTEXT static uint32_t xfer(usbkbd_state_t *k, uint32_t op, uint32_t addr, uint32_t ep, uint32_t data1,
                               const uint8_t *data, uint32_t len, uint8_t *in, uint32_t in_max, uint32_t *in_len) {
    piousb_shared_t *u = k->usb;
    uint32_t start = U_TIME_MS();
    uint32_t t = u->submitted;
    while (t - u->completed >= PIOUSB_RING) {
        if (U_TIME_MS() - start > 500u) return PIOUSB_ENGINE;
        U_YIELD();
    }
    piousb_xfer_t *s = &u->slot[t & (PIOUSB_RING - 1u)];
    s->op = (uint8_t)op; s->addr = (uint8_t)addr; s->ep = (uint8_t)ep; s->data1 = (uint8_t)data1;
    s->len = (uint16_t)len; s->status = 0xff; s->rx_len = 0;
    s->low_speed = k->ls;
    for (uint32_t i = 0; i < len && i < PIOUSB_MAX_PACKET; i++) s->data[i] = data[i];
    U_FENCE();
    u->submitted = t + 1u;
    while ((int32_t)(u->completed - t) <= 0) {
        if (U_TIME_MS() - start > 500u) return PIOUSB_ENGINE;
        U_YIELD();
    }
    U_FENCE();
    if (in) {
        uint32_t n = s->rx_len < in_max ? s->rx_len : in_max;
        for (uint32_t i = 0; i < n; i++) in[i] = s->data[i];
        if (in_len) *in_len = n;
    }
    k->xfers++;
    return s->status;
}

/* A control transfer: SETUP, the data stage if there is one (IN only --
 * nothing here writes a data stage), and the status stage in the opposite
 * direction. NAKs are retried for up to half a second per stage. */
KBD_UTEXT static uint32_t control(usbkbd_state_t *k, uint32_t addr, uint32_t mps, const uint8_t *su,
                                  uint8_t *buf, uint32_t want, uint32_t *got) {
    uint32_t st = PIOUSB_TIMEOUT, n = 0;
    if (got) *got = 0;
    for (int tries = 0; tries < 3 && st != PIOUSB_OK; tries++)
        st = xfer(k, PIOUSB_OP_SETUP, addr, 0, 0, su, 8, 0, 0, 0);
    if (st != PIOUSB_OK) return st;
    uint32_t have = 0;
    bool toggle = true, in_dir = (su[0] & 0x80u) != 0;
    if (mps == 0) mps = 8;
    uint32_t t0 = U_TIME_MS();
    while (in_dir && have < want) {
        if (U_TIME_MS() - t0 > 500u) return PIOUSB_TIMEOUT;
        st = xfer(k, PIOUSB_OP_IN, addr, 0, toggle, 0, 0, buf + have, want - have, &n);
        if (st == PIOUSB_NAK || st == PIOUSB_TIMEOUT || st == PIOUSB_CRC || st == PIOUSB_TOGGLE) continue;
        if (st != PIOUSB_OK) return st;
        have += n;
        toggle = !toggle;
        if (n < mps) break;                     /* a short packet ends the stage */
    }
    if (got) *got = have;
    t0 = U_TIME_MS();
    for (;;) {
        if (U_TIME_MS() - t0 > 500u) return PIOUSB_TIMEOUT;
        if (in_dir) st = xfer(k, PIOUSB_OP_OUT, addr, 0, 1, 0, 0, 0, 0, 0);
        else        st = xfer(k, PIOUSB_OP_IN, addr, 0, 1, 0, 0, buf, 0, &n);
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
    U_SLEEP(5);                                 /* SET_ADDRESS recovery: 2 ms */
    k->phase = USBKBD_PH_DESC18;
    setup(su, 0x80, 6, 0x0100, 0, 18);
    st = control(k, addr, mps0, su, k->buf, 18, &got);
    if (st != PIOUSB_OK || got < 18) { note_error(k, k->phase, st, addr); return st ? st : PIOUSB_PROTOCOL; }
    d->addr = (uint8_t)addr;
    d->hub_port = (uint8_t)port;
    d->full_speed = (uint8_t)!k->ls;
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
    k->ls = 0;                                  /* the hub itself is full speed */
    setup(su, (uint8_t)type, (uint8_t)req, (uint16_t)val, (uint16_t)port, 0);
    return control(k, k->hub_addr, 64, su, k->buf, 0, 0);
}

KBD_UTEXT static uint32_t port_status(usbkbd_state_t *k, uint32_t port, uint32_t *status, uint32_t *change) {
    uint8_t su[8], b[4];
    uint32_t got = 0;
    k->ls = 0;
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
    uint32_t t0 = U_TIME_MS();
    do {
        U_SLEEP(10);
        if (port_status(k, port, &status, &change) != PIOUSB_OK) return;
    } while (!(change & (1u << 4)) && U_TIME_MS() - t0 < 500u);
    (void)hub_req(k, 0x23, 1, 20, port);        /* CLEAR_FEATURE(C_PORT_RESET) */
    k->port_status[port - 1] = (uint16_t)status;
    if (!(status & 2u)) { note_error(k, USBKBD_PH_PORTRESET, PIOUSB_NODEV, port); return; }
    U_SLEEP(20);                                /* reset recovery: 10 ms */
    usbkbd_dev_t *nd = dev_free(k);
    if (!nd) return;
    /* Low speed (bit 9): every host packet to it goes out behind a PRE, which
     * the engine does when the slot says low_speed. */
    k->ls = (uint8_t)((status >> 9) & 1u);
    if (k->ls) k->lowspeed_seen++;
    uint32_t addr = k->next_addr;
    k->next_addr = (uint8_t)(addr >= 126u ? 2u : addr + 1u);
    /* A failed enumeration leaves nothing behind: the slot is freed, and the
     * main loop's once-a-second retry takes the port from the top. */
    if (enumerate(k, nd, addr, port) != PIOUSB_OK) dev_clear(nd);
    k->ls = 0;
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
    U_SLEEP(pwr_good_ms + 100u);
    for (uint32_t p = 1; p <= k->hub_ports; p++) hub_port(k, p);
}

/* The hub's status-change endpoint: one bit per port that changed (bit 0 is
 * the hub itself). NAK is "nothing changed". */
KBD_UTEXT static void hub_poll(usbkbd_state_t *k, usbkbd_dev_t *hub) {
    uint8_t b[2] = { 0, 0 };
    uint32_t n = 0;
    k->ls = 0;
    uint32_t st = xfer(k, PIOUSB_OP_IN, hub->addr, hub->hub_ep, hub->toggle, 0, 0, b, 2, &n);
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
    k->ls = (uint8_t)!d->full_speed;
    uint32_t st = xfer(k, PIOUSB_OP_IN, d->addr, d->kbd_ep, d->toggle, 0, 0, r, 8, &n);
    k->ls = 0;
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
        U_FENCE();
        k->ev_head = k->ev_head + 1u;
    }
    for (int i = 0; i < 8; i++) k->report[i] = r[i];
}

KBD_UTEXT static void forget_all(usbkbd_state_t *k) {
    for (uint32_t i = 0; i < USBKBD_MAX_DEV; i++) dev_clear(&k->dev[i]);
    for (uint32_t p = 0; p < USBKBD_MAX_PORTS; p++) k->port_status[p] = 0;
    k->hub_ports = 0;
    k->hub_addr = 0;
    k->next_addr = 2;
}

KBD_UTEXT static void kbd_umode_body(uintptr_t arg) {
    usbkbd_state_t *k = (usbkbd_state_t *)arg;
    piousb_shared_t *u = k->usb;
    for (;;) {
        forget_all(k);
        if (k->off_request) {
            k->phase = USBKBD_PH_OFF;
            U_FENCE();
            u->owner = PIOUSB_OWNER_KERNEL;
            while (k->off_request) U_SLEEP(50);
            u->owner = PIOUSB_OWNER_KBD;
            U_FENCE();
        }
        k->phase = USBKBD_PH_WAIT;
        while (!u->st.connected && !k->off_request) U_SLEEP(100);
        if (k->off_request) continue;
        U_SLEEP(100);                           /* attach debounce */
        k->phase = USBKBD_PH_ROOTRESET;
        uint32_t st = xfer(k, PIOUSB_OP_RESET, 0, 0, 0, 0, 0, 0, 0, 0);
        k->root_resets++;
        if (st != PIOUSB_OK) { note_error(k, USBKBD_PH_ROOTRESET, st, 0); U_SLEEP(1000); continue; }
        U_SLEEP(20);
        k->ls = 0;
        usbkbd_dev_t *root = &k->dev[0];
        if (enumerate(k, root, 1, 0) != PIOUSB_OK) { U_SLEEP(1000); continue; }
        if (root->is_hub) hub_setup(k, root);
        uint32_t last_hub = U_TIME_MS(), last_retry = last_hub;
        while (u->st.connected && u->st.enabled && !k->off_request) {
            uint32_t now = U_TIME_MS();
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
            U_SLEEP(1);
        }
        k->root_detaches++;
    }
}

/* --- Kernel side --------------------------------------------------------- */

static uint8_t g_kbd_ustack[2048] __attribute__((aligned(2048)))
                                  __attribute__((section(".ustacks2048")));
static int g_kbd_pid = -1;

static void kbd_task_body(void *arg) {
    (void)arg;
    uintptr_t ub, us;
    piousb_shared_region(&ub, &us);
    const driver_umode_spec_t spec = {
        .name         = "kbd",
        .fallback     = "no USB keyboard.",
        .body         = (void (*)(void))kbd_umode_body,
        .text_region  = board_kbd_text_region,
        .stack_base   = (uintptr_t)g_kbd_ustack,
        .stack_size   = sizeof(g_kbd_ustack),
        .regions      = { { (uintptr_t)&g_kbd_mem, sizeof(g_kbd_mem), MEM_R | MEM_W },
                          { ub, us, MEM_R | MEM_W } },
        .region_count = 2,
        .arg          = (uintptr_t)&g_kbd,
    };
    (void)driver_umode_enter(&spec);
}

int usbkbd_start(void) {
    piousb_shared_t *u = piousb_shared();
    if (!u || !u->st.running) {
        printk("[KBD] no USB engine; no keyboard task\n");
        return -1;
    }
    g_kbd.usb = u;
    u->owner = PIOUSB_OWNER_KBD;
    int pid = task_create_driver("kbd", kbd_task_body, NULL, 1);
    if (pid < 0) {
        u->owner = PIOUSB_OWNER_KERNEL;
        printk("[KBD] could not start the kbd task\n");
        return -1;
    }
    g_kbd_pid = pid;
    printk("[KBD] USB keyboard task #%d (U-mode) owns the PIO-USB port\n", pid);
    return pid;
}

const usbkbd_state_t *usbkbd_state(void) { return &g_kbd; }

bool usbkbd_set_off(bool off) {
    if (g_kbd_pid < 0) return false;
    g_kbd.off_request = off ? 1u : 0u;
    uint64_t end = time_get_ms() + 3000;
    while (time_get_ms() < end) {
        bool is_off = g_kbd.phase == USBKBD_PH_OFF && g_kbd.usb->owner == PIOUSB_OWNER_KERNEL;
        if (off == is_off) return true;
        task_sleep_ms(10);
    }
    return false;
}

#else

int usbkbd_start(void) { return -1; }
bool usbkbd_set_off(bool off) { (void)off; return false; }
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
                d->full_speed ? "full" : "low", d->is_hub ? " (hub)" : "", d->is_kbd ? " BOOT KEYBOARD" : "");
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
    cprintf("kbdlog: the last %lu events, then live for %lu s (any console key stops)\n",
            (unsigned long)back, (unsigned long)seconds);
    while (time_get_ms() < end) {
        /* The CR/LF that ended the command line is still arriving: only a
         * real key stops the log. */
        if (console_has_char()) {
            char c = console_getc();
            if (c != '\r' && c != '\n') break;
        }
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
