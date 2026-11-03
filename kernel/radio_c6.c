/* The kernel's half of the ESP32-C6 Wi-Fi radio:
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
 *   the peripherals   the modem (0x600A0000, 64 KB: MAC, baseband, PHY), PMU/LP_AON, PCR, UART0 and
 *                     the SAR ADC (k_radio_mmio), granted by the APM as well
 *
 * That is thirteen regions of the C6's sixteen PMP entries (MEM_DOMAIN_MAX_REGIONS).
 * PMP restricts the CPU and not the MAC's DMA (plan §4.5): this contains bugs,
 * and the `tainted` label (not here yet: 45.9) is what covers trust.
 *
 * Without the blob linked in (no ESP-IDF tree at configure time) none of this
 * exists and `radio` says so. */

#include "kernel/radio_c6.h"
#include "kernel/llp_node.h"
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
#include "kernel/device.h"
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

extern char _radio_text_start[], _radio_data_start[], _radio_bss_start[], _radio_bss_end[], _utext_kobj_start[], _udata_kobj_start[], _udata_kobj_end[];

#define ARENA_PAGES 16          /* 64 KB (one NAPOT region): the frame rings (10 KB) and the radio heap */
/* 8 KB: the main thread's stack, with the context (736 B) at its bottom. Measured on the C6-Zero
 * (66.3): 1.1 KB used through boot, a join and a loaded hour; 16 KB before. */
#define STACK_PAGES 2

/* single task: radio_c6_start() (one per boot, claimed there) sets them up
 * before it creates the radio task, which owns them after. */
static mem_domain_t g_rdomain;
static void *g_rstack;
static void *g_rarena;
static int g_rpid = -1;

/* The peripheral windows the radio's code touches, granted to it twice: by the PMP (its domain) and by
 * the APM below. */
static const struct { uint32_t base, size; } k_radio_mmio[] = {
    { 0x600A0000u, 65536 },   /* MAC, baseband, PHY, modem */
    { 0x600B0000u, 8192 },    /* PMU, LP_CLKRST, eFuse, LP_AON */
    { 0x60096000u, 4096 },    /* PCR */
    /* UART0: the chip ROM's PHY and clock code waits for its transmitter to drain before it changes a
     * clock (uart_tx_wait_idle reads UART_STATUS) -- found as a load fault at 0x6000001c on the first
     * cold run. */
    { 0x60000000u, 4096 },
    /* The SAR ADC, which holds the temperature sensor the PHY's calibration reads: not touched on a
     * warm chip, a load fault at 0x6000e000 on a cold one. */
    { 0x6000E000u, 4096 },
};
#define RADIO_MMIO_N (sizeof(k_radio_mmio) / sizeof(k_radio_mmio[0]))

/* The C6 has a second fence in front of its peripherals besides the PMP: the APM (access permission
 * management, TRM ch. 16). For the CPU it filters the peripherals and LP memory, not HP SRAM or ROM.
 * Its reset state denies every REE mode -- U-mode is REE0 -- by returning zeros to reads and dropping
 * writes, silently, and one region granting an access is enough (overlapping regions add up).
 *
 * The PMP alone would be enough behind LugalOS's own loader. Behind an ESP-IDF bootloader up to v5.2
 * (the factory one on boards with a C6 beside a P4) the PMP has a locked entry granting R/W on all of
 * 0x60000000-0x600fffff to every mode, which no later entry can take back. So the APM grants REE0
 * exactly the radio's windows above and nothing else, on every C6:
 *
 *   HP APM   region 0: the whole address space, REE2 only (the modem's DMA and every other master at
 *            its reset mode). It must stay enabled over everything: with filters on and no region
 *            covering an address, machine-mode accesses are filtered as well (IDF's TEE notes the same).
 *            Region 1: the SDIO slave's DMA (REE1), set by drivers/sdio_slave_esp32c6.c.
 *            Regions 2..: one per window, REE0 read/write.
 *   LP APM   region 0: the whole address space, REE1 and REE2. Regions 1..3: the windows in the LP
 *            peripherals (0x600B0000-0x600BFFFF), REE0 read/write: the PHY library switches the RF
 *            blocks' power (PMU_RF_PWC) itself while it calibrates, and behind a closed LP APM a cold
 *            chip's PLL calibration timed out ("pll_cal exceeds 2ms").
 *
 * What the APM does not fence is the CPU subsystem (0x20000000-0x2fffffff: the PLIC and CLINT), and the
 * same old bootloader locks a PMP entry granting that too: on those boards U-mode can still mask or
 * raise machine interrupts (ADR 007). */
#define HP_APM_BASE            0x60099000u
#define LP_APM_BASE            0x600B3800u
#define APM_FILTER_EN(b)       (*(volatile uint32_t *)((b) + 0x00u))
#define APM_REGION_START(b, n) (*(volatile uint32_t *)((b) + 0x04u + 12u * (uint32_t)(n)))
#define APM_REGION_END(b, n)   (*(volatile uint32_t *)((b) + 0x08u + 12u * (uint32_t)(n)))
#define APM_REGION_ATTR(b, n)  (*(volatile uint32_t *)((b) + 0x0Cu + 12u * (uint32_t)(n)))
#define APM_REE0_RW            0x006u           /* R (bit 2) | W (bit 1) of REE0's field [2:0] */
#define APM_REE1_ALL           0x070u
#define APM_REE2_ALL           0x700u
#define HP_APM_FIRST_REGION    2u
#define HP_APM_REGIONS         16u
#define LP_APM_REGIONS         4u
#define LP_PERI_LO             0x600B0000u
#define LP_PERI_HI             0x600BFFFFu

/* M-mode, before the radio's domain is entered. False if the windows do not fit the regions (then the
 * radio does not start: a U-mode with the APM half-configured would be neither confined nor working). */
static bool radio_apm_confine(void) {
    uint32_t hp = HP_APM_FIRST_REGION, lp = 1u, hp_en = 0, lp_en = 0;
    for (uint32_t i = 0; i < RADIO_MMIO_N; i++) {
        uint32_t lo = k_radio_mmio[i].base, hi = lo + k_radio_mmio[i].size - 1u;
        if (hp >= HP_APM_REGIONS) return false;
        APM_REGION_START(HP_APM_BASE, hp) = lo;
        APM_REGION_END(HP_APM_BASE, hp) = hi;
        APM_REGION_ATTR(HP_APM_BASE, hp) = APM_REE0_RW;
        hp_en |= 1u << hp++;
        if (lo >= LP_PERI_LO && hi <= LP_PERI_HI) {
            if (lp >= LP_APM_REGIONS) return false;
            APM_REGION_START(LP_APM_BASE, lp) = lo;
            APM_REGION_END(LP_APM_BASE, lp) = hi;
            APM_REGION_ATTR(LP_APM_BASE, lp) = APM_REE0_RW;
            lp_en |= 1u << lp++;
        }
    }
    APM_REGION_START(HP_APM_BASE, 0) = 0;
    APM_REGION_END(HP_APM_BASE, 0) = 0xFFFFFFFFu;
    APM_REGION_ATTR(HP_APM_BASE, 0) = APM_REE2_ALL;
    APM_FILTER_EN(HP_APM_BASE) |= 1u | hp_en;
    APM_REGION_START(LP_APM_BASE, 0) = 0;
    APM_REGION_END(LP_APM_BASE, 0) = 0xFFFFFFFFu;
    APM_REGION_ATTR(LP_APM_BASE, 0) = APM_REE1_ALL | APM_REE2_ALL;
    APM_FILTER_EN(LP_APM_BASE) |= 1u | lp_en;
    return true;
}

static void radio_task(void *arg) {
    (void)arg;
    radio_ctx_t *ctx = (radio_ctx_t *)g_rstack;
    ctx->arena = g_rarena;
    kos_init();
    ctx->tx_sem = (uint32_t)kos_sem_create(RN_TX_SLOTS, 0, (uintptr_t)&g_rdomain);   /* given once per queued frame */
    ctx->arena_bytes = ARENA_PAGES * 4096u;
    ctx->stage = RADIO_STAGE_NONE;

    bool apm_ok = radio_apm_confine();
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
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)_udata_kobj_start, (uint32_t)(_udata_kobj_end - _udata_kobj_start), MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)g_rarena, ARENA_PAGES * 4096u, MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, (uintptr_t)g_rstack, STACK_PAGES * 4096u, MEM_R | MEM_W);
    bad |= mem_domain_add(&g_rdomain, 0x40000000u, 512u * 1024, MEM_R | MEM_X);   /* ROM code */
    bad |= mem_domain_add(&g_rdomain, 0x4087c000u, 16384, MEM_R | MEM_W);        /* ROM data */
    for (uint32_t i = 0; i < RADIO_MMIO_N; i++)
        bad |= mem_domain_add(&g_rdomain, k_radio_mmio[i].base, k_radio_mmio[i].size, MEM_R | MEM_W);
    bad |= !apm_ok;
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

static const char *stage_name(uint32_t stage) {
    static const char *const names[] = { "not started", "FAILED", "OS shim up", "coexistence up", "esp_wifi_init_internal returned 0", "esp_wifi_start returned 0", "scan found access points", "joined (4-way handshake done)" };
    return stage < 8 ? names[stage] : "?";
}

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
    cprintf("radio: heap free %u (lowest %u), largest block %u, refused allocations %u\n",
            (unsigned)c->heap_free, (unsigned)c->heap_min_free, (unsigned)c->heap_largest, (unsigned)c->heap_fails);
#endif
}
int radio_netif_register(radio_ctx_t *ctx);
void radio_c6_trace(int on) { g_kobj_trace = on; }

int radio_c6_start(const char *ssid, const char *psk) {
    /* The blob's initialised data is loaded once, with the image; a second start would run
     * it from the state the first one left. Until the kernel keeps a pristine copy, one
     * start per boot. */
    /* Taken as one step: the shell's `radio start` and a join can arrive
     * together. Given back only by a failure before the task exists. */
    static volatile bool s_starting;   /* guarded by: the atomic exchange below */
    if (g_rpid >= 0 || __atomic_exchange_n(&s_starting, true, __ATOMIC_ACQ_REL)) {
        cprintf("radio: already started in this boot -- reset the board to start it again\n");
        return 1;
    }
    g_rstack = palloc_pages_aligned(STACK_PAGES, STACK_PAGES);
    g_rarena = palloc_pages_aligned(ARENA_PAGES, ARENA_PAGES);
    if (!g_rstack || !g_rarena) {
        cprintf("radio: out of memory for the radio's heap/stack\n");
        s_starting = false;
        return 1;
    }
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
    if (g_rpid < 0) { cprintf("radio: could not create the task\n"); s_starting = false; return 1; }
    for (int i = 0; i < 40000 && sched_task_state(g_rpid) != TASK_DEAD && !((const radio_ctx_t *)g_rstack)->done; i++) task_sleep_ms(1);
    radio_intr_report();
    const radio_ctx_t *ctx = (const radio_ctx_t *)g_rstack;
    cprintf("radio: stage %u (%s), esp_wifi_init rc=0x%x, start rc=0x%x, task %s\n", (unsigned)ctx->stage,
            stage_name(ctx->stage), (unsigned)ctx->rc_init, (unsigned)ctx->rc_start,
            sched_task_state(g_rpid) == TASK_DEAD ? "ended" : "still running");
    /* wlan0 exists whenever a join was intended and the radio is up -- not only when the first
     * attempt already succeeded: a node booting before its router (the power-cut case) joins later,
     * through the radio's rejoin loop, and must have an interface to join *with*. The link state follows
     * the association (kernel/radio_netif_c6.c), so DHCP and netsrv simply wait for it. */
    if (ctx->join && ctx->stage >= RADIO_STAGE_STARTED && radio_netif_register((radio_ctx_t *)g_rstack) == 0) {
        cprintf("radio: wlan0 registered (%s)\n", ctx->connected ? "link up" : "link down");
        /* With an LLP host up, the interface is the bridge's and this node's
         * stack never touches it; otherwise the stack takes it, and the
         * bridge takes it over later (kernel/llp_node.c). */
        if (!llp_node_offer_netif(radio_c6_netif())) {
            net_task_start();
            dhcp_start();
        }
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

bool radio_c6_started(void) { return false; }

bool radio_c6_rf_on(void) { return false; }

void radio_c6_probe(void) {
    cprintf("probe: this build has no Wi-Fi blob\n");
}

void radio_c6_stats(void) {
    cprintf("radio: this build has no Wi-Fi blob\n");
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
/* And the one it cannot recover from: a radio that has lost its link and fails every rejoin, which
 * happened twice on 2026-10-31 (a C6-Zero 3.5 h into a run, a NANO's C6) and needed a restart by hand.
 * Not joined for this long, the node restarts. A node whose access point is off therefore restarts
 * every 15 minutes until it is back: a few seconds each time, and the rejoin it was attempting anyway. */
#define JOIN_GIVE_UP_MS (15u * 60u * 1000u)

#if defined(CONFIG_RADIO_C6)
/* The network the `wlan` task joins: from the identity record at boot, or from
 * an LLP host that holds the credentials itself (radio_c6_join()). The key is
 * kept only until the radio has it. */
/* guarded by: g_join_busy, claimed with an atomic exchange before the
 * credentials are written; the join task owns them from then on. */
static char g_join_ssid[NODE_WLAN_SSID_MAX + 1];
static char g_join_psk[2 * NODE_WLAN_PSK_LEN + 1];
static volatile bool g_join_busy;

/* Starts the radio on g_join_*, then supervises the link for the life of the
 * board. `arg` non-NULL: the credentials came from the LLP host. */
/* accepted: set once by the join task before anything reads it. */
static volatile bool g_joined_for_host;

/* The identity's Wi-Fi credentials changed (`wlan`, an owner reset, /dev/owner). The radio keeps the ones
 * it joined with and cannot be told new ones while it runs, so a stand-alone C6 restarts: it then joins
 * with the new credentials, or without any stays off the network (dark), as an owner reset means. A C6
 * that is an LLP host's radio joins with the host's credentials, not its own: nothing to do. */
void radio_credentials_changed(void) {
    if (g_joined_for_host) return;
    printk("[wlan] Wi-Fi credentials %s: restarting\n", node_wlan_configured() ? "changed" : "cleared");
    (void)board_reset_later(false, 1000);
}

static void wlan_join_task(void *arg) {
    g_joined_for_host = arg != NULL;
    printk("[wlan] joining \"%s\" %s\n", g_join_ssid, arg ? "for the LLP host" : "from the identity record");
    int rc = radio_c6_start(g_join_ssid, g_join_psk);
    memset(g_join_psk, 0, sizeof(g_join_psk));
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (rc != 0 || !c || c->stage < RADIO_STAGE_STARTED) {
        printk("[wlan] the radio did not start -- nothing to supervise\n");
        task_set_exit_status(1);
        return;
    }
    uint32_t last_rx = c->rx_calls;
    uint64_t last_change = time_get_ms();
    uint64_t down_since = 0;
    for (;;) {
        task_sleep_ms(10000);
        uint64_t now = time_get_ms();
        if (!c->connected) {
            if (!down_since) down_since = now;
            else if (now - down_since >= JOIN_GIVE_UP_MS) {
                printk("[wlan] not joined for %u s (radio heap %u bytes free, largest %u, %u refused since start): "
                       "restarting\n", (unsigned)((now - down_since) / 1000u), (unsigned)c->heap_free,
                       (unsigned)c->heap_largest, (unsigned)c->heap_fails);
                (void)board_reset_later(false, 500);
                down_since = now;
            }
        } else {
            down_since = 0;
        }
        if (!c->connected || c->rx_calls != last_rx) { last_rx = c->rx_calls; last_change = now; continue; }
        if (now - last_change >= RX_SILENCE_MS) {
            printk("[wlan] connected, but nothing received for %u s -- rejoining\n", (unsigned)(RX_SILENCE_MS / 1000));
            ((radio_ctx_t *)c)->rejoin_req = 1;
            last_change = now;
        }
    }
}

static void wlan_autostart_task(void *arg) {
    (void)arg;
    /* The credentials are claimed before they are written: an LLP host may be
     * handing over its own at the same moment (radio_c6_join()). */
    if (__atomic_exchange_n(&g_join_busy, true, __ATOMIC_ACQ_REL)) { task_set_exit_status(0); return; }
    uint8_t psk[NODE_WLAN_PSK_LEN];
    if (!node_wlan_ssid(g_join_ssid, sizeof(g_join_ssid)) || !node_wlan_psk(psk)) {
        memset(psk, 0, sizeof(psk));
        g_join_busy = false;
        task_set_exit_status(0);
        return;                                  /* no credentials: not meant to join anything */
    }
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < NODE_WLAN_PSK_LEN; i++) {
        g_join_psk[2 * i] = digits[psk[i] >> 4];
        g_join_psk[2 * i + 1] = digits[psk[i] & 15];
    }
    g_join_psk[2 * NODE_WLAN_PSK_LEN] = 0;
    memset(psk, 0, sizeof(psk));
    wlan_join_task(NULL);
}
#endif

int radio_c6_join(const char *ssid, const char *psk_hex) {
#if defined(CONFIG_RADIO_C6)
    size_t n = strlen(ssid), k = strlen(psk_hex);
    if (n == 0 || n > NODE_WLAN_SSID_MAX || k != 2u * NODE_WLAN_PSK_LEN) return -1;
    for (size_t i = 0; i < k; i++) {
        char ch = psk_hex[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return -1;
    }
    if (g_rpid >= 0 || __atomic_exchange_n(&g_join_busy, true, __ATOMIC_ACQ_REL)) return -1;
    memcpy(g_join_ssid, ssid, n + 1);
    memcpy(g_join_psk, psk_hex, k + 1);
    if (task_create("wlan", wlan_join_task, (void *)1) < 0) {
        memset(g_join_psk, 0, sizeof(g_join_psk));
        g_join_busy = false;
        return -1;
    }
    return 0;
#else
    (void)ssid; (void)psk_hex;
    return -1;
#endif
}

/* /dev/wlan (fs/vfs_server.c): a Plan 9 ctl file. `join SSID PSKHEX` and
 * `rejoin` are written to it; reading it says where the radio is. */
int radio_c6_ctl_write(const char *buf, uint32_t len) {
    char line[128];
    uint32_t n = len;
    if (n >= sizeof(line)) return -1;
    memcpy(line, buf, n);
    line[n] = 0;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    int rc = -1;
    if (strncmp(line, "join ", 5) == 0) {
        char *ssid = line + 5;
        while (*ssid == ' ') ssid++;
        char *psk = ssid;
        while (*psk && *psk != ' ') psk++;
        if (*psk) {
            *psk++ = 0;
            while (*psk == ' ') psk++;
            rc = radio_c6_join(ssid, psk);
        }
    } else if (strcmp(line, "rejoin") == 0) {
#if defined(CONFIG_RADIO_C6)
        radio_ctx_t *c = (radio_ctx_t *)g_rstack;
        if (c && c->join) { c->rejoin_req = 1; rc = 0; }
#endif
    } else if (strcmp(line, "scan") == 0) {
        /* Serviced by the radio's supervision loop, so only once it joins a network; the results
         * replace /proc/wlanscan's a few seconds later. */
#if defined(CONFIG_RADIO_C6)
        radio_ctx_t *c = (radio_ctx_t *)g_rstack;
        if (c && c->join) { c->scan_req = 1; rc = 0; }
#endif
    }
    memset(line, 0, sizeof(line));
    return rc == 0 ? (int)len : -1;
}

uint32_t radio_c6_ctl_read(char *out, uint32_t cap) {
    netif_t *nif = radio_c6_netif();
    const char *state = !radio_c6_started() ? "idle" : !nif ? "starting" :
                        netif_link_up(nif) ? "joined" : "not joined";
    uint32_t n = (uint32_t)ksnprintf(out, cap, "state=%s\n", state);
#if defined(CONFIG_RADIO_C6)
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (c && n < cap) {
        const volatile uint8_t *b = c->bssid;
        bool joined = c->connected && c->rssi > -127;
        n += (uint32_t)ksnprintf(out + n, cap - n,
            "ssid=%s\nbssid=%02x:%02x:%02x:%02x:%02x:%02x\nchannel=%u\nrssi=%d\n"
            "mac=%02x:%02x:%02x:%02x:%02x:%02x\njoins=%u\nlast_disconnect=%u\n",
            c->join ? c->ssid : "",
            joined ? b[0] : 0, joined ? b[1] : 0, joined ? b[2] : 0, joined ? b[3] : 0, joined ? b[4] : 0, joined ? b[5] : 0,
            joined ? (unsigned)c->chan : 0u, joined ? (int)c->rssi : -127,
            c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
            (unsigned)c->joins, (unsigned)c->disc_reason);
    }
#endif
    return n < cap ? n : cap;
}

int radio_c6_rssi(void) {
#if defined(CONFIG_RADIO_C6)
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (c && c->connected) return (int)c->rssi;
#endif
    return -127;
}

uint32_t radio_c6_scan_render(char *out, uint32_t cap) {
#if defined(CONFIG_RADIO_C6)
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (!c) return (uint32_t)ksnprintf(out, cap, "aps=0\n");
    static radio_ap_t aps[RADIO_SCAN_MAX];   /* accepted: two readers at once can mix their lists */
    uint32_t count = 0;
    for (int tries = 0; tries < 8; tries++) {         /* the radio publishes rarely: a retry is enough */
        uint32_t seq = c->scan_seq;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (seq & 1u) { task_sleep_ms(5); continue; }
        count = c->scan_n < RADIO_SCAN_MAX ? c->scan_n : RADIO_SCAN_MAX;
        memcpy(aps, (const void *)c->scan, count * sizeof aps[0]);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (c->scan_seq == seq) break;
        count = 0;
    }
    uint32_t n = (uint32_t)ksnprintf(out, cap, "aps=%u\n", (unsigned)count);
    for (uint32_t i = 0; i < count; i++) {
        char line[80];
        const radio_ap_t *a = &aps[i];
        uint32_t k = (uint32_t)ksnprintf(line, sizeof line, "%02x:%02x:%02x:%02x:%02x:%02x %u %d %u %s\n",
                                         a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5],
                                         (unsigned)a->chan, (int)a->rssi, (unsigned)a->auth, a->ssid);
        if (k >= sizeof line || n + k >= cap) break;
        memcpy(out + n, line, k);
        n += k;
    }
    if (n < cap) out[n] = 0;
    return n;
#else
    return (uint32_t)ksnprintf(out, cap, "aps=0\n");
#endif
}

uint32_t radio_c6_proc_render(char *out, uint32_t cap) {
#if defined(CONFIG_RADIO_C6)
    const radio_ctx_t *c = (const radio_ctx_t *)g_rstack;
    if (!c) return (uint32_t)ksnprintf(out, cap, "blob=yes\nstarted=no\n");
    uint32_t n = (uint32_t)ksnprintf(out, cap,
        "blob=yes\nstarted=yes\nstage=%u (%s)\nrc_init=0x%x\nrc_start=0x%x\naps_at_boot=%u\n"
        "connected=%u\njoins=%u\nlast_disconnect=%u\nrx_frames=%u\nrx_dropped=%u\ntx_frames=%u\ntx_errors=%u\n",
        (unsigned)c->stage, stage_name(c->stage), (unsigned)c->rc_init, (unsigned)c->rc_start, (unsigned)c->ap_count,
        (unsigned)c->connected, (unsigned)c->joins, (unsigned)c->disc_reason, (unsigned)c->rx_calls,
        (unsigned)c->rx_dropped, (unsigned)c->tx_sent, (unsigned)c->tx_errors);
    if (n < cap)
        n += (uint32_t)ksnprintf(out + n, cap - n, "heap_free=%u\nheap_min_free=%u\nheap_largest=%u\nheap_refused=%u\n",
                                 (unsigned)c->heap_free, (unsigned)c->heap_min_free, (unsigned)c->heap_largest,
                                 (unsigned)c->heap_fails);
    if (n < cap && c->rings)
        n += (uint32_t)ksnprintf(out + n, cap - n, "ring_rx=%u/%u\nring_tx=%u/%u\n",
                                 (unsigned)c->rings->rx.head, (unsigned)c->rings->rx.tail,
                                 (unsigned)c->rings->tx.head, (unsigned)c->rings->tx.tail);
    return n < cap ? n : cap;
#else
    return (uint32_t)ksnprintf(out, cap, "blob=no\n");
#endif
}

void radio_c6_autostart(void) {
#if defined(CONFIG_RADIO_C6)
    if (task_create("wlan", wlan_autostart_task, NULL) < 0) printk("[wlan] could not create the autostart task\n");
#endif
}
