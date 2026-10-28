/* K210 loader: the LugalOS image behind the BROM's framing at flash
 * offset 0, and the one piece a network update never rewrites.
 *
 * Flow, per boot:
 *
 *   1. The BROM reads [flag][len][this image][sha256] at flash offset 0
 *      and enters it at 0x80000000 — which is also the kernel's link
 *      address, so the first thing done here is to move out of the way:
 *      copy this image to the top of SRAM (below an untouched stack
 *      window at the very top) and continue running from the copy.
 *   2. Read the boot-record pair and choose a slot with the shared
 *      rules (kernel/ota_image.h verbatim): `active`, or slot A when no
 *      record is valid — raw flash, since a host-provisioned board
 *      always carries a record.
 *   3. Read the slot's 256-byte OTA header, check magic/board/CRC,
 *      then stream part 0 — the kernel's flat SRAM image — into
 *      0x80000000, never reaching into this image's window. There is
 *      no execute-in-place on this chip, so a "slot" is a load blob,
 *      not mapped code.
 *   4. Jump. Trial-boot marks and a WDT0-bound on first boot of a
 *      pending slot need one flash write the loader must own (an
 *      in-place bit clear, which NOR allows without an erase); the
 *      on-device update driver brings that half. Host provisioning
 *      never leaves a record pending, so the plain `active` path is
 *      the whole story for now.
 *
 * It links no libc — just ota_image.c for the record rules — and reads
 * the NOR through the private single-line SPI routine below, which
 * carries the same measured framing laws as drivers/spi_nor_k210.c:
 * answer beats begin after the command beats, and the RX FIFO must be
 * drained between transactions or a stale beat shifts the window. */

#include "kernel/ota_image.h"
#include <stdbool.h>
#include <stdint.h>

#if !defined(LUGALOS_K210_BOOTREC_BASE) || !defined(LUGALOS_K210_SLOT_A_BASE)
#error "the K210 flash map (cmake/flash_layout_k210.cmake) is a compile definition"
#endif

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define UARTHS              0x38000000UL
#define SYSCTL_CLK_EN_CENT  REG(0x50440000UL + 0x28)
#define SYSCTL_CLK_EN_PERI  REG(0x50440000UL + 0x2c)
#define CENT_APB2           (1u << 5)
#define PERI_SPI3           (1u << 9)      /* struct order in k210_sysctl.h: spi0=6 .. spi3=9 */
#define PERI_FPIOA          (1u << 20)
#define FPIOA_FN(p)         REG(0x502B0000UL + 4u * (p))
#define GPIO_DATA           REG(0x50200000UL + 0x00)
#define GPIO_OUTPUT_EN      REG(0x50200000UL + 0x04)
#define GPIO_SOURCE_EN      REG(0x50200000UL + 0x08)

#define SPI3                0x54000000UL
#define SPI_CTRLR0          REG(SPI3 + 0x00)
#define SPI_CTRLR1          REG(SPI3 + 0x04)
#define SPI_SSIENR          REG(SPI3 + 0x08)
#define SPI_SER             REG(SPI3 + 0x10)
#define SPI_BAUDR           REG(SPI3 + 0x14)
#define SPI_TXFLR           REG(SPI3 + 0x20)
#define SPI_RXFLR           REG(SPI3 + 0x24)
#define SPI_IMR             REG(SPI3 + 0x2c)
#define SPI_DR              REG(SPI3 + 0x60)

#define CMD_READ            0x03

/* The K210's 6 MiB SRAM and the BROM's flash-load destination. Both
 * the kernel and the loader link here; the loader relocates itself to
 * the top of this window before streaming the kernel over its own
 * original home. */
#define SRAM_BASE           0x80000000UL
#define SRAM_TOP            0x80600000UL
#define STACK_WINDOW        0x2000u

extern char _loader_start[], _loader_end[];

static const uint32_t g_slot_base[2] = { LUGALOS_K210_SLOT_A_BASE,
                                         LUGALOS_K210_SLOT_B_BASE };

/* Set before the self-copy; the relocated image reads it as nonzero.
 * Lives in .data, which travels with the copy. */
static uintptr_t g_run_base;

/* GCC emits this for the header struct copies; no libc is linked. */
void *memcpy(void *dst, const void *src, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void putc_(char c) {
    while (REG(UARTHS) & (1u << 31))
        ;
    REG(UARTHS) = (uint32_t)c;
}
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puth(uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    for (int sh = 28; sh >= 0; sh -= 4)
        putc_(hx[(v >> sh) & 0xFu]);
}

/* Everything here is the writer blob's silicon-verified sequence
 * (tools/k210_wblob.c), copied rather than shared only because the
 * loader links nothing but ota_image.c. Deviating from it re-opens
 * every failure the blob already paid for: XPI owning the pads,
 * 4-byte address mode latched, HOLD#/WP# floating, BAUDR=4 running
 * the wire at ~100 MHz where the part answers 0xFF forever. */
static void nor_spi_init(void) {
    /* BROM's XPI boot reads leave spi3_xip_en set; while it is set the
     * XPI path owns the NOR pads and SSIC masters nothing. */
    REG(0x50440000UL + 0x58) &= ~((1u << 15) | (1u << 12));
    REG(0x50440000UL + 0x3c) &= ~(0xFFu << 24);   /* spi3 clk threshold 0 */
    SYSCTL_CLK_EN_CENT |= CENT_APB2;
    SYSCTL_CLK_EN_PERI |= PERI_FPIOA | PERI_SPI3;
    /* Maix Bit 1.09 wires the NOR's HOLD# to pad IO3 and WP# to pad
     * IO23. Undriven they float and the part misbehaves at random. */
    const uint32_t nfl = (1u << 16) | (3u << 5) | (1u << 12);
    FPIOA_FN(3)  = (56u + 3)  | nfl;
    FPIOA_FN(23) = (56u + 23) | nfl;
    GPIO_OUTPUT_EN |= (1u << 3) | (1u << 23);
    GPIO_SOURCE_EN |= (1u << 3) | (1u << 23);
    GPIO_DATA      |= (1u << 3) | (1u << 23);

    SPI_SSIENR = 0;
    SPI_CTRLR0 = 0;                 /* mode 0, standard TMOD, 8-bit fixed */
    SPI_CTRLR1 = 0;
    SPI_BAUDR  = 64;                /* parent ~390 MHz -> SCLK ~6 MHz */
    SPI_IMR    = 0;
    SPI_SSIENR = 1;

    /* Reset the part, then pin its state: 66/99 recovers any mode a
     * previous owner left; E9 forces 3-byte address width; WREN+WRSR
     * 0x00 clears stale block protect (protected programs complete
     * WIP-clean while writing nothing). */
    static const uint8_t seq[6] = { 0xFF, 0x66, 0x99, 0xE9, 0x06, 0x01 };
    static const uint8_t wr[2]  = { 0x01, 0x00 };
    for (int i = 0; i < 4; i++) {
        uint8_t c = seq[i];
        while (SPI_RXFLR & 0x3Fu) (void)SPI_DR;
        SPI_SER = 1;
        SPI_DR = c;
        for (volatile uint32_t d = 0; d < 2000u; d++) ;
        SPI_SER = 0;
    }
    /* WREN then WRSR(0x00), one frame each like the blob */
    for (int frame = 0; frame < 2; frame++) {
        static const uint8_t flen[2] = { 1, 2 };
        const uint8_t *f = frame ? wr : &seq[4];
        uint32_t n = flen[frame];
        while (SPI_RXFLR & 0x3Fu) (void)SPI_DR;
        SPI_SER = 1;
        for (uint32_t i = 0; i < n; i++)
            while ((SPI_TXFLR & 0x3Fu) >= 16u) ;
        for (uint32_t i = 0; i < n; i++) SPI_DR = f[i];
        for (volatile uint32_t d = 0; d < 4000u; d++) ;
        SPI_SER = 0;
    }
    /* The part needs ~ms after WRSR/99h; the loader is not racing. */
    for (volatile uint32_t d = 0; d < 400000u; d++) ;
}

static bool nor_read(uint32_t addr, void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint32_t chunk = len > 256u ? 256u : len;
        uint8_t cmd[4] = { CMD_READ, (uint8_t)(addr >> 16),
                           (uint8_t)(addr >> 8), (uint8_t)addr };
        uint8_t ans[288];               /* headroom: beats beyond the
                                         * expected count are counted
                                         * but must stay in bounds */
        uint32_t rxgot = 0, sent = 0;
        /* No SSIENR re-toggle: the enable edge glitches the wire.
         * Pre-drain bounded: this SSIC's RX FIFO can refill from
         * underflow artifacts faster than we poll, so an unbounded
         * drain never terminates (hang observed at 256-byte reads);
         * proceed after a bounded swallow like the writer blob, whose
         * byte-exact verify proves phantoms stay tail-only. */
        for (int pd = 0; pd < 64 && (SPI_RXFLR & 0x3Fu); pd++)
            (void)SPI_DR;
        SPI_SER = 1;
        uint32_t spin = 0;
        /* Beat accounting mirrors the silicon-verified writer blob:
         * every RX beat counts toward the total; only the first
         * 4+chunk are stored. */
        while (rxgot < 4 + chunk && spin++ < 20000000u) {
            while (SPI_RXFLR & 0x3Fu) {
                uint8_t b = (uint8_t)SPI_DR;
                if (rxgot < sizeof ans) ans[rxgot] = b;
                rxgot++;
            }
            if (sent < 4 + chunk && (SPI_TXFLR & 0x3Fu) < 16u) {
                SPI_DR = (sent < 4) ? cmd[sent] : 0xFFu;
                sent++;
            }
        }
        while ((SPI_TXFLR & 0x3Fu) && spin-- < 40000000u)
            ;
        for (volatile uint32_t d = 0; d < 200u; d++) ;
        SPI_SER = 0;
#ifdef LOADER_TRACE
        if (rxgot < 4 + chunk) {
            puts_("[RXSTALL a"); puth(addr); puts_(" want ");
            puth(4 + chunk); puts_(" got "); puth(rxgot);
            puts_(" sent "); puth(sent); puts_("]\n");
        }
#endif
        if (rxgot < 4 + chunk) return false;
        memcpy(out, ans + 4, chunk);
        out += chunk; addr += chunk; len -= chunk;
    }
    return true;
}

/* Validate a slot header and load its blob. False leaves nothing
 * behind but the attempt. */
static bool load_slot(int slot, uint32_t load_max) {
    ota_image_header_t hdr;
    if (!nor_read(g_slot_base[slot], &hdr, sizeof hdr)) return false;
    if (!ota_header_valid(&hdr)) return false;
    if (hdr.part_len[0] == 0 || hdr.part_len[0] > load_max) return false;
    if (!nor_read(g_slot_base[slot] + OTA_HEADER_LEN,
                  (void *)SRAM_BASE, hdr.part_len[0])) return false;
    puts_("[loader] slot "); putc_((char)('A' + slot));
    puts_(" len "); puth(hdr.part_len[0]); puts_(" loaded\n");
#ifdef LOADER_TRACE
    {   /* checksum of what actually landed in SRAM: distinguishes a
         * corrupt copy (flash-read fault) from a corrupt jump. */
        const uint32_t *w = (const uint32_t *)SRAM_BASE;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < hdr.part_len[0] / 4u; i++) sum += w[i];
        puts_("[loader] sram sum "); puth(sum); puts_("\n");
        puts_("[loader] w0 "); puth(w[0]); puts_(" w1 "); puth(w[1]);
        puts_("\n");
    }
#endif
    return true;
}

void loader_main(void);

#ifdef LOADER_TRACE
/* Verbose mtvec: prints the trap frame straight to UARTHS. Installed
 * for the bring-up window; a quiet loader would otherwise hide faults
 * behind the sink loop. */
void loader_trap_report(void) {
    uint64_t cause, epc, tval;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc"   : "=r"(epc));
    __asm__ volatile("csrr %0, mtval"  : "=r"(tval));
    puts_("\n[TRAP cause "); puth((uint32_t)cause);
    puts_(" epc ");          puth((uint32_t)epc);
    puts_(" tval ");         puth((uint32_t)tval); puts_("]\n");
    for (;;) ;
}
#endif

void loader_main(void) {
#ifdef LOADER_TRACE
    __asm__ volatile("csrw mtvec, %0"
                     :: "r"((uintptr_t)&loader_trap_report | 1u));
    puts_("[L0]");
#endif
    if (g_run_base == 0) {
        /* Relocate to the top of SRAM, under the stack window. */
        unsigned long size = (unsigned long)(_loader_end - _loader_start);
        unsigned long room = (SRAM_TOP - STACK_WINDOW) - SRAM_BASE - size;
        char *dst = (char *)(((uintptr_t)(SRAM_TOP - STACK_WINDOW - size))
                             & ~(uintptr_t)15u);   /* aligned fetch base;
                                                    * an unaligned copy
                                                    * base faults the
                                                    * fetch path */
        (void)room;
        g_run_base = (uintptr_t)dst;
        memcpy(dst, _loader_start, size);
#ifdef LOADER_TRACE
        puts_("[R"); puth((uint32_t)(uintptr_t)dst); puts_("]");
#endif
        void (*relocated)(void) =
            (void (*)(void))((uintptr_t)loader_main -
                             (uintptr_t)_loader_start + (uintptr_t)dst);
        __asm__ volatile("fence.i");
        relocated();
        for (;;) ;                  /* not reached */
    }

#ifdef LOADER_TRACE
    puts_("[L1]");
#endif
    nor_spi_init();
#ifdef LOADER_TRACE
    puts_("[L2]");
#endif

    ota_bootrec_t rec_store[2];
    const ota_bootrec_t *recs[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        int ok = nor_read(LUGALOS_K210_BOOTREC_BASE + i * 0x1000u,
                          &rec_store[i], sizeof rec_store[i]);
        if (ok && ota_bootrec_valid(&rec_store[i], 2))
            recs[i] = &rec_store[i];
    }
    ota_boot_choice_t choice = ota_boot_choose(recs, 2);
    int slot = choice.slot >= 0 ? choice.slot : 0;

    unsigned long size = (unsigned long)(_loader_end - _loader_start);
    uint32_t load_max = (uint32_t)(SRAM_TOP - STACK_WINDOW - size - SRAM_BASE);

    if (!load_slot(slot, load_max) &&
        !(slot != 0 && load_slot(0, load_max))) {
        puts_("[loader] nothing bootable, halting\n");
        for (;;) ;
    }

#ifdef LOADER_NO_KERNEL
    puts_("[loader] halting before kernel\n");
    for (;;) ;
#endif
    void (*entry)(void) = (void (*)(void))SRAM_BASE;
    entry();
    for (;;) ;
}
