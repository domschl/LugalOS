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
#include "kernel/radio_intr.h"
#include "kernel/palloc.h"
#include "kernel/mem_domain.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include "../drivers/radio/radio_main.h"
#include <stddef.h>
#include <stdint.h>

/* Keeps .radio_text alive in a build with no blob, so the flash layout does not
 * depend on whether there is one (linker/esp32c6.ld). */
const uint32_t g_radio_text_marker __attribute__((section(".radio_text_marker"), used)) = 0x31444152u;

#if defined(CONFIG_RADIO_C6)

extern char _radio_text_start[], _radio_data_start[], _radio_bss_start[], _radio_bss_end[], _utext_kobj_start[], _udata_kobj_start[];

#define ARENA_PAGES 16          /* 64 KB: the radio's heap */
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

static void radio_task(void *arg) {
    (void)arg;
    radio_ctx_t *ctx = (radio_ctx_t *)g_rstack;
    ctx->arena = g_rarena;
    ctx->arena_bytes = ARENA_PAGES * 4096u;
    ctx->stage = RADIO_STAGE_NONE;

    HP_APM_REGION0_PMS_ATTR = 0x777u;
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
    bad |= mem_domain_add(&g_rdomain, 0x6000E000u, 4096, MEM_R | MEM_W);         /* SAR ADC (the PHY's temperature sensor) */
    if (bad || task_set_domain(sched_current_pid(), &g_rdomain) != 0) {
        printk("[radio] Refusing to enter U-mode: the domain is not enforceable\n");
        ctx->stage = RADIO_STAGE_FAILED;
        return;
    }
    arch_enter_user((void (*)(void))radio_main, (uintptr_t)g_rstack + STACK_PAGES * 4096u, 0, (uintptr_t)g_rstack, 0);
}

extern volatile int g_kobj_trace;
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
    for (int i = 0; i < 40000 && sched_task_state(g_rpid) != TASK_DEAD; i++) task_sleep_ms(1);
    radio_intr_report();
    const radio_ctx_t *ctx = (const radio_ctx_t *)g_rstack;
    static const char *const names[] = { "not started", "FAILED", "OS shim up", "coexistence up", "esp_wifi_init_internal returned 0", "esp_wifi_start returned 0", "scan found access points", "joined (4-way handshake done)" };
    cprintf("radio: stage %u (%s), esp_wifi_init rc=0x%x, start rc=0x%x, task %s\n", (unsigned)ctx->stage,
            ctx->stage < 8 ? names[ctx->stage] : "?", (unsigned)ctx->rc_init, (unsigned)ctx->rc_start,
            sched_task_state(g_rpid) == TASK_DEAD ? "ended" : "still running");
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
