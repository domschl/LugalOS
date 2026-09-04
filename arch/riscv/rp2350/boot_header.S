/*
 * arch/riscv/rp2350/boot_header.S
 *
 * RP2350 BootROM Image Header, PICOBIN IMAGE_DEF, Clock Setup and Reset Handler
 * Adopted from working RP2350 bare-metal image (rp2350_cld).
 */

#include "kernel/meminfo.h"
#include "kernel/hart.h"

.equ BINARY_INFO_MARKER_START,    0x7188ebf2
.equ BINARY_INFO_MARKER_END,      0xe71aa390
.equ PICOBIN_BLOCK_MARKER_START,  0xffffded3
.equ PICOBIN_BLOCK_MARKER_END,    0xab123579

.equ BOOTROM_ENTRY_OFFSET,        0x7dfc
.equ SRAM_END,                    0x20082000

/* 1. Program Info Header */
.section .binary_info_header, "a"
binary_info_header:
    .word BINARY_INFO_MARKER_START
    .word __binary_info_start
    .word __binary_info_end
    .word address_mapping_table
    .word BINARY_INFO_MARKER_END

/* 2. PICOBIN EMBEDDED BLOCK (IMAGE_DEF) */
.section .embedded_block, "a"
embedded_block:
    .word PICOBIN_BLOCK_MARKER_START

    /* Item 1: Image Definition */
    .byte 0x42    /* Type: Image definition */
    .byte 0x01    /* Size: 1 word */
    .hword 0x1101 /* Flags: EXE | RISC-V | RP2350 */

    /* Item 2: Entry Point Definition */
    .byte 0x44    /* Type: Entry Point definition */
    .byte 0x03    /* Size: 3 words */
    .hword 0x00   /* 16-bit pad */
    .word _reset_handler  /* Initial PC address */
    .word SRAM_END        /* Initial SP address */

    /* Item 3: BLOCK_ITEM_LAST */
    .byte 0xff    /* Type: BLOCK_ITEM_LAST */
    .hword (embedded_block_end - embedded_block - 16) / 4
    .byte 0x00    /* 8-bit pad */

    /* Link (0 == self-contained block) */
    .word 0

    .word PICOBIN_BLOCK_MARKER_END
embedded_block_end:

/* 3. Address Mapping Table */
.section .rodata, "a"
.global address_mapping_table
address_mapping_table:
    .word __data_load_ptr
    .word __data_start
    .word __data_end
    .word __scratch_x_load_ptr
    .word __scratch_x_start
    .word __scratch_x_end
    .word __scratch_y_load_ptr
    .word __scratch_y_start
    .word __scratch_y_end
    .word 0       /* Null terminator */

/* 4. ELF Entry Point (Debugger Entry) */
.section .reset, "ax"
.global _entry_point
_entry_point:
    li t0, BOOTROM_ENTRY_OFFSET
    jr t0

/* 5. Reset Handler */
.section .text
.global _reset_handler
_reset_handler:
    /* Check mhartid: only core 0 executes boot init */
    csrr t0, mhartid
    beqz t0, .Lcore0_boot

    /* A secondary reaching _reset_handler at all leaves proof of it, then
     * parks. See linker/rp2350.ld's .smpmark.
     *
     * Measured 2026-09-04: this never fires. The comment above -- "RP2350
     * boots both cores" -- is wrong, and has been since it was written. Core
     * 1 comes out of reset inside the *bootrom*, waiting on the SIO FIFO for
     * the standard launch sequence, and never executes a byte of this image
     * until sent one. So this branch is dead code, kept only because a probe
     * that costs three instructions is worth more than a comment claiming
     * the same thing.
     *
     * Core 1 enters at core1_entry below instead, because the bootrom jumps
     * it straight there. */
    la t1, _smp_mark
    li t2, 0x51C0DE01
    sw t2, 0(t1)
    j 3f
.Lcore0_boot:

    /* Park the raw hart id in tp until .bss is cleared and
     * SETUP_HART_POINTER can turn it into this hart's record pointer (S0,
     * kernel/include/kernel/hart.h). This path stays in M-mode throughout,
     * so mhartid would still be readable later -- it is captured here anyway
     * so that both boot paths do the identical thing in the identical place,
     * which is the whole reason SETUP_HART_POINTER is a shared macro. */
    mv tp, t0

    /* Setup Machine trap vector */
    la t0, trap_vector_entry
    csrw mtvec, t0

    /* Take ownership of the boot stack.
     *
     * The bootrom starts us with sp at SRAM_END (0x20082000, the top of
     * SCRATCH_Y) -- see the IMAGE_DEF header above, which is where that value
     * comes from. linker/rp2350.ld allocates a 16 KB .stack in main RAM and
     * explains at length why the boot stack must not live in SCRATCH: it gets
     * 8 KB at most before it runs into the page allocator's heap at
     * 0x20080000, and overflowing there corrupts the heap instead of
     * faulting.
     *
     * That section was added, and nothing ever loaded sp from it. The kernel
     * has been running on the scratch stack the whole time, with 16 KB of RAM
     * reserved and untouched, and the linker script describing a layout that
     * was not in effect. Found when the boot-stack high-water instrumentation
     * (kernel/meminfo.c) read 100% usage on real silicon and 6 KB of 64 KB on
     * QEMU -- the two boot paths had diverged and only one of them was ever
     * looked at.
     *
     * Set before the copy and clear loops below so every `call` from here on
     * runs on the real stack. */
    la sp, _stack_top

    /* Load data/scratch sections from FLASH to RAM */
    la t0, address_mapping_table
1:
    lw a0, 0(t0)
    beqz a0, 2f
    lw a1, 4(t0)
    lw a2, 8(t0)
    call data_cpy
    addi t0, t0, 12
    j 1b
2:
    /* Zero out .bss section */
    la a0, __bss_start
    la a1, __bss_end
    call bss_fill

    /* The PMP-granted driver regions (linker/rp2350.ld's .ustacksN), which
     * live OUTSIDE __bss_start..__bss_end and so were never cleared by
     * anything.
     *
     * That was not a tidiness problem. drivers/usb_cdc.c keeps its entire
     * state -- ring indices, ep2_configured, ep2_dtr -- in g_usb_region,
     * which sits in .ustacks16384. On a cold boot those fields held whatever
     * SRAM happened to contain, so usb_cdc_putc()'s guard let a caller
     * through on garbage and then indexed its ring with an unmasked garbage
     * head: an out-of-bounds write into a PMP boundary, a fault, and no
     * handler installed that early to report it. The board simply stopped,
     * silently, in the middle of a printk.
     *
     * It survived every reflash because a BOOTSEL session runs the bootrom's
     * own mass-storage code through this SRAM first and leaves values that
     * happen to fail the guard -- which is exactly why it looked like "works
     * after a flash, dies on the next power cycle", and why it looked
     * host-dependent when it never was (2026-08-23).
     *
     * Zeroing here rather than in each driver's init: the memory belongs to
     * the region layout, not to whichever driver was asked to own it, and a
     * driver that forgets is a driver that fails this way again. */
    la a0, _ustacks_start
    la a1, _ustacks_end
    call bss_fill

    /* Paint the unused stack so its high-water mark can be recovered later
     * (kernel/meminfo.c, /proc/meminfo). After the clear, for the same reason
     * as the QEMU path: .bss and .stack are adjacent, so a clear that ran
     * past __bss_end would erase the paint rather than corrupt anything and
     * the bug would present as "the stack is never used". */
    PAINT_BOOT_STACK t0, t1, t2

    /* tp stops being the raw hart id and becomes this hart's record (S0,
     * kernel/include/kernel/hart.h). After the .bss clear above, because
     * g_harts lives in .bss. Before the calls below, because the trap vector
     * dereferences tp on every kernel trap from here on -- and mtvec has been
     * armed since the top of this function. */
    SETUP_HART_POINTER t0, t1

    /* Hardware clock and timer initialization */
    call init_xosc
    call init_pll
    call configure_clk_ref
    call configure_clk_sys
    call start_sys_timers

    /* Jump to kernel_main */
    call kernel_main

3:
    wfi
    j 3b

/* --- Core 1 entry (X3, plan/phase23_multicore_scheduling.md) ---
 *
 * The bootrom jumps here after core 0 completes the SIO-FIFO launch
 * sequence (see smp_launch_core1() in kernel/smp.c), with sp already set to
 * the value we sent and mtvec already loaded from the vector_table word.
 * Verified against the Pico SDK's multicore_launch_core1_raw() rather than
 * assumed -- on RISC-V that word is mtvec, where on Arm it is VTOR.
 *
 * Everything core 0 does before this point has already happened: .data
 * copied, .bss cleared, hardware clocks up. So unlike the QEMU secondary
 * path in entry.S, there is nothing to wait for -- being launched *is* the
 * release. */
/* Weak, because minimal_rp2350.elf -- the standalone hardware test built
 * from this same boot path -- does not link kernel/smp.c and has no
 * secondary_main() to call. It also never launches core 1, so the reference
 * is genuinely unreachable there rather than merely unresolved. */
.weak core1_probe_main
.global core1_entry
core1_entry:
    /* Proof of arrival, written before anything else can go wrong.
     *
     * If core 1 faults three instructions later, this word is still the
     * difference between "the bootrom never started it" and "it started and
     * died" -- and a fault here is silent, because nothing is set up yet to
     * report one. */
    la t0, _smp_mark
    li t1, 0x51C0DE02
    sw t1, 0(t0)

    /* Park the raw hart id where SETUP_HART_POINTER expects it. Read from
     * the CSR rather than assumed to be 1: MAX_HARTS is 2 today and the
     * macro indexes g_harts by whatever this says. */
    csrr tp, mhartid

    /* This core's own trap vector. The bootrom set mtvec from the launch
     * word already; setting it again from the symbol keeps the two boot
     * paths saying the same thing rather than depending on what was sent. */
    la t0, trap_vector_entry
    csrw mtvec, t0

    /* Interrupts off until secondary_main() decides otherwise, matching what
     * core 0 does at the top of its own boot. */
    csrw mstatus, zero

    SETUP_HART_POINTER t0, t1

    /* core1_probe_main(), not secondary_main(): X3 proves the launch first
     * and joins the scheduler second. See kernel/smp.c for why those are
     * deliberately separate milestones rather than one leap. */
    call core1_probe_main
    /* core1_probe_main() does not return. */
1:  wfi
    j 1b

data_cpy_loop:
    lw a3, (a0)
    sw a3, (a1)
    addi a0, a0, 4
    addi a1, a1, 4
data_cpy:
    bltu a1, a2, data_cpy_loop
    ret

bss_fill_loop:
    sw x0, (a0)
    addi a0, a0, 4
bss_fill:
    bltu a0, a1, bss_fill_loop
    ret

/* --- Clock and Timer Initialization Functions --- */
.equ XOSC_FREQ,       (12 * 1000 * 1000)
.equ XOSC_BASE,       0x40048000
.equ XOSC_CTRL,       XOSC_BASE + 0x00
.equ XOSC_STATUS,     XOSC_BASE + 0x04
.equ XOSC_STARTUP,    XOSC_BASE + 0x0c
.equ XOSC_COUNT,      XOSC_BASE + 0x10

init_xosc:
    li a1, XOSC_STARTUP
    lw a0, (a1)
    li a2, (~0x00003fff)
    and a0, a0, a2
    ori a0, a0, 469 /* 10 ms startup delay */
    sw a0, (a1)

    li a1, XOSC_CTRL
    lw a0, (a1)
    li a2, (~0x00ffffff)
    and a0, a0, a2
    li a2, 0xfabaa0
    or a0, a0, a2
    sw a0, (a1)

    li a0, XOSC_STATUS
1:
    lw a1, (a0)
    bexti a1, a1, 31 /* Extract STABLE bit */
    beqz a1, 1b
    ret

.equ ATOMIC_XOR,     0x1000
.equ ATOMIC_SET,     0x2000
.equ ATOMIC_CLEAR,   0x3000

.equ PLL_SYS_BASE,     0x40050000
.equ PLL_SYS_CS,       PLL_SYS_BASE + 0x00
.equ PLL_SYS_PWR,      PLL_SYS_BASE + 0x04
.equ PLL_SYS_FBDIV_INT, PLL_SYS_BASE + 0x08
.equ PLL_SYS_PRIM,     PLL_SYS_BASE + 0x0c

.equ RESETS_BASE,      0x40020000
.equ RESETS_RESET,     RESETS_BASE + 0x0
.equ RESETS_RESET_DONE, RESETS_BASE + 0x8

init_pll:
    /* Bring PLL_SYS out of reset */
    li a0, (1 << 14)
    li a1, RESETS_RESET + ATOMIC_CLEAR
    sw a0, (a1)
    li a1, RESETS_RESET_DONE
1:
    lw a2, (a1)
    and a2, a2, a0
    bne a2, a0, 1b

    /* Set FBDIV: VCO = 1500 MHz */
    li a1, PLL_SYS_FBDIV_INT
    lw a0, (a1)
    li a2, (~0x00000fff)
    and a0, a0, a2
    ori a0, a0, 125
    sw a0, (a1)

    /* Turn on PLL and PLL VCO */
    li a0, (1 << 5) | (1 << 0)
    li a1, PLL_SYS_PWR + ATOMIC_CLEAR
    sw a0, (a1)

    /* Wait for PLL to lock */
    li a0, PLL_SYS_CS
1:
    lw a1, (a0)
    bexti a1, a1, 31 /* Extract LOCK bit */
    beqz a1, 1b

    /* Set post dividers: 5 and 2 -> 150 MHz sys clock */
    li a1, PLL_SYS_PRIM
    lw a0, (a1)
    li a2, (~0x00077000)
    and a0, a0, a2
    li a2, (5 << 16) | (2 << 12)
    or a0, a0, a2
    sw a0, (a1)

    /* Turn on PLL post dividers */
    li a0, (1 << 3)
    li a1, PLL_SYS_PWR + ATOMIC_CLEAR
    sw a0, (a1)
    ret

.equ CLOCKS_BASE,             0x40010000
.equ CLOCKS_CLK_REF_CTRL,     CLOCKS_BASE + 0x30
.equ CLOCKS_CLK_REF_SELECTED, CLOCKS_BASE + 0x38
.equ CLOCKS_CLK_SYS_CTRL,     CLOCKS_BASE + 0x3c
.equ CLOCKS_CLK_SYS_SELECTED, CLOCKS_BASE + 0x44

configure_clk_ref:
    /* Select XOSC as source */
    li a1, CLOCKS_CLK_REF_CTRL
    lw a0, (a1)
    li a2, (~0x00000003)
    and a0, a0, a2
    ori a0, a0, 0x2
    sw a0, (a1)

    /* Wait for XOSC to be selected */
    li a0, CLOCKS_CLK_REF_SELECTED
    li a1, 0b0100
1:
    lw a2, (a0)
    andi a2, a2, 0x00f
    bne a2, a1, 1b
    ret

configure_clk_sys:
    /* Select AUX as source */
    li a0, 1
    li a1, CLOCKS_CLK_SYS_CTRL + ATOMIC_SET
    sw a0, (a1)

    /* Wait for AUX to be selected */
    li a0, CLOCKS_CLK_SYS_SELECTED
    li a1, 0b10
1:
    lw a2, (a0)
    andi a2, a2, 0x003
    bne a2, a1, 1b
    ret

.equ TICKS_BASE,           0x40108000
.equ TICKS_TIMER0_CTRL,    TICKS_BASE + 0x18
.equ TICKS_TIMER0_CYCLES,  TICKS_BASE + 0x1c
.equ TICKS_TIMER1_CTRL,    TICKS_BASE + 0x24
.equ TICKS_TIMER1_CYCLES,  TICKS_BASE + 0x28

start_sys_timers:
    /* Bring timers out of reset */
    li a0, (1 << 24) | (1 << 23)
    li a1, RESETS_RESET + ATOMIC_CLEAR
    sw a0, (a1)
    li a1, RESETS_RESET_DONE
1:
    lw a2, (a1)
    and a2, a2, a0
    bne a2, a0, 1b

    /* Set up the timer0 and timer1 tick generators: one 1 us tick per 12
     * cycles of the 12 MHz clk_ref selected above.
     *
     * CYCLES must be *written*, not OR-ed into whatever is already there.
     * This used to be `lw / ori 12 / sw`, and the bootrom does not hand the
     * block over at zero: it left bit 4 set, so `16 | 12` programmed 28
     * cycles per tick. TIMER0 then counted at 12/28 MHz = 428.6 kHz while
     * kernel/time.c called each count a microsecond, making every clock in
     * the system -- uptime, delays, the clock face, DCF-77 pulse widths --
     * run at 12/28 = 42.9% of real time. Found 2026-08-23 from DCF-77 second
     * pulses arriving every "429 ms" with "45/88 ms" widths instead of every
     * 1000 ms with 100/200 ms widths; kernel/ticker.c had already measured
     * the same 2.33x against this timer and written it off as the RISC-V
     * tick generator being fast, when in fact that one (a clean write of 12)
     * was the correct clock of the two.
     *
     * ENABLE is cleared first because CYCLES must not be changed while the
     * generator is running. */
    li a1, TICKS_TIMER0_CTRL
    sw zero, (a1)
    li a1, TICKS_TIMER0_CYCLES
    li a0, 12
    sw a0, (a1)
    li a1, TICKS_TIMER0_CTRL
    li a0, 1
    sw a0, (a1)

    li a1, TICKS_TIMER1_CTRL
    sw zero, (a1)
    li a1, TICKS_TIMER1_CYCLES
    li a0, 12
    sw a0, (a1)
    li a1, TICKS_TIMER1_CTRL
    li a0, 1
    sw a0, (a1)

    /* Wait for timer0's generator to actually run before returning: RUNNING
     * is synchronised to the source clock, and the first thing the kernel
     * does with this timer is measure things against it. */
    li a1, TICKS_TIMER0_CTRL
1:
    lw a0, (a1)
    andi a0, a0, 2
    beqz a0, 1b
    ret
