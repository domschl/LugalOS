/* The `kbd` task on the RP2350's PIO-USB engine -- 36.8,
 * plan/phase36_rp2350_lcd7_terminal.md §3.2. Since 47.9 this file is only the
 * *port* (drivers/include/drivers/usbkbd_task.h): one transaction through the
 * engine's ring, the task's clock, and the task itself. Enumeration, the hub,
 * the boot keyboard and the console input above it are drivers/usbkbd_host.c,
 * shared with the ESP32-P4's OTG HS port (drivers/usbhost_esp32p4.c).
 *
 * A U-mode task, PMP-confined to four regions:
 *   its stack; .kbdtext (this file's U-mode half, drivers/usbkbd_host.c's and
 *   drivers/usbkbd.c); its own state block below (1 KB); and the PIO-USB
 *   engine's shared block (drivers/piousb.h, 512 bytes: the transaction ring
 *   and the engine status).
 * No MMIO: the task talks to the bus only by filling ring slots, which core 1
 * executes. A wild pointer here can corrupt the ring, which the engine
 * tolerates (lengths are clamped, unknown ops refused) -- and nothing else.
 *
 * Low-speed devices behind the hub need PRE (plan §3.2), which the engine
 * does when a slot says low_speed. */

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

KBD_UTEXT uint32_t usbkbd_port_ms(void) { return U_TIME_MS(); }
KBD_UTEXT void usbkbd_port_sleep(uint32_t ms) { U_SLEEP(ms); }
KBD_UTEXT bool usbkbd_port_up(usbkbd_state_t *k) {
    return k->usb->st.connected && k->usb->st.enabled && !k->off_request;
}

/* One transaction through the engine's ring. Returns PIOUSB_*; for IN, the
 * payload lands in in[] (up to in_max) and *in_len says how much came. */
KBD_UTEXT uint32_t usbkbd_port_xfer(usbkbd_state_t *k, uint32_t op, uint32_t addr, uint32_t ep, uint32_t data1,
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

KBD_UTEXT static void kbd_umode_body(uintptr_t arg) {
    usbkbd_state_t *k = (usbkbd_state_t *)arg;
    piousb_shared_t *u = k->usb;
    for (;;) {
        usbkbd_host_forget(k);
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
        if (usbkbd_host_session(k)) k->root_detaches++;
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
    usbkbd_host_attach(&g_kbd);
    printk("[KBD] USB keyboard task #%d (U-mode) owns the PIO-USB port; keys are console input\n", pid);
    return pid;
}

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

#endif
