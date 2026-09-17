/* Starting the ESP32-P4's second HP core. 34.9,
 * plan/phase34_esp32p4_pll_bringup.md.
 *
 * ## Why this milestone is a counter and nothing else
 *
 * `kernel/smp.c:104` records what happened when RP2350's X3 skipped this
 * step, and the reasoning transfers whole:
 *
 *     "The first attempt skipped this step and sent core 1 straight into
 *      secondary_main() -- trap_init(), the ticker, the scheduler, printk. It
 *      wedged, and because everything downstream is silent when it does,
 *      there was no way to say which half had failed."
 *
 * So core 1's entire program here is an increment. If the counter moves, then
 * the stall release, the clock, the reset, the ROM's hand-off and core 1's
 * first instructions are *all* good, and anything still broken afterwards is
 * above that line. If it does not move, nothing above that line works and
 * there is no point looking higher.
 *
 * ## No stack, deliberately
 *
 * The probe is assembly with no calls, so core 1 needs no stack, no `gp` and
 * no `tp`. That is not minimalism for its own sake: `linker/esp32p4.ld` has
 * no `_stack_secondary` at all (§4.2), and giving core 1 a stack is 34.10's
 * job. Doing it here would mean this milestone could fail for two reasons
 * instead of one.
 *
 * It does execute from flash like everything else, so the probe also
 * exercises core 1's instruction fetch through the XIP window. If `ICACHE1`
 * is off the counter still moves -- uncached fetch is slow, not broken --
 * which is exactly the failure 34.12 is told to read as a diagnosis.
 *
 * ## The launch, and where each register came from
 *
 * ESP-IDF's `start_other_core()` (components/esp_system/port/cpu_start.c),
 * with the register fields from `hal/cpu_utility_ll.h`:
 *
 *     esp_cpu_unstall(1);                                 // PMU stall code
 *     cpu_utility_ll_enable_clock_and_reset_app_cpu();    // clock + reset
 *     cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();  // empty here
 *     ets_set_appcpu_boot_addr((uint32_t)call_start_cpu1);
 *
 * The boot address is written **last**, after the core is already out of
 * reset, because that is the handshake: core 1 comes up in ROM code which
 * spins until the address is non-zero and then jumps to it. IDF's own
 * `call_start_cpu1()` clears it again on arrival, which is the other half of
 * the same protocol and is why this file does too.
 *
 * 34.8 measured that core 1 is held by **reset**, not by the stall -- the
 * stall code reads 0x00, neither 0x86 nor 0xff. The unstall write is made
 * anyway: a launch that works because a register happens to read zero is a
 * launch that breaks the first time something else writes it.
 */

#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4)

#include <stdint.h>
#include <stdbool.h>
#include "arch/smp_esp32p4.h"
#include "kernel/console.h"
#include "kernel/time.h"

#define P4_REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* HP_SYS_CLKRST, the block drivers/uart_esp32p4.c and clk_esp32p4.c share. */
#define P4_CLKRST_BASE          0x500E6000UL
#define P4_SOC_CLK_CTRL0        (P4_CLKRST_BASE + 0x14)
#define CORE1_CPU_CLK_EN        (1u << 4)      /* reset default 0 */
#define P4_HP_RST_EN0           (P4_CLKRST_BASE + 0xc0)
#define RST_EN_CORE1_GLOBAL     (1u << 8)      /* reset default 1 = held */

/* PMU: LPAON + 0x5000. */
#define P4_PMU_CPU_SW_STALL     (0x50115000UL + 0x200)
#define HPCORE1_STALL_CODE_S    16
#define HPCORE1_STALL_CODE_M    0xFFu
#define STALL_CODE_RUN          0xFFu

/* HP_SYSTEM, HPPERIPH1 + 0x25000. */
#define P4_HP_SYSTEM_CORESTALL  0x500E5064UL
#define CORE1_CORESTALLED_ST    (1u << 1)

/* ROM 0x4fc000a8, identical in esp32p4.rom.ld and esp32p4.rom.eco0_4.ld --
 * the check 34.1 established as necessary after finding ets_clk_get_cpu_freq
 * at different addresses in the two. */
#define ROM_ETS_SET_APPCPU_BOOT_ADDR  0x4fc000a8u
typedef void (*rom_set_appcpu_boot_addr_t)(uint32_t addr);

/* Core 1's entire program, and the evidence.
 *
 * In .bss, which entry.S cleared long before any shell command can run, so a
 * non-zero value here can only have been written by core 1. Volatile because
 * core 0 reads it while another core writes it -- and on this chip that is
 * all it takes, since the L1 data cache is shared between the two (34.8, TRM
 * Figure 9.3-3). On a part with per-core data caches this would need
 * maintenance; here it needs a keyword. */
volatile uint32_t g_p4_core1_ticks;

/* The probe itself. `naked` because there is no stack to build a frame on,
 * and a plain infinite loop because core 1 has nowhere to return to. */
__attribute__((naked, noreturn)) static void core1_probe_entry(void) {
    __asm__ __volatile__(
        "la    t0, g_p4_core1_ticks\n"
        "1:\n"
        "lw    t1, 0(t0)\n"
        "addi  t1, t1, 1\n"
        "sw    t1, 0(t0)\n"
        "j     1b\n"
        ::: "t0", "t1", "memory");
}

bool esp32p4_core1_launch_probe(void) {
    /* 1. Unstall. Belt-and-braces on this board; see the header. */
    uint32_t stall = P4_REG(P4_PMU_CPU_SW_STALL);
    stall &= ~(HPCORE1_STALL_CODE_M << HPCORE1_STALL_CODE_S);
    stall |= STALL_CODE_RUN << HPCORE1_STALL_CODE_S;
    P4_REG(P4_PMU_CPU_SW_STALL) = stall;

    uint64_t t0 = time_get_us();
    while (P4_REG(P4_HP_SYSTEM_CORESTALL) & CORE1_CORESTALLED_ST) {
        if (time_get_us() - t0 > 10000u) {
            cprintf("[SMP] core 1 stayed stalled\n");
            return false;
        }
    }

    /* 2. Clock, then reset -- IDF's order, and it checks each before writing
     * because a debugger may have gone first. 34.8 found neither touched. */
    if (!(P4_REG(P4_SOC_CLK_CTRL0) & CORE1_CPU_CLK_EN)) {
        P4_REG(P4_SOC_CLK_CTRL0) |= CORE1_CPU_CLK_EN;
    }
    if (P4_REG(P4_HP_RST_EN0) & RST_EN_CORE1_GLOBAL) {
        P4_REG(P4_HP_RST_EN0) &= ~RST_EN_CORE1_GLOBAL;
    }

    /* 3. The boot address, last, because it is the handshake rather than a
     * setting: core 1 is already running ROM code that waits for it. */
    ((rom_set_appcpu_boot_addr_t)(uintptr_t)ROM_ETS_SET_APPCPU_BOOT_ADDR)
        ((uint32_t)(uintptr_t)core1_probe_entry);
    return true;
}

void esp32p4_core1_probe_report(void) {
    uint32_t before = g_p4_core1_ticks;

    if (!esp32p4_core1_launch_probe()) {
        return;
    }

    /* A tenth of a second is many millions of increments if core 1 is
     * running at all, and short enough that a dead core costs nothing. */
    uint64_t t0 = time_get_us();
    while (time_get_us() - t0 < 100000u) { }

    uint32_t after = g_p4_core1_ticks;
    if (after != before) {
        cprintf("[SMP] CORE1_ALIVE -- counter %u -> %u in 100 ms\n",
                (unsigned)before, (unsigned)after);
    } else {
        cprintf("[SMP] CORE1_SILENT -- launch completed, counter never moved "
                "(was %u)\n", (unsigned)before);
    }
}

#endif /* CONFIG_BOARD_ESP32P4 */
