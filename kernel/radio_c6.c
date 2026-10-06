/* The kernel's half of the ESP32-C6 Wi-Fi radio (45.6, plan/phase45_esp32c6.md):
 * build the radio's memory domain, start its main thread in U-mode, and report
 * what happened.
 *
 * The blob, the OS-table shim and the chip-side code (drivers/radio/) all run
 * *there*, in one domain, behind a PMP that grants exactly what they touch:
 *
 *   text + rodata     the flash window's first 512 KB (linker/esp32c6.ld, .radio_text)
 *   state             the blob's .data/.bss (.radio_data) and the shim's (.udata_kobj)
 *   shim text         16 KB in SRAM (.utext_kobj)
 *   heap and stack    palloc'd, each one NAPOT block
 *   the chip's ROM    code (512 KB) and its working data (0x4087c000, 16 KB), which the
 *                     blob's ROM-resident halves read and write (45.3a)
 *   the modem         0x600A0000 (64 KB: MAC, baseband, PHY), PMU/LP_AON, PCR
 *
 * That is twelve regions of the C6's sixteen PMP entries (MEM_DOMAIN_MAX_REGIONS).
 * PMP restricts the CPU and not the MAC's DMA (plan §4.5): this contains bugs,
 * and the `tainted` label (not here yet: 45.9) is what covers trust.
 *
 * Without the blob linked in (no ESP-IDF tree at configure time) none of this
 * exists and `radio` says so. */

#include "kernel/radio_c6.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/radio_intr.h"
#include "kernel/kobj_sched.h"
#include "net/netif.h"
#include "net/ip.h"
#include "kernel/palloc.h"
#include "kernel/mem_domain.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/identity.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include "../drivers/radio/radio_main.h"
#include <stddef.h>
#include <string.h>
#include <stdint.h>

/* The node's MAC is the chip's own, from eFuse (EFUSE_RD_MAC_SYS_0/1: the base MAC Espressif burned,
 * which is also what IDF and every router's device list know this board by) -- so the address a
 * router reserves for it is the one printed on the chip's label and stable across builds. Overrides the
 * weak hook in kernel/identity.c. Bytes: mac1[1], mac1[0], mac0[3..0]. */
bool board_factory_mac(uint8_t out[6]) {
    uint32_t m0 = *(volatile uint32_t *)0x600B0844u, m1 = *(volatile uint32_t *)0x600B0848u;
    if (m0 == 0 && (m1 & 0xffffu) == 0) return false;
    out[0] = (uint8_t)(m1 >> 8); out[1] = (uint8_t)m1;
    out[2] = (uint8_t)(m0 >> 24); out[3] = (uint8_t)(m0 >> 16); out[4] = (uint8_t)(m0 >> 8); out[5] = (uint8_t)m0;
    return (out[0] & 1) == 0;
}

/* Keeps .radio_text alive in a build with no blob, so the flash layout does not
 * depend on whether there is one (linker/esp32c6.ld). */
const uint32_t g_radio_text_marker __attribute__((section(".radio_text_marker"), used)) = 0x31444152u;

#if defined(CONFIG_RADIO_C6)

extern char _radio_text_start[], _radio_data_start[], _radio_bss_start[], _radio_bss_end[], _utext_kobj_start[], _udata_kobj_start[];

#define ARENA_PAGES 16          /* 64 KB (one NAPOT region): the frame rings (10 KB) and the radio heap */
#define STACK_PAGES 4           /* 16 KB: the main thread's stack, with the context at its bottom */

static mem_domain_t g_rdomain;
static void *g_rstack;
static void *g_rarena;
static int g_rpid = -1;

/* The C6 has a second fence in front of its peripherals besides the PMP: the HP APM
 * (access permission management, TRM ch. 16), whose reset state (region 0 = the whole
 * address space, attribute 0) denies every REE mode -- which is U-mode -- *by returning
 * zeros to reads and dropping writes*, silently. The radio's first reads of the modem
 * block showed it: M-mode saw 0x7e600000 where U-mode saw 0. The PMP is the finer fence
 * and the one that is ours (it grants the radio exactly three peripheral windows), so
 * the APM is opened for all three REE modes (R/W/X each); the PMP still decides. */
#define HP_APM_REGION0_PMS_ATTR (*(volatile uint32_t *)0x6009900cu)
/* The LP APM fences the LP peripherals (PMU, LP clock/reset, LP_AON) the same way, and its reset state is
 * just as closed. The PHY library switches the RF blocks' power (PMU_RF_PWC: the TX/RX, clock-generator and
 * PLL I2C buses) itself while it calibrates; from U-mode, behind a closed LP APM, those writes vanished and
 * a cold chip's PLL calibration timed out ("pll_cal exceeds 2ms", no access point found). Opened like the
 * HP APM; the PMP grants the radio only the PMU/LP_AON window of it. */
#define LP_APM_REGION0_PMS_ATTR (*(volatile uint32_t *)0x600b380cu)

static void radio_task(void *arg) {
    (void)arg;
    radio_ctx_t *ctx = (radio_ctx_t *)g_rstack;
    ctx->arena = g_rarena;
    kos_init();
    ctx->tx_sem = (uint32_t)kos_sem_create(RN_TX_SLOTS, 0, (uintptr_t)&g_rdomain);   /* given once per queued frame */
    ctx->arena_bytes = ARENA_PAGES * 4096u;
    ctx->stage = RADIO_STAGE_NONE;

    HP_APM_REGION0_PMS_ATTR = 0x777u;
    LP_APM_REGION0_PMS_ATTR = 0x777u;
    /* The chip's boot-time clock setup (drivers/radio/plat_esp32c6.c) runs here, in M-mode, not in the
     * radio's domain: part of it writes the LP_CLKRST block, which is behind the LP APM -- a second
     * permission controller, closed to U-mode by reading zeros and dropping writes, exactly as the HP
     * APM was. Run from U-mode the modem's time counter never started and the PHY waited on it forever
     * (a cold boot; a RAM load hid it with state the previous firmware left). */
    {
        extern void pmu_init(void);              /* IDF's: the power management unit's active/modem/sleep parameters */
        extern void radio_plat_early_init(void);
        extern void radio_plat_cpu_to_pll(void);
        radio_plat_early_init();                 /* modem clock gating and the analog I2C master, which the PLL setup needs */
        radio_plat_cpu_to_pll();                 /* a flash boot leaves the CPU on the 40 MHz crystal, PLL off */
        pmu_init();
        radio_plat_early_init();
    }
    radio_intr_reset();
    for (volatile char *p = _radio_bss_start; p < _radio_bss_end; p++) *p = 0;   /* not in the image */
    mem_domain_init(&g_rdomain);
    int bad = 0;
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)_radio_text_start, 512u * 1024, MEM_R | MEM_X);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)_radio_data_start, 32768, MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)_utext_kobj_start, 16384, MEM_R | MEM_X);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)_udata_kobj_start, 32768, MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)g_rarena, ARENA_PAGES * 4096u, MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)g_rstack, STACK_PAGES * 4096u, MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, 0x40000000u, 512u * 1024, MEM_R | MEM_X);   /* ROM code */
    bad |= mem_domain_add(&g_rdomain, 0x4087c000u, 16384, MEM_R | MEM_W);        /* ROM data */
    bad |= mem_domain_add(&g_rdomain, 0x600A0000u, 65536, MEM_R | MEM_W);        /* MAC, baseband, PHY, modem */
    bad |= mem_domain_add(&g_rdomain, 0x600B0000u, 8192, MEM_R | MEM_W);         /* PMU, LP_AON */
    bad |= mem_domain_add(&g_rdomain, 0x60096000u, 4096, MEM_R | MEM_W);         /* PCR */
    /* UART0: the chip ROM's PHY and clock code waits for its transmitter to drain before it changes a
     * clock (uart_tx_wait_idle reads UART_STATUS) -- found as a load fault at 0x6000001c on the first
     * cold run. */
    bad |= mem_domain_add(&g_rdomain, 0x60000000u, 4096, MEM_R | MEM_W);
    /* And the SAR ADC, which holds the temperature sensor the PHY's calibration reads: not touched on a
     * warm chip, a load fault at 0x6000e000 on a cold one. */
    bad |= mem_domain_add(&g_rdomain, 0x6000E000u, 4096, MEM_R | MEM_W);
    if (bad || task_set_domain(sched_current_pid(), &g_rdomain) != 0) {
        printk("[radio] Refusing to enter U-mode: the domain is not enforceable\n");
        ctx->stage = RADIO_STAGE_FAILED;
        return;
    }
    arch_enter_user((void (*)(void))radio_main, (uintptr_t)g_rstack + STACK_PAGES * 4096u, 0, (uintptr_t)g_rstack, 0);
}

extern volatile int g_kobj_trace;
/* Bring-up probe: is the modem's time counter (0x600AD000) running, before and after the early
 * clock setup? M-mode, no domain: the setup code is plain register writes. */
void radio_c6_probe(void) {
#if defined(CONFIG_RADIO_C6)
    extern void radio_plat_early_init(void);
    volatile uint32_t *c = (volatile uint32_t *)0x600AD000u;
    uint32_t a = *c; for (volatile int i = 0; i < 400000; i++) { } uint32_t b = *c;
    cprintf("probe: modem counter before early init: %08x -> %08x (%s)\n", (unsigned)a, (unsigned)b, a != b ? "running" : "STOPPED");
    *(volatile uint32_t *)0x6009900cu = 0x777u;
    radio_plat_early_init();
    a = *c; for (volatile int i = 0; i < 400000; i++) { } b = *c;
    cprintf("probe: after early init: %08x -> %08x (%s)\n", (unsigned)a, (unsigned)b, a != b ? "running" : "STOPPED");
    {   /* the counter's rate against the system timer (XTAL-derived, 1 us resolution) */
        uint64_t t0 = time_get_us(); uint32_t c0 = *c;
        task_sleep_ms(200);
        uint64_t t1 = time_get_us(); uint32_t c1 = *c;
        cprintf("probe: modem counter %u ticks in %u us (system timer)\n", (unsigned)(c1 - c0), (unsigned)(t1 - t0));
    }
#endif
}

bool radio_c6_started(void) { return g_rpid >= 0; }

bool radio_c6_rf_on(void) {
#if defined(CONFIG_RADIO_C6)
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    return c && c->stage >= RADIO_STAGE_STARTED;
#else
    return false;
#endif
}

void radio_c6_stats(void) {
#if defined(CONFIG_RADIO_C6)
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (!c) { cprintf("radio: not started\n"); return; }
    radio_intr_report();
    cprintf("radio: joins=%u ", (unsigned)c->joins);
    cprintf("connected=%u disc_reason=%u rx_cb=%u rx_dropped=%u tx_sent=%u tx_err=%u rings rx %u/%u tx %u/%u\n",
            (unsigned)c->connected, (unsigned)c->disc_reason, (unsigned)c->rx_calls, (unsigned)c->rx_dropped,
            (unsigned)c->tx_sent, (unsigned)c->tx_errors, (unsigned)c->rings->rx.head, (unsigned)c->rings->rx.tail,
            (unsigned)c->rings->tx.head, (unsigned)c->rings->tx.tail);
#endif
}
int radio_netif_register(radio_ctx_t *ctx);
void radio_c6_trace(int on) { g_kobj_trace = on; }

int radio_c6_start(const char *ssid, const char *psk) {
    /* The blob's initialised data is loaded once, with the image; a second start would run
     * it from the state the first one left. Until the kernel keeps a pristine copy, one
     * start per boot. */
    if (g_rpid >= 0) { cprintf("radio: already started in this boot -- reset the board to start it again\n"); return 1; }
    g_rstack = palloc_pages_aligned(STACK_PAGES, STACK_PAGES);
    g_rarena = palloc_pages_aligned(ARENA_PAGES, ARENA_PAGES);
    if (!g_rstack || !g_rarena) { cprintf("radio: out of memory for the radio's heap/stack\n"); return 1; }
    radio_ctx_t *c0 = (radio_ctx_t *)g_rstack;
    c0->join = 0;
    if (ssid && psk) {
        uint32_t i = 0;
        for (; ssid[i] && i < 32; i++) c0->ssid[i] = ssid[i];
        c0->ssid[i] = 0;
        for (i = 0; psk[i] && i < 64; i++) c0->pass[i] = psk[i];
        c0->pass[i] = 0;
        c0->join = 1;
    }
    g_rpid = task_create("radio", radio_task, NULL);
    if (g_rpid < 0) { cprintf("radio: could not create the task\n"); return 1; }
    for (int i = 0; i < 40000 && sched_task_state(g_rpid) != TASK_DEAD && !((const radio_ctx_t *)g_rstack)->done; i++) task_sleep_ms(1);
    radio_intr_report();
    const radio_ctx_t *ctx = (const radio_ctx_t *)g_rstack;
    static const char *const names[] = { "not started", "FAILED", "OS shim up", "coexistence up", "esp_wifi_init_internal returned 0", "esp_wifi_start returned 0", "scan found access points", "joined (4-way handshake done)" };
    cprintf("radio: stage %u (%s), esp_wifi_init rc=0x%x, start rc=0x%x, task %s\n", (unsigned)ctx->stage,
            ctx->stage < 8 ? names[ctx->stage] : "?", (unsigned)ctx->rc_init, (unsigned)ctx->rc_start,
            sched_task_state(g_rpid) == TASK_DEAD ? "ended" : "still running");
    /* wlan0 exists whenever a join was intended and the radio is up -- not only when the first
     * attempt already succeeded: a node booting before its router (the power-cut case) joins later,
     * through the radio's rejoin loop, and must have an interface to join *with*. The link state follows
     * the association (kernel/radio_netif_c6.c), so DHCP and netsrv simply wait for it. */
    if (ctx->join && ctx->stage >= RADIO_STAGE_STARTED && radio_netif_register((radio_ctx_t *)g_rstack) == 0) {
        cprintf("radio: wlan0 registered (%s)\n", ctx->connected ? "link up" : "link down");
        net_stack_attach(radio_c6_netif());
        net_task_start();
        dhcp_start();
    }
    return ctx->stage >= RADIO_STAGE_STARTED ? 0 : 1;
}

#else

void radio_c6_trace(int on) { (void)on; }

int radio_c6_start(const char *ssid, const char *psk) {
    (void)ssid; (void)psk;
    cprintf("radio: this build has no Wi-Fi blob (no ESP-IDF tree at configure time)\n");
    return 1;
}

#endif

/* `radio rejoin`: drop the association and join again, as the supervisor below does on its own. */
void radio_c6_rejoin(void) {
#if defined(CONFIG_RADIO_C6)
    radio_ctx_t *c = (radio_ctx_t *)g_rstack;
    if (!c || !c->join) { cprintf("radio: not started with a network to join\n"); return; }
    c->rejoin_req = 1;
    cprintf("radio: rejoin requested\n");
#endif
}

/* --- unattended operation (45.8) ----------------------------------------
 *
 * The RP2350W's policy, applied to this radio (drivers/cyw43_rp2350.c, I9): **WLAN credentials in the
 * identity record are the intent to join.** No enable flag -- `wlan` stores them, and a board with none
 * does nothing here. A task, so the shell is usable at once; it starts the radio, which then joins and
 * keeps rejoining by itself (drivers/radio/radio_main.c's pump: capped backoff, never gives up), and stays
 * as the supervisor for the life of the board.
 *
 * What it supervises is the one failure the radio cannot see: a link that is "connected" and carries
 * nothing. The CYW43 learned it on a clock board that went silent for a day (2026-09-02) -- an AP that
 * stops without a goodbye leaves the association standing. On any real segment something arrives within
 * minutes (ARP, broadcasts), so RX_SILENCE_MS without a single received frame means the link is up in
 * name only, and a rejoin costs a few seconds where not doing one costs the board. */
#define RX_SILENCE_MS (5u * 60u * 1000u)

#if defined(CONFIG_RADIO_C6)
static void wlan_autostart_task(void *arg) {
    (void)arg;
    char ssid[NODE_WLAN_SSID_MAX + 1];
    uint8_t psk[NODE_WLAN_PSK_LEN];
    if (!node_wlan_ssid(ssid, sizeof(ssid)) || !node_wlan_psk(psk)) {
        memset(psk, 0, sizeof(psk));
        task_set_exit_status(0);
        return;                                  /* no credentials: not meant to join anything */
    }
    char hex[2 * NODE_WLAN_PSK_LEN + 1];
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < NODE_WLAN_PSK_LEN; i++) { hex[2 * i] = digits[psk[i] >> 4]; hex[2 * i + 1] = digits[psk[i] & 15]; }
    hex[2 * NODE_WLAN_PSK_LEN] = 0;
    memset(psk, 0, sizeof(psk));
    printk("[wlan] joining \"%s\" from the identity record\n", ssid);
    int rc = radio_c6_start(ssid, hex);
    memset(hex, 0, sizeof(hex));
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (rc != 0 || !c || c->stage < RADIO_STAGE_STARTED) {
        printk("[wlan] the radio did not start -- nothing to supervise\n");
        task_set_exit_status(1);
        return;
    }
    uint32_t last_rx = c->rx_calls;
    uint64_t last_change = time_get_ms();
    for (;;) {
        task_sleep_ms(10000);
        uint64_t now = time_get_ms();
        if (!c->connected || c->rx_calls != last_rx) { last_rx = c->rx_calls; last_change = now; continue; }
        if (now - last_change >= RX_SILENCE_MS) {
            printk("[wlan] connected, but nothing received for %u s -- rejoining\n", (unsigned)(RX_SILENCE_MS / 1000));
            ((radio_ctx_t *)c)->rejoin_req = 1;
            last_change = now;
        }
    }
}
#endif

void radio_c6_autostart(void) {
#if defined(CONFIG_RADIO_C6)
    if (task_create("wlan", wlan_autostart_task, NULL) < 0) printk("[wlan] could not create the autostart task\n");
#endif
}
