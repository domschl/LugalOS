/*
 * LugalOS driver: the hardware watchdog on the ESP32-C6 -- 45.8, plan/phase45_esp32c6.md.
 *
 * An unattended node must not stay hung. MWDT0 (timer group 0's watchdog) is armed with one stage:
 * no feed for WDT_TIMEOUT_MS and it resets the whole digital system. A task at normal priority feeds
 * it every quarter of that -- so what it proves is that the scheduler still runs ordinary tasks: a
 * hang with interrupts masked, a scheduler wedged on a lock, a higher-tier task that never yields all
 * stop the feeding, and the board comes back by itself.
 *
 * The flash-boot watchdogs the ROM arms are disarmed in .boot.text (arch/riscv/common/xip_esp32c6.c);
 * this arms MWDT0 again, deliberately, once the scheduler exists. The register layout is IDF's
 * (components/soc/esp32c6/register/soc/timer_group_reg.h, esp_hal_wdt/esp32c6/include/hal/mwdt_ll.h).
 * Its clock is the PCR's default for it, the 40 MHz crystal, so a prescaler of 40000 counts
 * milliseconds.
 *
 * The cause of the last reset is the ROM's (rtc_get_reset_reason): reported at boot, so a node that
 * restarted itself says so instead of looking like one that was simply switched on.
 */

#include "drivers/watchdog.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define WDT_WKEY            0x50D83AA1u
#define TIMG0_BASE          0x60008000u
#define TIMG0_WDTCONFIG0    (TIMG0_BASE + 0x48u)
#define TIMG0_WDTCONFIG1    (TIMG0_BASE + 0x4cu)
#define TIMG0_WDTCONFIG2    (TIMG0_BASE + 0x50u)     /* stage 0 hold, in WDT clock ticks */
#define TIMG0_WDTFEED       (TIMG0_BASE + 0x60u)
#define TIMG0_WDTWPROTECT   (TIMG0_BASE + 0x64u)

#define WDT_EN              (1u << 31)
#define WDT_STG0_S          29
#define WDT_STG_RESET_SYSTEM 3u
#define WDT_CONF_UPDATE_EN  (1u << 22)
#define WDT_SYS_RESET_LEN_S 15
#define WDT_CPU_RESET_LEN_S 18
#define WDT_PRESCALE_S      16
#define WDT_DIVCNT_RST      (1u << 0)

#define WDT_TIMEOUT_MS      30000u

typedef uint32_t (*rom_reset_reason_fn)(int cpu);
#define rom_reset_reason ((rom_reset_reason_fn)0x40000018UL)   /* rtc_get_reset_reason, esp32c6.rom.ld */

static volatile bool g_armed, g_withhold;
static uint32_t g_reason;
static uint32_t g_feeds;

static void wdt_feed(void) {
    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;
    REG(TIMG0_WDTFEED) = 1;
    REG(TIMG0_WDTWPROTECT) = 0;
    g_feeds++;
}

static void wdt_arm(uint32_t timeout_ms) {
    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;
    REG(TIMG0_WDTCONFIG0) = 0;                                       /* off while it is reconfigured */
    REG(TIMG0_WDTCONFIG1) = (40000u << WDT_PRESCALE_S) | WDT_DIVCNT_RST;   /* 40 MHz / 40000 = 1 kHz */
    REG(TIMG0_WDTCONFIG2) = timeout_ms;
    REG(TIMG0_WDTCONFIG0) = WDT_EN | (WDT_STG_RESET_SYSTEM << WDT_STG0_S) |
                            (7u << WDT_SYS_RESET_LEN_S) | (7u << WDT_CPU_RESET_LEN_S);
    REG(TIMG0_WDTCONFIG0) |= WDT_CONF_UPDATE_EN;                     /* latch the new configuration */
    REG(TIMG0_WDTFEED) = 1;
    REG(TIMG0_WDTWPROTECT) = 0;
}

const char *watchdog_reset_reason(void) {
    switch (g_reason) {
    case 0x01: return "power-on";
    case 0x03: return "software (system)";
    case 0x05: return "deep sleep";
    case 0x07: return "watchdog (MWDT0, system)";
    case 0x08: return "watchdog (MWDT1, system)";
    case 0x09: return "watchdog (RTC, system)";
    case 0x0B: return "watchdog (MWDT0, CPU)";
    case 0x0C: return "software (CPU)";
    case 0x0D: return "watchdog (RTC, CPU)";
    case 0x0F: return "brown-out";
    case 0x10: return "watchdog (RTC, system and RTC)";
    case 0x11: return "watchdog (MWDT1, CPU)";
    case 0x12: return "super watchdog";
    case 0x14: return "eFuse CRC error";
    case 0x15: return "USB UART";
    case 0x16: return "USB JTAG";
    case 0x18: return "JTAG (CPU)";
    default:   return "unknown";
    }
}

bool watchdog_last_reset_was_watchdog(void) {
    return g_reason == 0x07 || g_reason == 0x08 || g_reason == 0x09 || g_reason == 0x0B ||
           g_reason == 0x0D || g_reason == 0x10 || g_reason == 0x11 || g_reason == 0x12;
}

static void wdog_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!g_withhold) wdt_feed();
        task_sleep_ms(WDT_TIMEOUT_MS / 4);
    }
}

void watchdog_start(void) {
    g_reason = rom_reset_reason(0);
    printk("[WDT] last reset: %s (0x%02x)%s\n", watchdog_reset_reason(), (unsigned)g_reason,
           watchdog_last_reset_was_watchdog() ? " -- this node restarted itself" : "");
    wdt_arm(WDT_TIMEOUT_MS);
    if (task_create_sized("wdog", wdog_task, NULL, 1) < 0) {   /* one page: it sleeps and writes a register */
        /* Without its feeder the watchdog would reset a healthy board in 30 s: disarm it instead. */
        REG(TIMG0_WDTWPROTECT) = WDT_WKEY;
        REG(TIMG0_WDTCONFIG0) = 0;
        REG(TIMG0_WDTWPROTECT) = 0;
        printk("[WDT] could not create the feeder task -- watchdog left off\n");
        return;
    }
    g_armed = true;
    printk("[WDT] armed: a system reset after %u s without a feed\n", (unsigned)(WDT_TIMEOUT_MS / 1000));
}

void watchdog_report(void) {
    printk("watchdog: %s, timeout %u s, %u feeds%s\n", g_armed ? "armed" : "off",
           (unsigned)(WDT_TIMEOUT_MS / 1000), (unsigned)g_feeds, g_withhold ? ", feeding WITHHELD" : "");
    printk("last reset: %s (0x%02x)\n", watchdog_reset_reason(), (unsigned)g_reason);
}

/* `wdt test`: stop feeding, as a hung scheduler would. The board resets within the timeout and the
 * next boot reports a watchdog reset -- the whole path, proven on the hardware. */
void watchdog_withhold(void) {
    g_withhold = true;
    printk("[WDT] feeding withheld: expect a reset within %u s\n", (unsigned)(WDT_TIMEOUT_MS / 1000));
}
