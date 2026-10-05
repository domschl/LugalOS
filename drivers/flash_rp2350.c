/*
 * LugalOS Driver: RP2350 internal flash writes -- I7b,
 * plan/phase21_identity_and_authentication.md §3.2.
 *
 * One operation, deliberately: erase a 4 KB sector and program 4 KB into it.
 * That is the whole shape of what the identity store needs
 * (IDSTORE_SIZE_BYTES is exactly one sector, and idstore_writer_commit()
 * writes all of it in a single call), and a general-purpose flash API would
 * be more surface than anything here uses.
 *
 * ## Why any of this is delicate
 *
 * The image executes from flash over XIP. Writing flash means turning XIP
 * off, which means that for the duration of the operation *every* instruction
 * fetch from 0x10000000-and-up faults or hangs. So:
 *
 *   - the routine doing it must already be in RAM (`.ramfunc`, copied at boot
 *     by _reset_handler's table walk -- see linker/rp2350.ld),
 *   - it must not call anything that is not (no printk, no memcpy from libc,
 *     no bootrom lookups -- those are resolved by the caller, in flash, and
 *     passed in),
 *   - and interrupts must be off, because this kernel preempts: the 100 Hz
 *     ticker alone would vector to a handler in flash.
 *
 * §10 of the plan names the failure mode for getting this wrong, and it is
 * worth repeating here: the board hangs, it does not report an error. There
 * is nothing left running to report with.
 *
 * ## What is taken from the SDK rather than from prose
 *
 * The call sequence, the ROM table codes and the erase parameters all come
 * from pico-sdk's src/rp2_common/hardware_flash/flash.c and
 * boot_bootrom_headers/include/boot/bootrom_constants.h. In particular the
 * RP2350 path needs flash_flush_cache for more than the cache -- it also
 * releases the CSn IO force.
 *
 * ## What the bootrom sequence breaks, and how it is put back (38.1)
 *
 * plan/phase38_psram.md §2 H3/H4 and plan/phase38_preliminaries.md §4,
 * measured on the RP2350-LCD-7. connect_internal_flash / flash_exit_xip /
 * flash_enter_cmd_xip reset **both** QMI windows to the ROM's own generic
 * read (serial 03h at CLKDIV 12) and flush the XIP cache:
 *
 *   - window 0 (flash) stayed slow until the next reset -- code ran ~46 %
 *     slower after a write. This file used to say that was the SDK's own
 *     behaviour; it is not. pico-sdk re-runs the XIP setup the bootrom left
 *     in boot RAM. This tree's boot leaves M0 in plain quad I/O read (0xEB,
 *     suffix 0x00, no continuous-read mode bits), so the chip needs no
 *     re-entry sequence and putting the registers back is the whole repair --
 *     which `xipcycle` checks against a timed read rather than assuming;
 *   - window 1 (PSRAM, when a board has it) is left reading 03h from a chip in
 *     QPI mode, i.e. garbage, and the flush **discards** dirty lines: 1008 of
 *     1024 words written through the cache were lost in the measurement.
 *
 * So the RAM routine cleans the cache by set/way first (as pico-sdk's
 * xip_cache_clean_all() does, RP2350-E11), saves every window register,
 * XIP_CTRL and the QSPI pads, and writes them all back after
 * flash_enter_cmd_xip. Cleaning costs ~2048 byte stores; on a board with no
 * PSRAM there is nothing dirty and it changes nothing.
 */

#include "lugalos_config.h"
#include "drivers/flash_rp2350.h"
#include "arch/rp2350_bootrom.h"
#include "arch/rp2350_clocks.h"
#include "kernel/printk.h"
#include "kernel/hart.h"
#include "kernel/console.h"
#include "kernel/time.h"

#include <stddef.h>

#if defined(CONFIG_BOARD_RP2350)

#define ROM_TABLE_CODE(c1, c2)  ((uint32_t)((c1) | ((c2) << 8)))

#define ROM_FUNC_CONNECT_INTERNAL_FLASH ROM_TABLE_CODE('I', 'F')
#define ROM_FUNC_FLASH_EXIT_XIP         ROM_TABLE_CODE('E', 'X')
#define ROM_FUNC_FLASH_RANGE_ERASE      ROM_TABLE_CODE('R', 'E')
#define ROM_FUNC_FLASH_RANGE_PROGRAM    ROM_TABLE_CODE('R', 'P')
#define ROM_FUNC_FLASH_FLUSH_CACHE      ROM_TABLE_CODE('F', 'C')
#define ROM_FUNC_FLASH_ENTER_CMD_XIP    ROM_TABLE_CODE('C', 'X')

/* pico-sdk hardware_flash/flash.h and flash.c. The block size and block erase
 * command are what the ROM may use to erase large ranges more efficiently;
 * for a single 4 KB range it falls back to 0x20 sector erases regardless, but
 * passing the SDK's own values keeps this call identical to the one the SDK
 * makes rather than a variant nobody has run. */
#define FLASH_SECTOR_SIZE     4096u
#define FLASH_BLOCK_SIZE      65536u
#define FLASH_BLOCK_ERASE_CMD 0xd8u

/* RP2350 datasheet / pico-sdk hardware_regs: qmi.h, xip.h, pads_qspi.h,
 * addressmap.h. The ten window registers are contiguous: M0 TIMING, RFMT,
 * RCMD, WFMT, WCMD at 0x0c..0x1c, then M1's at 0x20..0x30. */
#define REG(a)              (*(volatile uint32_t *)(uintptr_t)(a))
#define QMI_BASE            0x400d0000UL
#define QMI_M0_TIMING       (QMI_BASE + 0x0c)
#define QMI_M0_RFMT         (QMI_BASE + 0x10)
#define QMI_M0_RCMD         (QMI_BASE + 0x14)
#define QMI_M1_TIMING       (QMI_BASE + 0x20)
#define QMI_M1_RFMT         (QMI_BASE + 0x24)
#define QMI_M1_RCMD         (QMI_BASE + 0x28)
#define QMI_WINDOW_REGS     10u
#define XIP_CTRL            0x400c8000UL
#define PADS_QSPI_IO        (0x40040000UL + 0x04)   /* SCLK, SD0..SD3, SS */
#define PADS_QSPI_IOS       6u
#define XIP_MAINT_BASE      0x18000000UL
#define XIP_SPACE_END       0x04000000UL            /* 64 MB of XIP address space */
#define XIP_CACHE_BYTES     16384u
#define XIP_CACHE_LINE      8u
#define SRAM_BASE           0x20000000UL
#define SRAM_END            0x20082000UL
#define FLASH_UNCACHED      0x14000000UL

typedef struct {
    uint32_t qmi[QMI_WINDOW_REGS];
    uint32_t xip_ctrl;
    uint32_t pads[PADS_QSPI_IOS];
} qspi_state_t;

__attribute__((section(".ramfunc")))
static void qspi_save(qspi_state_t *s) {
    for (unsigned i = 0; i < QMI_WINDOW_REGS; i++) s->qmi[i] = REG(QMI_M0_TIMING + 4u * i);
    s->xip_ctrl = REG(XIP_CTRL);
    for (unsigned i = 0; i < PADS_QSPI_IOS; i++) s->pads[i] = REG(PADS_QSPI_IO + 4u * i);
}

__attribute__((section(".ramfunc")))
static void qspi_restore(const qspi_state_t *s) {
    for (unsigned i = 0; i < PADS_QSPI_IOS; i++) REG(PADS_QSPI_IO + 4u * i) = s->pads[i];
    for (unsigned i = 0; i < QMI_WINDOW_REGS; i++) REG(QMI_M0_TIMING + 4u * i) = s->qmi[i];
    REG(XIP_CTRL) = s->xip_ctrl;
}

/* Converting the ROM's data pointer to a function pointer is what this
 * interface requires and ISO C has no conforming way to express it -- the
 * same suppression, for the same reason, as arch/riscv/rp2350/bootrom.c.
 * Scoped to this file so it cannot mask the warning where it would mean
 * something. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

typedef void (*rom_void_fn)(void);
typedef void (*rom_flash_range_erase_fn)(uint32_t addr, uint32_t count,
                                         uint32_t block_size, uint8_t block_cmd);
typedef void (*rom_flash_range_program_fn)(uint32_t addr, const uint8_t *data,
                                           uint32_t count);

/* Resolved in flash-resident code, before XIP goes away, and handed to the
 * RAM-resident routine as data. */
typedef struct {
    rom_void_fn                connect_internal_flash;
    rom_void_fn                flash_exit_xip;
    rom_flash_range_erase_fn   flash_range_erase;
    rom_flash_range_program_fn flash_range_program;
    rom_void_fn                flash_flush_cache;
    rom_void_fn                flash_enter_cmd_xip;
} flash_rom_fns_t;

/* The critical section itself. Everything it touches is either in RAM (its
 * own code, the caller's buffer, the resolved pointers, the saved registers
 * on this task's SRAM stack) or in the bootrom; nothing is in flash or PSRAM.
 * `noinline` so it cannot be inlined back into a flash-resident caller, which
 * would silently undo the whole point of the section attribute.
 *
 * `data` NULL runs the sequence without erasing or programming (`xipcycle`).
 * `rom_left`, when given, receives the registers as the bootrom left them,
 * before the restore -- the evidence that the restore is doing something. */
__attribute__((section(".ramfunc"), noinline))
static void flash_rom_sequence_ram(const flash_rom_fns_t *f,
                                   uint32_t flash_offs,
                                   const uint8_t *data,
                                   qspi_state_t *rom_left) {
    qspi_state_t saved;
    qspi_save(&saved);

    /* Write back every dirty line before flash_flush_cache() discards them:
     * clean by set/way over the top 16 KB of the XIP space, which the
     * RP2350-E11 workaround requires instead of by address. */
    for (uintptr_t a = XIP_SPACE_END - XIP_CACHE_BYTES; a < XIP_SPACE_END; a += XIP_CACHE_LINE) {
        *(volatile uint8_t *)(XIP_MAINT_BASE + a + 1u) = 0;
    }
    __asm__ __volatile__("fence" ::: "memory");

    f->connect_internal_flash();
    f->flash_exit_xip();
    if (data) {
        f->flash_range_erase(flash_offs, FLASH_SECTOR_SIZE,
                             FLASH_BLOCK_SIZE, FLASH_BLOCK_ERASE_CMD);
        f->flash_range_program(flash_offs, data, FLASH_SECTOR_SIZE);
    }
    /* Not only a cache flush: this is also what releases the CSn IO force
     * that flash_exit_xip() applied. Skipping it leaves the bus held. */
    f->flash_flush_cache();
    f->flash_enter_cmd_xip();

    if (rom_left) qspi_save(rom_left);
    qspi_restore(&saved);
    __asm__ __volatile__("fence" ::: "memory");
}

static int flash_rom_op(uint32_t flash_offs, const uint8_t *data, qspi_state_t *rom_left) {
    flash_rom_fns_t f;
    f.connect_internal_flash = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    f.flash_exit_xip         = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_EXIT_XIP);
    f.flash_range_erase      = (rom_flash_range_erase_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_RANGE_ERASE);
    f.flash_range_program    = (rom_flash_range_program_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_RANGE_PROGRAM);
    f.flash_flush_cache      = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_FLUSH_CACHE);
    f.flash_enter_cmd_xip    = (rom_void_fn)rp2350_rom_func_lookup(ROM_FUNC_FLASH_ENTER_CMD_XIP);

    /* Checked as a group and *before* anything is disturbed. A missing ROM
     * function discovered halfway through would leave XIP off with no way to
     * fetch the instruction that would report it. */
    if (!f.connect_internal_flash || !f.flash_exit_xip || !f.flash_range_erase ||
        !f.flash_range_program || !f.flash_flush_cache || !f.flash_enter_cmd_xip) {
        printk("[Flash] bootrom flash functions not found -- not writing\n");
        return -1;
    }

    /* X7: get the *other* core out of the XIP window first.
     *
     * Masking interrupts below protects this core. It does nothing for core
     * 1, which -- once it joins the scheduler (phase 23 X7) -- is executing
     * flash-resident code at this instant, and would fetch into a window
     * that is about to stop answering. The failure is a hung board with
     * nothing left running to report it, which is why this is a handshake
     * with an acknowledgement rather than a delay.
     *
     * Refused, not risked, on timeout. A write that does not happen is an
     * error a caller can report; a write that proceeds into a live core 1
     * is a board that stops mid-erase. No-ops when core 1 is not running,
     * and on every non-RP2350 target. */
    if (!smp_flash_park_request()) {
        printk("[Flash] core 1 did not park; refusing to turn XIP off\n");
        return -1;
    }

    /* Interrupts off across the whole operation, restored exactly as found.
     * csrrci returns the previous mstatus, so a caller that already had them
     * disabled stays that way. */
    uintptr_t saved;
    __asm__ __volatile__("csrrci %0, mstatus, 0x8" : "=r"(saved));

    flash_rom_sequence_ram(&f, flash_offs, data, rom_left);

    if (saved & 0x8) {
        __asm__ __volatile__("csrsi mstatus, 0x8");
    }

    smp_flash_park_release();
    return 0;
}

int flash_rp2350_write_sector(uint32_t flash_offs, const void *data) {
    if (flash_offs % FLASH_SECTOR_SIZE != 0) {
        printk("[Flash] refusing unaligned sector write at offset 0x%x\n", flash_offs);
        return -1;
    }
    if (!data) return -1;
    /* H2: the bootrom reads the source while XIP -- flash *and* PSRAM -- is
     * off. A buffer anywhere but SRAM is read as garbage, or hangs. */
    uintptr_t src = (uintptr_t)data;
    if (src < SRAM_BASE || src > SRAM_END - FLASH_SECTOR_SIZE) {
        printk("[Flash] refusing a source buffer outside SRAM (0x%08lx)\n", (unsigned long)src);
        return -1;
    }
    return flash_rom_op(flash_offs, (const uint8_t *)data, NULL);
}

static const char *read_cmd_name(uint32_t rfmt, uint32_t rcmd) {
    unsigned data_width = (rfmt >> 8) & 3u;     /* 0 single, 1 dual, 2 quad */
    unsigned cmd = rcmd & 0xffu;
    if (cmd == 0x03 && data_width == 0) return "serial 03h -- the bootrom's slow generic read";
    if (cmd == 0xeb && data_width == 2) return "quad I/O EBh";
    return data_width == 2 ? "quad" : data_width == 1 ? "dual" : "serial";
}

static void window_line(const char *name, uint32_t timing, uint32_t rfmt, uint32_t rcmd) {
    unsigned div = timing & 0xffu;
    if (div == 0) div = 256;
    cprintf("%s TIMING 0x%08lx RFMT 0x%08lx RCMD 0x%08lx: SCK clk_sys/%u = %lu kHz, %s\n",
            name, (unsigned long)timing, (unsigned long)rfmt, (unsigned long)rcmd, div,
            (unsigned long)(CONFIG_CLK_SYS_HZ / div / 1000u), read_cmd_name(rfmt, rcmd));
}

void flash_rp2350_qmi_report(void) {
    window_line("QMI M0 (flash):", REG(QMI_M0_TIMING), REG(QMI_M0_RFMT), REG(QMI_M0_RCMD));
    window_line("QMI M1 (CS1):  ", REG(QMI_M1_TIMING), REG(QMI_M1_RFMT), REG(QMI_M1_RCMD));
}

/* 64 KB of the image through the uncached alias, so every word is a real
 * QSPI transfer at whatever M0 says.
 *
 * The fastest of three passes (2026-10-05): one pass is 3.3 ms of a 10 ms
 * tick, and on the clock persona a ~0.7 ms preemption landed inside it a
 * third of the time -- on whichever side it hit, "after" read 20 % slower
 * than "before" and the verdict said NOT RESTORED with all 17 registers
 * identical (6 of 20 runs, the same on the firmware before phase31 §7).
 * A preemption only ever adds time, and a mode left slow is slow on every
 * pass, so the minimum measures the flash and not the scheduler. */
static uint32_t timed_flash_read_us(void) {
    const volatile uint32_t *p = (const volatile uint32_t *)FLASH_UNCACHED;
    uint32_t best = UINT32_MAX;
    for (int pass = 0; pass < 3; pass++) {
        uint32_t sum = 0;
        uint64_t t0 = time_get_us();
        for (uint32_t i = 0; i < 16384u; i++) sum += p[i];
        uint64_t t1 = time_get_us();
        __asm__ __volatile__("" :: "r"(sum));
        if ((uint32_t)(t1 - t0) < best) best = (uint32_t)(t1 - t0);
    }
    return best;
}

int flash_rp2350_xip_cycle(void) {
    return flash_rom_op(0, NULL, NULL);
}

void flash_rp2350_selftest(void) {
    qspi_state_t before, rom_left, after;
    qspi_save(&before);
    uint32_t us_before = timed_flash_read_us();

    if (flash_rom_op(0, NULL, &rom_left) != 0) {
        cprintf("xipcycle: the sequence did not run\n");
        return;
    }
    qspi_save(&after);
    uint32_t us_after = timed_flash_read_us();

    unsigned differ = 0;
    for (unsigned i = 0; i < QMI_WINDOW_REGS; i++) differ += before.qmi[i] != after.qmi[i];
    for (unsigned i = 0; i < PADS_QSPI_IOS; i++) differ += before.pads[i] != after.pads[i];
    differ += before.xip_ctrl != after.xip_ctrl;

    cprintf("xipcycle: bootrom XIP exit and re-entry ran (nothing erased or written)\n");
    cprintf("  as the bootrom left them:\n  ");
    window_line("M0", rom_left.qmi[0], rom_left.qmi[1], rom_left.qmi[2]);
    cprintf("  ");
    window_line("M1", rom_left.qmi[5], rom_left.qmi[6], rom_left.qmi[7]);
    cprintf("  after the restore:\n  ");
    window_line("M0", after.qmi[0], after.qmi[1], after.qmi[2]);
    cprintf("  ");
    window_line("M1", after.qmi[5], after.qmi[6], after.qmi[7]);
    cprintf("xipcycle: %u of %u QSPI registers differ from before; "
            "64 KB uncached flash read %lu us before, %lu us after -- %s\n",
            differ, (unsigned)(QMI_WINDOW_REGS + PADS_QSPI_IOS + 1),
            (unsigned long)us_before, (unsigned long)us_after,
            differ == 0 && us_after <= us_before + us_before / 10 ? "RESTORED" : "NOT RESTORED");
}

#pragma GCC diagnostic pop

#else

int flash_rp2350_write_sector(uint32_t flash_offs, const void *data) {
    (void)flash_offs; (void)data;
    return -1; /* no internal flash to write on the QEMU targets */
}

void flash_rp2350_selftest(void) {}
int flash_rp2350_xip_cycle(void) { return -1; }
void flash_rp2350_qmi_report(void) {}

#endif
