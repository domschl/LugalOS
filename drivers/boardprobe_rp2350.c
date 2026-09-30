/*
 * boardprobe -- 36.0, plan/phase36_rp2350_lcd7_terminal.md.
 *
 * Three questions, answered from the board rather than assumed:
 *
 *   1. Which die stepping is this? RP2350-E9 (pads with pull-downs latching
 *      high through input-buffer leakage) is present on A2 and fixed in A3
 *      (datasheet Appendix C). A USB host samples D+/D- through pull-downs to
 *      see a device leave, so the PIO-USB design depends on the answer.
 *   2. What is on the PIO-USB port, and at what speed? A device announces
 *      itself with a 1.5 K pull-up: on D+ for full speed, on D- for low
 *      speed. With our pull-downs on both lines, the pulled-up one reads high.
 *   3. Is GP0 still untouched? On the RP2350-LCD-7 it is the PSRAM's /CS,
 *      sharing the QSPI data lines with the flash we execute from
 *      (cmake/board-rp2350-terminal.cmake explains the consequence). Read
 *      back from IO_BANK0 rather than trusted to the board file.
 *
 * Guard-by-pin-map, like drivers/uart1_link_rp2350.c: always compiled on
 * RP2350, empty unless the board file declares a PIO-USB port.
 *
 * Register provenance (RP2350 datasheet, RP-008373-DS-2):
 *   SYSINFO_BASE 0x40000000; CHIP_ID +0x00: [31:28] REVISION, [27:12] PART,
 *     [11:1] MANUFACTURER, [0] 1. REVISION 0x2 = A2, 0x3 = A3, **0x8 = A4**
 *     (Appendix C -- A4 is not 0x4). PACKAGE_SEL +0x04: bit 0, 0 = QFN80.
 *   SIO_BASE 0xd0000000; GPIO_HI_IN +0x008, bits [15:0] = GPIO32..47.
 *   IO_BANK0_BASE 0x40028000; GPIOn_CTRL at +0x004 + 8n, FUNCSEL [4:0],
 *     reset value 0x1f (NULL).
 *   PADS_BANK0_BASE 0x40038000; GPIOn at +0x004 + 4n (n = 0..47):
 *     bit 8 ISO, 7 OD, 6 IE, 5:4 DRIVE, 3 PUE, 2 PDE, 1 SCHMITT, 0 SLEWFAST.
 */

#include "drivers/boardprobe.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdint.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)

/* The PIO-USB pins are in the high bank on the only board that has one.
 * `1u << pin` for pin >= 32 is undefined behaviour in C and the wrong register
 * in practice, so the bank is chosen here, once, at compile time. */
_Static_assert(CONFIG_PIOUSB_DP_GPIO >= 32 && CONFIG_PIOUSB_DP_GPIO <= 47,
               "boardprobe reads the PIO-USB pins from GPIO_HI_IN");
_Static_assert(CONFIG_PIOUSB_DM_GPIO >= 32 && CONFIG_PIOUSB_DM_GPIO <= 47,
               "boardprobe reads the PIO-USB pins from GPIO_HI_IN");

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define SYSINFO_BASE        0x40000000UL
#define SYSINFO_CHIP_ID     (SYSINFO_BASE + 0x00)
#define SYSINFO_PACKAGE_SEL (SYSINFO_BASE + 0x04)

#define SIO_BASE            0xd0000000UL
#define SIO_GPIO_HI_IN      (SIO_BASE + 0x008)

#define IO_BANK0_BASE       0x40028000UL
#define IO_BANK0_CTRL(n)    (IO_BANK0_BASE + 0x004 + (n) * 8)
#define FUNCSEL_MASK        0x1fu
#define FUNCSEL_NULL        0x1fu

#define PADS_BANK0_BASE     0x40038000UL
#define PADS_BANK0_PAD(n)   (PADS_BANK0_BASE + 0x004 + (n) * 4)
/* IE | PDE | SCHMITT, ISO clear, output driver irrelevant (FUNCSEL NULL). */
#define PAD_INPUT_PULLDOWN  ((1u << 6) | (1u << 2) | (1u << 1))

static const char *stepping_name(unsigned rev) {
    switch (rev) {
    case 0x2: return "A2";
    case 0x3: return "A3";
    case 0x8: return "A4";
    default:  return "unknown";
    }
}

static unsigned hi_in(unsigned gpio) {
    return (REG(SIO_GPIO_HI_IN) >> (gpio - 32u)) & 1u;
}

void boardprobe(void) {
    uint32_t chip = REG(SYSINFO_CHIP_ID);
    unsigned rev = chip >> 28;
    unsigned part = (chip >> 12) & 0xffffu;
    unsigned qfn60 = REG(SYSINFO_PACKAGE_SEL) & 1u;

    cprintf("chip:    CHIP_ID=0x%08lx part=0x%04x stepping %s (REVISION 0x%x), package %s\n",
            (unsigned long)chip, part, stepping_name(rev), rev,
            qfn60 ? "QFN60 (RP2350A)" : "QFN80 (RP2350B)");
    cprintf("         RP2350-E9 (pull-down latch): %s\n",
            rev == 0x2 ? "PRESENT -- do not rely on pad pull-downs for USB detach"
                       : "fixed on this stepping");

    /* Before the PIO-USB engine (36.7) starts, the pads are left as this
     * configures them: FUNCSEL untouched (NULL at reset, so nothing drives the
     * lines), and pull-downs are what a USB host presents on D+/D- anyway.
     * Once the engine owns the port, its pads and its input inversion are its
     * own: read through them rather than rewrite them. */
    bool owned = (REG(IO_BANK0_CTRL(CONFIG_PIOUSB_DP_GPIO)) & FUNCSEL_MASK) != FUNCSEL_NULL;
    unsigned inv = (REG(IO_BANK0_CTRL(CONFIG_PIOUSB_DP_GPIO)) >> 16) & 1u;
    if (!owned) {
        REG(PADS_BANK0_PAD(CONFIG_PIOUSB_DP_GPIO)) = PAD_INPUT_PULLDOWN;
        REG(PADS_BANK0_PAD(CONFIG_PIOUSB_DM_GPIO)) = PAD_INPUT_PULLDOWN;
    }
    time_delay_us(1000);   /* let the lines settle through 27 R + cable */

    /* Several samples, not one: a device mid-attach (or a noisy cable) should
     * read as unstable, not as whatever one sample happened to catch. */
    unsigned dp_hi = 0, dm_hi = 0;
    const unsigned samples = 64;
    for (unsigned i = 0; i < samples; i++) {
        dp_hi += hi_in(CONFIG_PIOUSB_DP_GPIO) ^ inv;
        dm_hi += hi_in(CONFIG_PIOUSB_DM_GPIO) ^ inv;
        time_delay_us(50);
    }
    const char *verdict;
    if (dp_hi == samples && dm_hi == 0)      verdict = "full-speed device attached";
    else if (dm_hi == samples && dp_hi == 0) verdict = "low-speed device attached";
    else if (dp_hi == 0 && dm_hi == 0)       verdict = "nothing attached";
    else                                     verdict = "lines unstable -- recheck";
    cprintf("piousb:  GP%d(D+) high %u/%u, GP%d(D-) high %u/%u: %s\n",
            CONFIG_PIOUSB_DP_GPIO, dp_hi, samples,
            CONFIG_PIOUSB_DM_GPIO, dm_hi, samples, verdict);

    uint32_t gp0 = REG(IO_BANK0_CTRL(0));
    unsigned fsel = gp0 & FUNCSEL_MASK;
    cprintf("gp0:     GPIO0_CTRL=0x%08lx FUNCSEL=%u: %s\n", (unsigned long)gp0, fsel,
            fsel == FUNCSEL_NULL ? "untouched (PSRAM /CS left to its pull-up)"
                                 : "CLAIMED -- something drives the PSRAM chip select");
}

#endif /* CONFIG_BOARD_RP2350 && CONFIG_PIOUSB_DP_GPIO */
