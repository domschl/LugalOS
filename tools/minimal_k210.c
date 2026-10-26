/* K210 silicon truth probe (65.1).
 *
 * Standalone M-mode blob, ISP-loaded into SRAM (no kernel, no flash
 * write). Settles, by measurement, the facts plan/phase65_k210_support.md
 * could only read out of the vendor SDK: misa, privilege levels, PMP,
 * atomics, F/D, mcycle, the CLINT (mtime rate, msip, mtimecmp), the
 * PLIC, the BROM's clock claim, hart 1 behaviour, the SPI-NOR JEDEC
 * ID -- and the core's legacy trap contract (see entry.S).
 *
 * Everything risky runs inside a guarded section: the trap handler
 * throws back to the caller of guarded() with the mcause; an
 * unexpected trap with no guard armed sets g_trap_unexpected.
 *
 * Built -march=rv64imafdc_zicsr_zifencei -mabi=lp64 (soft ABI): the
 * float experiments exist only as explicit asm; C code never sees a
 * floating type, keeping the AGENTS.md no-float rule testable from the
 * ELF itself.
 */

#include <stdint.h>
#include <stdbool.h>

/* addresses: platform.h / clint.h / plic.h / spi.h of the vendor SDK */
#define UARTHS        ((volatile uint32_t *)0x38000000UL)
#define CLINT_MSIP0     0x02000000UL
#define CLINT_MTIMECMP  0x02004000UL
#define CLINT_MTIME     0x0200BFF8UL
#define PLIC_BASE       0x0C000000UL
#define SPI0_BASE       0x52000000UL
#define SYSCTL_BASE     0x50440000UL

#define HART1_MAGIC_ADDR 0x80400000UL

#define UART_TX_FULL   0x80000000UL
#define UART_RX_EMPTY  0x80000000UL

/* ------------------------------------------------------------------ */
/* console                                                             */
/* ------------------------------------------------------------------ */
static void putc_(char c)
{
    while (UARTHS[0] & UART_TX_FULL)
        ;
    UARTHS[0] = (uint32_t)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void puth(uint64_t v, int width)
{
    for (int i = (width / 4) - 1; i >= 0; i--) {
        uint8_t n = (uint8_t)((v >> (i * 4)) & 0xf);
        putc_(n < 10 ? '0' + (char)n : 'a' + (char)(n - 10));
    }
}

static void putd(uint64_t u)
{
    char buf[24];
    int i = 0;
    do {
        buf[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    while (i--)
        putc_(buf[i]);
}

/* ------------------------------------------------------------------ */
/* trap plumbing (frame layout matches minimal_k210_entry.S)           */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t regs[32];  /* indexed by register number; a0 = regs[10] */
    uint64_t mcause;
    uint64_t mepc;
} trap_frame_t;

typedef struct { uint64_t ra, sp, s0, s1; } jmpb_t;

volatile uint64_t g_try_buf;      /* armed jmpb *, or 0               */
volatile uint64_t g_umode_arm;    /* 1: next trap exits into U-mode   */
volatile uint64_t g_u_reached;
static void u_target(void);
volatile uint64_t g_last_cause;
volatile int g_trap_unexpected;
volatile uint64_t g_msip_seen, g_timer_seen, g_irq_unexpected, g_hart1_irq;
uint64_t g_trap_ticks;

int try_begin(jmpb_t *buf);       /* naked asm; 0 on entry, 1 on throw */
void try_disarm(void);
void mret_to_umode(void *target);

/* Run fn() under trap guard; returns 0 or the synchronous mcause. */
static uint64_t guarded(void (*fn)(void))
{
    jmpb_t buf;
    if (try_begin(&buf) == 0) {
        fn();
        try_disarm();
        return 0;
    }
    uint64_t c = g_last_cause;
    g_last_cause = 0;
    return c;
}

void trap_c(trap_frame_t *f)
{
    uint64_t cause = f->mcause;
    if (cause & (1ULL << 63)) {
        uint64_t irq = cause & ~(1ULL << 63);
        if (irq == 3) {                          /* CLINT software int */
            uint64_t hid;
            __asm__ volatile("csrr %0, mhartid" : "=r"(hid));
            *(volatile uint32_t *)(CLINT_MSIP0 + 4 * hid) = 0;
            if (hid)
                g_hart1_irq = 1;
            else
                g_msip_seen = 1;
        } else if (irq == 7) {                   /* CLINT timer        */
            *(volatile uint64_t *)CLINT_MTIMECMP =
                *(volatile uint64_t *)CLINT_MTIME + 100000;
            g_timer_seen++;
        } else {
            g_irq_unexpected++;
        }
        return;                                  /* resume same pc     */
    }
    if (g_umode_arm) {
        /* exit THIS trap into u_target with MPP=U: the only way onto
         * U on this core (mret outside trap state is a no-op) */
        g_umode_arm = 0;
        f->mepc = (uint64_t)u_target;
        __asm__ volatile("li t0, 0x1800\ncsrc mstatus, t0");
        return;
    }
    if (g_try_buf) {                             /* throw to guarded() */
        uint64_t *b = (uint64_t *)g_try_buf;
        g_last_cause = cause;
        g_try_buf = 0;
        f->mepc = b[0];
        f->regs[10] = 1;                         /* a0 = 1 on throw    */
        return;
    }
    g_trap_unexpected++;
    if (g_trap_unexpected < 10) {   /* bounded telemetry of the storm */
        puts_("\nUT cause ");
        puth(cause, 16);
        puts_(" epc ");
        puth(f->mepc, 16);
        puts_(" mstatus ");
        uint64_t ms;
        __asm__ volatile("csrr %0, mstatus" : "=l"(ms));
        puth(ms, 16);
        puts_("\n");
    }
    /* resume at the next instruction: trapping sync faults do not
     * clear by themselves, and retrying the same pc would storm */
    f->mepc += 4;
}

/* ------------------------------------------------------------------ */
/* guarded operations (each: one risky thing, result in `r`)           */
/* ------------------------------------------------------------------ */
static volatile uint64_t r;

static void op_misa(void)     { __asm__ volatile("csrr %0, misa"     : "=l"(r)); }
static void op_mvendorid(void){ __asm__ volatile("csrr %0, mvendorid": "=l"(r)); }
static void op_marchid(void)  { __asm__ volatile("csrr %0, marchid"  : "=l"(r)); }
static void op_mimpid(void)   { __asm__ volatile("csrr %0, mimpid"   : "=l"(r)); }
static void op_mcycle(void)   { __asm__ volatile("csrr %0, mcycle"   : "=l"(r)); }

static uint64_t v_medeleg, v_mideleg;
static void op_medeleg(void)  { __asm__ volatile("csrrs %0, 0x302, x0" : "=l"(v_medeleg)); }
static void op_mideleg(void)  { __asm__ volatile("csrrs %0, 0x304, x0" : "=l"(v_mideleg)); }

static void op_ecall(void)
{
    __asm__ volatile("ecall");
}


volatile uint64_t g_skip_landed;
static void op_ecall_skip4(void)
{
    /* trap_c's default unexpected path already does mepc += 4 and
     * mrets, but that would report unexpected; instead arm try with a
     * dedicated handler: use g_try_buf path? simpler: let the guard
     * catch the ecall and check landing separately via a marker: the
     * ecall below traps; trap_c throws to guarded(); g_skip_landed
     * stays 0. So instead: DON'T guard; rely on handler's +=4:
     * after this ecall executes, the handler's mret must resume at
     * the instruction AFTER the ecall. */
    g_skip_landed = 0;
    __asm__ volatile("ecall");
    g_skip_landed = 1;
}

static void op_pmpcfg_write(void)
{
    __asm__ volatile("li t0, 0xff\ncsrw pmpcfg0, t0");
}
static void op_pmpaddr_write(void)
{
    __asm__ volatile("li t0, 0x5a5a5a5a5a5a5a5a\ncsrw pmpaddr0, t0");
}
static uint64_t pmp_read_cfg0(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, pmpcfg0" : "=l"(v));
    return v;
}
static uint64_t pmp_read_addr0(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, pmpaddr0" : "=l"(v));
    return v;
}

static uint64_t atomic_mem;

static void op_amoswap(void)
{
    uint64_t old;
    __asm__ volatile("amoswap.d %0, %1, (%2)"
                     : "=r"(old) : "r"(0x11ULL), "r"(&atomic_mem) : "memory");
    r = old;
}
static void op_amoadd(void)
{
    atomic_mem = 40;
    uint64_t old;
    __asm__ volatile("amoadd.d %0, %1, (%2)"
                     : "=r"(old) : "r"(2ULL), "r"(&atomic_mem) : "memory");
    r = old + atomic_mem;                     /* expect 40 + 42 = 82 */
}
static void op_lrsc(void)
{
    uint64_t v, s;
    atomic_mem = 7;
    __asm__ volatile("1: lr.d %0, (%2)\n\t"
                     "add %1, %0, 1\n\t"
                     "sc.d %1, %1, (%2)\n\t"
                     "bnez %1, 1b"
                     : "=&r"(v), "=&r"(s) : "r"(&atomic_mem) : "memory");
    r = ((uint64_t)s << 1) | (uint64_t)(atomic_mem == 8);
}

/* float experiments: explicit asm only; FS must be non-zero first. */
static volatile uint32_t fmem;
static void op_fsingle(void)
{
    uint64_t v;
    /* store path (fsw) as well as fmv.x: distinguishes a dead f-pr
     * file from a broken fmv bridge; delay chain flushes pipelines */
    __asm__ volatile(
        "li t0, 0x3f800000\n"
        "fmv.w.x ft0, t0\n"
        "fadd.s ft1, ft0, ft0\n"
        "fadd.s ft1, ft1, ft0\n"
        "fadd.s ft1, ft1, ft0\n"
        "fsw ft1, 0(%1)\n"
        "fence\n"
        :
        : "r"(&fmem), "r"(&fmem)
        : "t0", "ft0", "ft1", "memory");
    v = 0;
    __asm__ volatile("fadd.s ft1, ft0, ft0\nfmv.x.w %0, ft1" : "=r"(v));
    r = ((uint64_t)fmem << 32) | (v & 0xffffffffULL);
}
static void op_fdouble(void)
{
    uint64_t v;
    __asm__ volatile("li t0, 0x3ff00000\nslli t0, t0, 32\nfmv.d.x ft0, t0\nfmv.x.d %0, ft0"
                     : "=r"(v) :: "t0");
    r = v;
}

/* the U-mode experiment, run under guard: u_target records arrival,
 * reads mvendorid (illegal in U, fine in M) and ecalls back; the
 * recorded cause tells in which mode the target actually ran.
 */
static void u_target(void)
{
    g_u_reached = 1;
    __asm__ volatile("csrr t0, mvendorid" ::: "t0");  /* illegal in U */
    __asm__ volatile("ecall");                        /* 8 in U, 11 in M */
    for (;;)
        ;
}

static void op_umode(void)
{
    /* enter U via the trap-exit path (trap_c arms MPP=U + retargets);
     * whatever u_target does next traps back into the guard */
    g_umode_arm = 1;
    __asm__ volatile("ecall");
    g_umode_arm = 0;   /* never reached unless the arm path skipped */
}

/* ------------------------------------------------------------------ */
/* CLINT helpers                                                       */
/* ------------------------------------------------------------------ */
static uint64_t mtime_read(void)
{
    return *(volatile uint64_t *)CLINT_MTIME;
}

/* ------------------------------------------------------------------ */
/* SPI0 (DesignWare SSI) — JEDEC ID 0x9F                               */
/* ------------------------------------------------------------------ */
static volatile uint32_t *const spi0 = (volatile uint32_t *)SPI0_BASE;
static volatile uint32_t *const spi1 = (volatile uint32_t *)0x53000000UL;
#define SPI_CTRLR0 0x00
#define SPI_CTRLR1 0x04
#define SPI_SSIENR 0x08
#define SPI_SER    0x10
#define SPI_BAUDR  0x14
#define SPI_RXFLR  0x24
#define SPI_SR     0x28
#define SPI_DR     0x60

static uint64_t spi_idr(void)      { return spi0[0x58 / 4]; }
static uint64_t spi_ssic_ver(void) { return spi0[0x5c / 4]; }

/* FPIOA: SPI1 is pad-muxed; until the flash pins carry the SPI1
 * functions, the controller sees a floating bus (measured: MISO 0xffff
 * with the BROM pad state). Maix Bit schematic 1.09: NOR on GPIO 27..32.
 * Function numbers from the vendor fpioa table. */
#define FPIOA_BASE 0x400A0000UL
static void fpioa_fn(uint32_t pin, uint32_t fn)
{
    *(volatile uint32_t *)(FPIOA_BASE + 4 * pin) = fn | 0x100 /* force */;
}
static void fpioa_flash_pins(void)
{
    /* Schematic 1.09: the GD25LQ hangs off SPI0 (not SPI1!):
     * SPI0_SCLK IO_27, MOSI IO_28, CS0 IO_29, MISO IO_30, IO2/IO3 31/32 */
    fpioa_fn(27, 17);   /* SPI0_SCLK */
    fpioa_fn(28, 4);    /* SPI0_D0   */
    fpioa_fn(30, 5);    /* SPI0_D1   */
    fpioa_fn(31, 6);    /* SPI0_D2   */
    fpioa_fn(32, 7);    /* SPI0_D3   */
}

/* Trans/recv mode: ctrlr1+1 = 4 receive beats, the one transmit beat
 * carrying the command; the first received byte is MISO during the
 * command, the remaining three are the ID. Slow divider: SPI0's parent
 * frequency is unknown at this point. */
/* software CS on pin 30 (GPIO function 0), vendor spi_nor-demo style:
 * one trans-only frame for the command, one recv-only frame for the
 * answer, CS held low across both by GPIO. The SSI's own CS cannot
 * bridge two frames on this core. */
#define GPIOHS_BASE 0x1A000000UL   /* same layout; HS pads = pins 24..44 */
static volatile uint32_t *const gpio_dir  = (volatile uint32_t *)(GPIOHS_BASE + 0x04);
static volatile uint32_t *const gpio_out  = (volatile uint32_t *)(GPIOHS_BASE + 0x00);
static volatile uint32_t *const gpio_in   = (volatile uint32_t *)(GPIOHS_BASE + 0x50);
static void cs_init(void) { fpioa_fn(29, 0); *gpio_out |= (1u << 29); *gpio_dir |= (1u << 29); }
static void cs_low(void)  { *gpio_out &= ~(1u << 29); }
static void cs_high(void) { *gpio_out |= (1u << 29); }

static void report_sr(uint64_t v) { puth(v, 16); puts_("\n"); }
static uint64_t jedec_try(void)
{
    /* SPI0 ctrlr0: TMOD latches at [10:8] but DFS reads zero —
     * controller fixed at 8-bit frames; ndf unproven. Single CS
     * window (GPIO), 5-beat transceive: [0x9f ff ff ff ff]; the JEDEC
     * ID is beats 1..3. Raw beats printed for the record. */
    volatile uint32_t *const sysctl = (volatile uint32_t *)SYSCTL_BASE;
    sysctl[0x28 / 4] |= (1u << 5);                 /* apb2_clk_en          */
    sysctl[0x2c / 4] |= (1u << 5) | (1u << 6);     /* gpio_clk_en, spi0    */
    volatile uint32_t *const spix = spi0;
    cs_init();
    spix[SPI_SSIENR / 4] = 0;
    spix[SPI_CTRLR0 / 4] = (1u << 8);              /* tmod = tx+rx(0)? no: */
    spix[SPI_CTRLR0 / 4] = 0;                      /* tmod 00: trans+recv  */
    spix[SPI_CTRLR1 / 4] = 0;
    spix[SPI_BAUDR / 4] = 128;
    report_sr(spix[SPI_SSIENR / 4]);               /* enables? 0=expect    */
    spix[SPI_SSIENR / 4] = 1;
    report_sr(spix[SPI_SSIENR / 4]);               /* =expect 1             */
    while (spix[SPI_SR / 4] & 0x2)
        ;
    cs_low();
    for (int i = 0; i < 5; i++)
        spix[SPI_DR / 4] = i ? 0xffu : 0x9fu;
    for (uint32_t n = 0; n < 3000000u && (spix[SPI_SR / 4] & 0x2); n++)
        ;
    uint32_t got = spix[SPI_RXFLR / 4];
    puts_("RPT spi_got ");
    report_sr(got);
    uint64_t id = 0;
    for (uint32_t i = 0; i < got; i++) {
        uint64_t b = spix[SPI_DR / 4] & 0xff;
        if (i >= 1 && i <= 3)
            id = (id << 8) | b;
        if (i < 6) {
            puts_("RPT spi_b");
            putc_('0' + (char)i);
            putc_(' ');
            report_sr(b);
        }
    }
    cs_high();
    spix[SPI_SSIENR / 4] = 0;
    return id;
}

/* ------------------------------------------------------------------ */
/* report                                                              */
/* ------------------------------------------------------------------ */
static void report_line(const char *label, uint64_t v, int hex)
{
    puts_("RPT ");
    puts_(label);
    putc_(' ');
    if (hex)
        puth(v, 16);
    else
        putd(v);
    puts_("\n");
}

uint64_t mk_mtime[2];

volatile uint64_t g_f_early[2];

void k210_probe_main(void)
{
    /* F test at the vendor crt.S startup point: NO trap has occurred
     * yet. crt.S recipe: FS=11, fssr x0, fmv.w.x. If F works HERE but
     * not after the first trap, the core loses FPU state in trap; if
     * it fails both places, F data path is dead on this die. */
    __asm__ volatile(
        "li t0, 0x6000\ncsrs mstatus, t0\n"
        "fscsr x0\n"
        "li t0, 0x3f800000\nfmv.w.x ft0, t0\nfmv.x.w a5, ft0\n"
        "sd a5, 0(%0)\n"
        "fsw ft0, 8(%0)"
        :
        : "r"(g_f_early)
        : "t0", "a5", "ft0", "memory");
    puts_("\nK210-PROBE 65.1 up (hart0)\n");
    report_line("f_early_fmv", g_f_early[0], 16);
    report_line("f_early_fsw", g_f_early[1], 16);

    /* with `kflash -t` the host port is open before boot, so nothing
     * is lost; a short settle, but an RX byte still starts the report
     * early on hosts that connect late and poke */
    for (volatile uint64_t i = 0; i < 3000000ULL; i++) {
        if (!(UARTHS[1] & UART_RX_EMPTY))
            break;
    }

    report_line("uarths_div", UARTHS[0x18 / 4], 16);
    {
        uint64_t v;
        __asm__ volatile("csrr %0, mtvec" : "=l"(v));
        report_line("mtvec", v, 16);
        for (int bit = 0; bit < 64; bit += 8) {
            uint64_t pat = 0xffULL << bit;
            __asm__ volatile("csrw mepc, %0\ncsrr %0, mepc" : "+l"(pat));
            puts_("RPT mepc_mask_");
            putc_((char)('0' + (bit / 10) % 10));
            putc_((char)('0' + bit % 10));
            puts_(" ");
            puth(pat, 16);
            puts_("\n");
        }
        __asm__ volatile("li t0, 0\ncsrw mepc, t0");
        __asm__ volatile("csrr %0, mstatus" : "=l"(v));
        report_line("mstatus_boot", v, 16);
    }
    {
        uint64_t v;
        __asm__ volatile("li t0, 0xa5a5\ncsrw mscratch, t0\ncsrr %0, mscratch"
                         : "=l"(v));
        report_line("mscratch_rw", v, 16);       /* expect a5a5 */
    }

    /* mret from plain M code with a software-set mepc (the core's
     * answer to "does return honour mepc here"): land == yes. */
    puts_("DBG mret_sem\n");
    {
        uint64_t before = g_trap_ticks;
        uint64_t here = (uint64_t)&&mret_after;
        __asm__ volatile("csrw mepc, %0" ::"r"(here));
        __asm__ volatile("mret");
        puts_("mret_fell_through\n");
    mret_after:
        report_line("mret_handler_reentries", g_trap_ticks - before, 0);
        report_line("mret_landed", 1, 0);
    }

    /* hart1: marker at entry proves both harts execute from boot;
     * then the vendor core_enable() dance: set release flag, send the
     * CLINT IPI; hart1 trap arrival proves cross-hart delivery works */
    uint64_t h1 = 0;
    for (volatile uint32_t i = 0; i < 8000000u && !h1; i++)
        h1 = *(volatile uint64_t *)HART1_MAGIC_ADDR;
    report_line("hart1_magic", h1, 16);
    {
        volatile uint64_t *m = (volatile uint64_t *)HART1_MAGIC_ADDR;
        m[1] = 1;                                   /* release          */
        *(volatile uint32_t *)(CLINT_MSIP0 + 4) = 1;/* ipi to hart 1    */
        for (uint32_t i = 0; i < 8000000u && !m[2]; i++)
            ;
        report_line("hart1_after_release", m[2], 16);
        for (uint32_t i = 0; i < 40u && !g_hart1_irq; i++)
            for (volatile uint32_t j = 0; j < 100000u; j++)
                ;
        report_line("hart1_ipi_trapped", g_hart1_irq, 0);
        *(volatile uint32_t *)(CLINT_MSIP0 + 4) = 0;
        m[0] = 0; m[1] = 0; m[2] = 0;
    }

    /* identification --------------------------------------------------- */
    report_line("misa_cause", guarded(op_misa), 16);
    report_line("misa", r, 16);
    report_line("mvendorid_cause", guarded(op_mvendorid), 16);
    report_line("mvendorid", r, 16);
    report_line("marchid_cause", guarded(op_marchid), 16);
    report_line("marchid", r, 16);
    report_line("mimpid_cause", guarded(op_mimpid), 16);
    report_line("mimpid", r, 16);
    {
        uint64_t hartid;
        __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
        report_line("hartid", hartid, 16);
    }

    /* delegation after the crt-style clear (numeric CSR numbers) --------- */
    report_line("medeleg_cause", guarded(op_medeleg), 16);
    report_line("medeleg", v_medeleg, 16);
    report_line("mideleg_cause", guarded(op_mideleg), 16);
    report_line("mideleg", v_mideleg, 16);

    /* mcycle and the mcycle/mtime ratio --------------------------------- */
    uint64_t c = guarded(op_mcycle);
    report_line("mcycle_cause", c, 16);
    if (c == 0) {
        uint64_t mc0, mc1, mt0, mt1;
        mt0 = mtime_read();
        __asm__ volatile("csrr %0, mcycle" : "=l"(mc0));
        for (volatile int i = 0; i < 20000; i++)
            ;
        __asm__ volatile("csrr %0, mcycle" : "=l"(mc1));
        mt1 = mtime_read();
        report_line("mcycle_first", mc0, 16);
        report_line("cycles_per_mtime_tick_x1000",
                    mt1 > mt0 ? (mc1 - mc0) * 1000 / (mt1 - mt0) : 0, 0);
    }

    /* mtime advances ------------------------------------------------------ */
    {
        uint64_t a = mtime_read();
        for (volatile int i = 0; i < 10000; i++)
            ;
        uint64_t b = mtime_read();
        report_line("mtime_advanced", b > a ? b - a : 0, 0);
    }

    /* BROM's clock claim and reset source ---------------------------------- */
    report_line("sysctl_clk_freq", *(volatile uint32_t *)(SYSCTL_BASE + 0x04), 0);
    report_line("sysctl_reset_status", *(volatile uint32_t *)(SYSCTL_BASE + 0x60), 16);

    /* wire timing: the host times these markers against its own clock ------- */
    mk_mtime[0] = mtime_read();
    puts_("MK0 ");
    putd(mk_mtime[0]);
    puts_("\n");
    for (int i = 0; i < 27600; i++)
        putc_('#');
    mk_mtime[1] = mtime_read();
    puts_("MK1 ");
    putd(mk_mtime[1]);
    puts_("\n");

    /* exception through the (now cleared) path: ecall must trap to M
     * with cause 11 and the guard must be able to unwind ------------- */
    puts_("DBG ecall_guard\n");
    report_line("ecall_cause", guarded(op_ecall), 16);   /* expect 11 */

    /* mepc low-16 write + full-capture model: advancing the frame mepc
     * by 4 and mret-ing must skip the ecall and land after it */
    puts_("DBG mret_skip\n");
    {
        extern volatile int g_trap_unexpected;
        uint64_t u0 = (uint64_t)g_trap_unexpected;
        op_ecall_skip4();
        report_line("mret_skip_landed", g_skip_landed, 0);
        report_line("mret_skip_traps", (uint64_t)g_trap_unexpected - u0, 0);
    }

    /* PMP ------------------------------------------------------------------ */
    puts_("DBG pmpcfg\n");
    report_line("pmpcfg_write_cause", guarded(op_pmpcfg_write), 16);
    report_line("pmpcfg0_readback", pmp_read_cfg0(), 16);
    report_line("pmpaddr_write_cause", guarded(op_pmpaddr_write), 16);
    report_line("pmpaddr0_readback", pmp_read_addr0(), 16);

    /* which MPP values survive a write -------------------------------------- */
    for (uint64_t mpp = 0; mpp <= 3; mpp++) {
        uint64_t old, v;
        __asm__ volatile("csrr %0, mstatus" : "=r"(old));
        old = (old & ~0x1800ULL) | (mpp << 11);
        __asm__ volatile("csrw mstatus, %0" ::"r"(old));
        __asm__ volatile("csrr %0, mstatus" : "=r"(v));
        puts_("RPT mpp[");
        putc_((char)('0' + (char)mpp));
        puts_("]=");
        putd((v >> 11) & 3);
        puts_("\n");
    }

    /* the U-mode experiment -------------------------------------------------- */
    puts_("DBG umode\n");
    g_u_reached = 0;
    c = guarded(op_umode);
    report_line("umode_cause", c, 16);
    report_line("u_reached", g_u_reached, 0);

    /* FS writability ------------------------------------------------------------ */
    for (uint64_t fs = 0; fs <= 3; fs++) {
        uint64_t old, v;
        __asm__ volatile("csrr %0, mstatus" : "=r"(old));
        old = (old & ~0x6000ULL) | (fs << 13);
        __asm__ volatile("csrw mstatus, %0" ::"r"(old));
        __asm__ volatile("csrr %0, mstatus" : "=r"(v));
        puts_("RPT fs[");
        putc_((char)('0' + (char)fs));
        puts_("]=");
        putd((v >> 13) & 3);
        puts_("\n");
    }

    /* F and D -------------------------------------------------------------------- */
    __asm__ volatile("li t0, 0x2000\ncsrs mstatus, t0");   /* FS = initial */
    report_line("fpu_single_cause", guarded(op_fsingle), 16);
    report_line("fpu_single_val", r, 16);
    report_line("fpu_double_cause", guarded(op_fdouble), 16);
    report_line("fpu_double_val", r, 16);
    __asm__ volatile("li t0, 0x6000\ncsrc mstatus, t0");   /* FS off again */

    /* atomics --------------------------------------------------------------------- */
    report_line("amoswap_cause", guarded(op_amoswap), 16);
    atomic_mem = 0x99;
    guarded(op_amoswap);
    report_line("amoswap_ok", r == 0x99 ? 1 : 0, 0);
    report_line("amoadd_cause", guarded(op_amoadd), 16);
    report_line("amoadd_ok", r == 82 ? 1 : 0, 0);
    report_line("lrsc_cause", guarded(op_lrsc), 16);
    report_line("lrsc_ok", r == 1 ? 1 : 0, 0);

    /* PLIC -------------------------------------------------------------------------- */
    report_line("plic_word0", *(volatile uint32_t *)(PLIC_BASE), 16);
    report_line("plic_version", *(volatile uint32_t *)(PLIC_BASE + 0x7FF000), 16);

    /* CLINT software interrupt -------------------------------------------------------- */
    g_msip_seen = 0;
    *(volatile uint32_t *)CLINT_MSIP0 = 1;
    __asm__ volatile("csrs mie, 8\ncsrs mstatus, 8");   /* MSIE + MIE */
    for (uint32_t i = 0; i < 2000000u && !g_msip_seen; i++)
        ;
    __asm__ volatile("csrc mie, 8\ncsrc mstatus, 8");
    report_line("msip_seen", g_msip_seen, 0);

    /* CLINT timer interrupt ----------------------------------------------------------- */
    g_timer_seen = 0;
    *(volatile uint64_t *)CLINT_MTIMECMP = mtime_read() + 1000;
    __asm__ volatile("li t0, 0x80\ncsrs mie, t0\ncsrs mstatus, 8");
    for (uint32_t i = 0; i < 8000000u && g_timer_seen < 3; i++)
        ;
    __asm__ volatile("li t0, 0x80\ncsrc mie, t0\ncsrc mstatus, 8");
    *(volatile uint64_t *)CLINT_MTIMECMP = UINT64_MAX;
    report_line("timer_seen", g_timer_seen, 0);
    report_line("irq_unexpected", g_irq_unexpected, 0);

    /* SPI NOR ------------------------------------------------------------------------- */
    report_line("spi0_idr", spi_idr(), 16);
    report_line("spi1_idr", spi1[0x58 / 4], 16);
    report_line("spi0_ssic_ver", spi_ssic_ver(), 16);
    /* NOR RDID: deferred whole to 65.5 (measured so far: 0x20000000
     * load-faults — no XIP; GPIO 0x19000000 stores fault; SSI enables
     * and RX fills but framing/CS owner (SFC vs SSI + GPIOHS) unknown) */
    (void)fpioa_flash_pins;
    (void)jedec_try;

    report_line("unexpected_traps", (uint64_t)g_trap_unexpected, 0);
    puts_("K210-PROBE DONE\n");
    for (;;)
        ;
}
