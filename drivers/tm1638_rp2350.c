/*
 * LugalOS Hardware Driver: QYF-TM1638 8-Digit 7-Segment Display + 4x4 Keypad
 * Vendored and adapted from ~/gith/domschl/LugalChess (firmware/tm1638.c, H2,
 * plan/phase9_chess_computer.md).
 *
 * Hardware Mapping (cmake/board-rp2350.cmake is the source of truth):
 *   GP6 : STB (Strobe, Function 5 - SIO GPIO)
 *   GP7 : CLK (Clock,  Function 5 - SIO GPIO)
 *   GP8 : DIO (Data,   Function 5 - SIO GPIO, bidirectional)
 *
 * A simple bit-banged 3-wire protocol, not a real SPI/I2C peripheral -- no
 * hardware controller dependency beyond the raw SIO register access already
 * used for the LED pins (drivers/uart_rp2350.c).
 */

#include "drivers/tm1638.h"
#include "kernel/time.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/palloc.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include <stdbool.h>
#include <string.h>

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_IN             (SIO_BASE + 0x004)
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)
#define SIO_GPIO_OE_CLR         (SIO_BASE + 0x040)

#define REG(addr) (*(volatile uint32_t *)(addr))

/* M5 Phase 2, plan/phase12_microkernel_migration.md: RP2350's Secure/
 * Non-secure SIO split, first found and fixed for heartbeat's own GPIO
 * (drivers/uart_rp2350.c) -- a Non-secure (U-mode) SIO bus access is
 * filtered per-GPIO by ACCESSCTRL's GPIO_NSMASK0, and defaults to
 * Secure-only (all bits 0) on reset. tm1638_init() below sets it for
 * STB/CLK/DIO the same way uart_init() does for the heartbeat LED. See
 * that file's own comment on ACCESSCTRL_GPIO_NSMASK0 for the full
 * datasheet citation and reasoning; not repeated here. */
#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c) /* GPIO 0-31 */

#define STB_PIN CONFIG_TM1638_STB_GPIO
#define CLK_PIN CONFIG_TM1638_CLK_GPIO
#define DIO_PIN CONFIG_TM1638_DIO_GPIO
#define STB_MASK (1u << STB_PIN)
#define CLK_MASK (1u << CLK_PIN)
#define DIO_MASK (1u << DIO_PIN)

/* M5 Phase 2, plan/phase12_microkernel_migration.md: the same attribute
 * heartbeat's own U-mode conversion established (drivers/uart_rp2350.c),
 * defined here (rather than later, next to the functions that need it) so
 * font7seg below -- read by both the kernel-mode fallback path and the new
 * U-mode task, and therefore needing to live somewhere U-mode can reach --
 * can use it too. Redefined locally rather than shared through a header:
 * a section attribute is two lines, and sharing it would suggest more
 * coupling between the two files' U-mode code than exists.
 * no_sanitize("undefined"): UBSan's own instrumented checks are kernel
 * .text calls, unreachable from a page U-mode can execute but the kernel
 * does not treat as its own .text. */
#define TM1638_UATTR __attribute__((section(".utext"))) __attribute__((no_sanitize("undefined")))

/* A distinct section name (not plain ".utext"), because GCC refuses to mix
 * const data and executable code in one section -- read-only-data and
 * executable get different section flags, and "same name, different
 * flags" is exactly the "section type conflict" this data/code split
 * avoids. Still lands in the same linked page: the linker script's
 * `*(.utext .utext.*)` wildcard (linker/rp2350.ld) matches this name too. */
#define TM1638_UDATA __attribute__((section(".utext.rodata")))

/* 7-segment font mapping for printable ASCII characters. In .utext (not
 * ordinary .rodata) so the U-mode task's own display routine below can
 * read it -- board_text_region()'s own grant is execute+read, which this
 * is the intended use of: "a task can run kernel code... and read
 * constants". A single shared table, safe across the privilege boundary
 * because it is never written. */
TM1638_UDATA static const uint8_t font7seg[128] = {
    [' '] = 0x00, ['-'] = 0x40, ['_'] = 0x08, ['='] = 0x48,
    ['0'] = 0x3F, ['1'] = 0x06, ['2'] = 0x5B, ['3'] = 0x4F,
    ['4'] = 0x66, ['5'] = 0x6D, ['6'] = 0x7D, ['7'] = 0x07,
    ['8'] = 0x7F, ['9'] = 0x6F,
    ['A'] = 0x77, ['B'] = 0x7C, ['C'] = 0x39, ['D'] = 0x5E,
    ['E'] = 0x79, ['F'] = 0x71, ['G'] = 0x3D, ['H'] = 0x76,
    ['I'] = 0x06, ['J'] = 0x1E, ['L'] = 0x38, ['O'] = 0x3F,
    ['P'] = 0x73, ['S'] = 0x6D, ['U'] = 0x3E, ['Y'] = 0x6E,
    ['a'] = 0x5F, ['b'] = 0x7C, ['c'] = 0x58, ['d'] = 0x5E,
    ['e'] = 0x79, ['f'] = 0x71, ['g'] = 0x6F, ['h'] = 0x74,
    ['i'] = 0x04, ['j'] = 0x0E, ['l'] = 0x06, ['n'] = 0x54,
    ['o'] = 0x5C, ['p'] = 0x73, ['r'] = 0x50, ['u'] = 0x1C,
    ['t'] = 0x78, ['y'] = 0x6E,

    /* Additional approximations for letters that are otherwise missing */
    ['N'] = 0x54, ['R'] = 0x50, ['Q'] = 0x67, ['q'] = 0x67,
    ['K'] = 0x76, ['k'] = 0x76, ['M'] = 0x37, ['m'] = 0x54,
    ['W'] = 0x3E, ['w'] = 0x1C, ['V'] = 0x3E, ['v'] = 0x1C,
    ['T'] = 0x78
};

/* Internal 16-byte RAM cache corresponding to display & LED addresses */
static uint8_t tm1638_ram[16];

static inline void stb_low(void)  { REG(SIO_GPIO_OUT_CLR) = STB_MASK; }
static inline void stb_high(void) { REG(SIO_GPIO_OUT_SET) = STB_MASK; }

/* Write a byte to DIO with stable timing delays. Caller must ensure DIO is
 * in output mode. */
static void tm1638_write_byte(uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        if ((byte >> i) & 1) {
            REG(SIO_GPIO_OUT_SET) = DIO_MASK;
        } else {
            REG(SIO_GPIO_OUT_CLR) = DIO_MASK;
        }
        time_delay_us(3); /* Clock low pulse width */
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
        time_delay_us(3); /* Clock high pulse width */
    }
}

/* Read a byte from DIO. Caller must ensure DIO is in input mode. */
static uint8_t tm1638_read_byte(void) {
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        time_delay_us(3);
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
        time_delay_us(3); /* TM1638 shifts data out on rising edge */
        if (REG(SIO_GPIO_IN) & DIO_MASK) {
            byte |= (1u << i);
        }
    }
    return byte;
}

static void tm1638_send_command(uint8_t cmd) {
    stb_low();
    tm1638_write_byte(cmd);
    stb_high();
    time_delay_us(2); /* Strobe recovery delay */
}

/* Flush local RAM cache to TM1638 display registers using auto-increment
 * address mode (0x40) */
static void tm1638_flush(void) {
    tm1638_send_command(0x40);

    stb_low();
    tm1638_write_byte(0xC0); /* Start address */
    for (int i = 0; i < 16; i++) {
        tm1638_write_byte(tm1638_ram[i]);
    }
    stb_high();
    time_delay_us(2);
}

void tm1638_init(void) {
    REG(IO_BANK0_CTRL(STB_PIN)) = 5;
    REG(IO_BANK0_CTRL(CLK_PIN)) = 5;
    REG(IO_BANK0_CTRL(DIO_PIN)) = 5;
    REG(PADS_BANK0_PAD(STB_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(CLK_PIN)) = 0x5A;
    /* DIO keeps its internal pull-up (PUE=1, part of 0x5A) for when it
     * switches to input during key reads. */
    REG(PADS_BANK0_PAD(DIO_PIN)) = 0x5A;

    REG(SIO_GPIO_OE_SET) = STB_MASK | CLK_MASK | DIO_MASK;

    /* M5 Phase 2: STB/CLK/DIO need to be Non-secure-accessible for the
     * U-mode task's own serve loop below to actually toggle them -- must
     * happen here, from M-mode, before the task exists. See
     * ACCESSCTRL_GPIO_NSMASK0's own comment above. */
    REG(ACCESSCTRL_GPIO_NSMASK0) |= (STB_MASK | CLK_MASK | DIO_MASK);

    stb_high();
    REG(SIO_GPIO_OUT_SET) = CLK_MASK;
    REG(SIO_GPIO_OUT_CLR) = DIO_MASK;

    time_delay_us(10000);

    tm1638_send_command(0x8F); /* Activate display, max brightness */

    memset(tm1638_ram, 0, sizeof(tm1638_ram));
    tm1638_flush();
}

static void tm1638_hw_display_string(const char *str) {
    uint8_t buffer[8];
    memset(buffer, 0, sizeof(buffer));

    int len = (int)strlen(str);
    int buf_idx = 0;

    for (int i = 0; i < len && buf_idx < 8; i++) {
        char c = str[i];

        if (c == '.' && buf_idx > 0) {
            buffer[buf_idx - 1] |= 0x80;
        } else {
            uint8_t pattern = 0x00;
            if ((uint8_t)c < 128) {
                pattern = font7seg[(uint8_t)c];
            }
            buffer[buf_idx++] = pattern;
        }
    }

    for (int seg = 0; seg < 8; seg++) {
        tm1638_ram[seg * 2] = 0x00;
    }

    /* CRITICAL: Transpose the 8x8 matrix (QYF-TM1638 is Common Anode).
     * Digit i segment s is bit s of buffer[i]. Digit index 0 (left-most
     * character) maps to bit 7 (Digit 1 on module); digit index 7
     * (right-most character) maps to bit 0 (Digit 8 on module). */
    for (int seg = 0; seg < 8; seg++) {
        uint8_t val = 0;
        for (int digit = 0; digit < 8; digit++) {
            if ((buffer[digit] >> seg) & 1) {
                val |= (1u << (7 - digit));
            }
        }
        tm1638_ram[seg * 2] = val;
    }

    tm1638_flush();
}

static void tm1638_hw_set_leds(uint8_t mask) {
    for (int i = 0; i < 8; i++) {
        tm1638_ram[(i * 2) + 1] = (mask >> i) & 1;
    }
    tm1638_flush();
}

static int tm1638_hw_get_key(void) {
    uint8_t keys[4];

    stb_low();
    tm1638_write_byte(0x42); /* Read keys command */

    REG(SIO_GPIO_OE_CLR) = DIO_MASK; /* DIO -> input for the reply */
    time_delay_us(5); /* Wait for DIO turnaround */

    for (int i = 0; i < 4; i++) {
        keys[i] = tm1638_read_byte();
    }

    REG(SIO_GPIO_OE_SET) = DIO_MASK; /* DIO -> output for subsequent writes */

    stb_high();
    time_delay_us(2); /* Strobe recovery delay */

    /* Phantom key rejection: count total pressed keys across all bytes. If
     * more than 1 key appears pressed, it's noise from the bus. Key-relevant
     * bits per byte: 0x04 (K3/KS_odd), 0x40 (K3/KS_even), 0x02 (K2/KS_odd),
     * 0x20 (K2/KS_even). */
    int total_pressed = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = keys[i] & 0x66;
        keys[i] = b;
        while (b) {
            total_pressed += (b & 1);
            b >>= 1;
        }
    }
    if (total_pressed != 1) {
        return -1; /* No key or phantom (multiple keys / noise) */
    }

    /* Matrix key decoding: K3 line -> S1..S8 -> File keys (indices 0..7);
     * K2 line -> S9..S16 -> Rank keys (indices 8..15). */
    for (int i = 0; i < 4; i++) {
        uint8_t b = keys[i];
        if (b & 0x04) return i * 2 + 0;
        if (b & 0x40) return i * 2 + 1;
        if (b & 0x02) return i * 2 + 8;
        if (b & 0x20) return i * 2 + 9;
    }

    return -1;
}

/* M4.5, plan/phase12_microkernel_migration.md, Part B: the driver as a task.
 * tm1638_hw_get_key() is polled from user/chess/src/chess_ui.c's tm_wait_key()
 * at a paced 20ms interval (~50/sec while actively waiting on a human, never
 * a tight loop) -- no batching redesign needed, one call already carries one
 * whole logical operation, same shape as drivers/spisd_rp2350.c's read/write.
 *
 *   'D' display_string req: [op] + str(len, up to TM1638_STR_MAX) resp: 0 bytes
 *   'L' set_leds       req: [op] + mask(1)                        resp: 0 bytes
 *   'K' get_key        req: [op]                                  resp: key(1, int8_t)
 *
 * TM1638_STR_MAX bounds the one variable-length op: every real caller in
 * this tree only ever displays across the module's 8 digits (plus '.'
 * suffixes, which don't consume a slot -- tm1638_hw_display_string()'s own
 * logic above), so 32 is headroom over anything actually used, not a
 * measured ceiling; an oversized string falls back to direct access below
 * rather than being refused outright, same shape as BLK_MAX_COUNT. */
#define TM1638_OP_DISPLAY_STRING ((uint8_t)'D')
#define TM1638_OP_SET_LEDS       ((uint8_t)'L')
#define TM1638_OP_GET_KEY        ((uint8_t)'K')

#define TM1638_STR_MAX 32u
#define TM1638_REQ_CAP (1u + TM1638_STR_MAX)
#define TM1638_RESP_CAP 1u

static uint8_t         g_tm1638_req[TM1638_REQ_CAP];
static uint8_t         g_tm1638_resp[TM1638_RESP_CAP];
static chan_endpoint_t *g_tm1638_ep;
static int              g_tm1638_task_pid = -1;

/* M4.5 verify: counts chan_call()s actually served -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning. */
static uint32_t g_tm1638_calls;

uint32_t tm1638_task_call_count(void) { return g_tm1638_calls; }

static bool tm1638_task_alive(void) {
    if (g_tm1638_task_pid < 0) return false;
    int st = sched_task_state(g_tm1638_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* ---- U-mode implementation, M5 Phase 2, plan/phase12_microkernel_migration.md ----
 *
 * A second, independent copy of the bit-bang protocol above
 * (tm1638_write_byte/read_byte/send_command/flush/hw_display_string/
 * hw_set_leds/hw_get_key), tagged TM1638_UATTR and reachable only from the
 * U-mode task's own serve loop below -- not a refactor of the existing
 * kernel-mode versions into something shared. Those keep serving the
 * direct-hardware fallback path exactly as before (unreachable from
 * U-mode: the fallback runs in kernel/M-mode, before the task exists or if
 * it never started), and their own tm1638_ram lives in ordinary kernel
 * .bss, which a U-mode domain has no business being granted access to. The
 * two copies never run concurrently -- the facade functions below route to
 * one or the other depending on tm1638_task_alive() -- so there is no
 * consistency requirement between their two independent RAM-cache states.
 *
 * tm1638_usys_ram: 16 bytes of persistent state that must survive separate
 * chan_serve_wait()/chan_serve_reply() round trips (display_string() and
 * set_leds() each touch only half of it), so it cannot be a plain
 * function-local. Rather than spend a fourth PMP region on 16 bytes (still
 * needing its own page-aligned NAPOT grant -- the same "coarse but honest"
 * lesson heartbeat's own SIO window learned), it lives inside the last 16
 * bytes of the task's own dedicated stack page -- already RW-granted,
 * already page-aligned, and actual stack usage (a handful of small local
 * buffers) stays nowhere near that boundary. */
#define TM1638_USTACK_SIZE 4096u
#define TM1638_URAM_SIZE   16u
static uint8_t g_tm1638_ustack[TM1638_USTACK_SIZE] __attribute__((aligned(4096)));
#define tm1638_usys_ram (&g_tm1638_ustack[TM1638_USTACK_SIZE - TM1638_URAM_SIZE])

/* Hand-rolled per translation unit, not shared with drivers/uart_rp2350.c's
 * heartbeat_usys_*() or user/progs/usys.h -- a TM1638_UATTR function must
 * not call anything the compiler might place outside .utext, and a
 * cross-file inline is not a guarantee. */
__attribute__((always_inline)) static inline void tm1638_usys_delay_us(long us) {
    register long r_a0 __asm__("a0") = SYS_DELAY_US;
    register long r_a1 __asm__("a1") = us;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
}
__attribute__((always_inline)) static inline long tm1638_usys_chan_serve_wait(const char *name, uint8_t *buf, long buf_max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = buf_max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}
__attribute__((always_inline)) static inline long tm1638_usys_chan_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

TM1638_UATTR static void tm1638_usys_stb_low(void)  { REG(SIO_GPIO_OUT_CLR) = STB_MASK; }
TM1638_UATTR static void tm1638_usys_stb_high(void) { REG(SIO_GPIO_OUT_SET) = STB_MASK; }

TM1638_UATTR static void tm1638_usys_write_byte(uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        if ((byte >> i) & 1) {
            REG(SIO_GPIO_OUT_SET) = DIO_MASK;
        } else {
            REG(SIO_GPIO_OUT_CLR) = DIO_MASK;
        }
        tm1638_usys_delay_us(3);
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
        tm1638_usys_delay_us(3);
    }
}

TM1638_UATTR static uint8_t tm1638_usys_read_byte(void) {
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        tm1638_usys_delay_us(3);
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
        tm1638_usys_delay_us(3);
        if (REG(SIO_GPIO_IN) & DIO_MASK) byte |= (1u << i);
    }
    return byte;
}

TM1638_UATTR static void tm1638_usys_send_command(uint8_t cmd) {
    tm1638_usys_stb_low();
    tm1638_usys_write_byte(cmd);
    tm1638_usys_stb_high();
    tm1638_usys_delay_us(2);
}

TM1638_UATTR static void tm1638_usys_flush(void) {
    tm1638_usys_send_command(0x40);

    tm1638_usys_stb_low();
    tm1638_usys_write_byte(0xC0);
    for (int i = 0; i < 16; i++) {
        tm1638_usys_write_byte(tm1638_usys_ram[i]);
    }
    tm1638_usys_stb_high();
    tm1638_usys_delay_us(2);
}

/* Takes an explicit length rather than relying on a NUL terminator: the
 * request bytes it reads from are a raw slice of the endpoint's message,
 * not a C string. */
TM1638_UATTR static void tm1638_usys_display_string(const char *str, uint32_t len) {
    /* Not memset(): under -fno-builtin (this whole tree's build flags),
     * memset()/memcpy() calls are not inlined -- they compile to a real
     * call to libc's own implementation, which lives in ordinary kernel
     * .text, outside every region this task's domain grants. Found the
     * hard way, as a real hardware instruction-access fault (cause 1) the
     * first time this ran on real silicon, immediately after the string-
     * literal fix above resolved the earlier hang -- the same "an
     * ordinary-looking C construct silently reaches outside .utext" class
     * of bug, just a different construct. Written out explicitly instead,
     * same discipline kernel/shell.c's user_deputy() already uses for its
     * own path[] and this exact reasoning generalizes from. */
    uint8_t buffer[8];
    for (int i = 0; i < 8; i++) buffer[i] = 0;

    int buf_idx = 0;
    for (uint32_t i = 0; i < len && buf_idx < 8; i++) {
        char c = str[i];
        if (c == '.' && buf_idx > 0) {
            buffer[buf_idx - 1] |= 0x80;
        } else {
            uint8_t pattern = 0x00;
            if ((uint8_t)c < 128) pattern = font7seg[(uint8_t)c];
            buffer[buf_idx++] = pattern;
        }
    }

    for (int seg = 0; seg < 8; seg++) tm1638_usys_ram[seg * 2] = 0x00;
    for (int seg = 0; seg < 8; seg++) {
        uint8_t val = 0;
        for (int digit = 0; digit < 8; digit++) {
            if ((buffer[digit] >> seg) & 1) val |= (1u << (7 - digit));
        }
        tm1638_usys_ram[seg * 2] = val;
    }

    tm1638_usys_flush();
}

TM1638_UATTR static void tm1638_usys_set_leds(uint8_t mask) {
    for (int i = 0; i < 8; i++) tm1638_usys_ram[(i * 2) + 1] = (mask >> i) & 1;
    tm1638_usys_flush();
}

TM1638_UATTR static int tm1638_usys_get_key(void) {
    uint8_t keys[4];

    tm1638_usys_stb_low();
    tm1638_usys_write_byte(0x42); /* Read keys command */

    REG(SIO_GPIO_OE_CLR) = DIO_MASK;
    tm1638_usys_delay_us(5);

    for (int i = 0; i < 4; i++) keys[i] = tm1638_usys_read_byte();

    REG(SIO_GPIO_OE_SET) = DIO_MASK;

    tm1638_usys_stb_high();
    tm1638_usys_delay_us(2);

    int total_pressed = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = keys[i] & 0x66;
        keys[i] = b;
        while (b) { total_pressed += (b & 1); b >>= 1; }
    }
    if (total_pressed != 1) return -1;

    for (int i = 0; i < 4; i++) {
        uint8_t b = keys[i];
        if (b & 0x04) return i * 2 + 0;
        if (b & 0x40) return i * 2 + 1;
        if (b & 0x02) return i * 2 + 8;
        if (b & 0x20) return i * 2 + 9;
    }
    return -1;
}

TM1638_UATTR static void tm1638_umode_body(void) {
    /* Not a string literal: a literal lands in ordinary .rodata, outside
     * every region this task's domain grants, so strncpy_from_user()
     * (arch/riscv/common/trap.c) refuses to read it -- found the hard way,
     * as a full board hang rather than a clean refusal, the first time
     * this code ran on real hardware; reproduced and root-caused cleanly
     * on QEMU afterward via a synthetic echo-service probe before this fix
     * ever touched hardware again. Same class of bug kernel/shell.c's
     * user_deputy() already documents for this exact reason (its own
     * path[] comment), applied here for the first time to a
     * chan_serve_wait()/chan_serve_reply() endpoint name rather than a
     * SYS_READ_FILE path. volatile: gcc recognises a run of consecutive
     * stores and turns it back into a copy from a .rodata blob otherwise. */
    volatile char name[7];
    name[0]='t'; name[1]='m'; name[2]='1'; name[3]='6'; name[4]='3';
    name[5]='8'; name[6]='\0';

    for (;;) {
        uint8_t req[TM1638_REQ_CAP];
        long req_len = tm1638_usys_chan_serve_wait((const char *)name, req, sizeof(req));
        if (req_len < 1) {
            tm1638_usys_chan_serve_reply((const char *)name, NULL, 0);
            continue;
        }

        uint8_t op = req[0];
        uint8_t resp[TM1638_RESP_CAP];
        uint32_t resp_len = 0;
        switch (op) {
        case TM1638_OP_DISPLAY_STRING: {
            uint32_t len = (uint32_t)req_len - 1;
            if (len > TM1638_STR_MAX) len = TM1638_STR_MAX;
            tm1638_usys_display_string((const char *)&req[1], len);
            break;
        }
        case TM1638_OP_SET_LEDS:
            if (req_len >= 2) tm1638_usys_set_leds(req[1]);
            break;
        case TM1638_OP_GET_KEY:
            resp[0] = (uint8_t)(int8_t)tm1638_usys_get_key();
            resp_len = 1;
            break;
        default:
            break;
        }
        tm1638_usys_chan_serve_reply((const char *)name, resp, resp_len);
    }
}

static mem_domain_t g_tm1638_domain;

/* This task's own kernel-mode entry point: task_create_sized() calls this
 * (ordinary kernel stack, kernel privilege) to build the domain and make
 * the one-way jump into U-mode. Mirrors drivers/uart_rp2350.c's
 * heartbeat_task_body() shape exactly: the same 3-region domain (own
 * stack, the shared .utext page, a 4096-byte SIO window), the same
 * refuse-rather-than-claim-unverified-isolation rule. */
static void tm1638_task_body(void *arg) {
    (void)arg;
    while (!g_tm1638_ep) sched_yield();

    mem_domain_init(&g_tm1638_domain);
    mem_domain_add(&g_tm1638_domain, (uintptr_t)g_tm1638_ustack,
                   sizeof(g_tm1638_ustack), MEM_R | MEM_W);

    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&g_tm1638_domain, tbase, tsize, MEM_R | MEM_X);

    mem_domain_add(&g_tm1638_domain, SIO_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &g_tm1638_domain) != 0) {
        printk("[TM1638] Refusing to enter U-mode: memory domain not enforceable; keypad/display stay on direct hardware access.\n");
        return;
    }
    /* The stack top handed to arch_enter_user() stops short of the full
     * page -- the last TM1638_URAM_SIZE bytes are tm1638_usys_ram's own
     * storage (see its comment above), not usable stack. */
    arch_enter_user(tm1638_umode_body,
                    (uintptr_t)g_tm1638_ustack + TM1638_USTACK_SIZE - TM1638_URAM_SIZE,
                    0, 0, 0);
}

/* Called from kernel/main.c, after sched_init(). Not fatal if it fails:
 * every function below falls back to direct hardware access whenever the
 * task is not alive, same as every boot-time call before this ever ran. */
int tm1638_task_start(void) {
    int pid = task_create_sized("tm1638", tm1638_task_body, NULL, 1);
    if (pid < 0) {
        printk("[TM1638] Could not start the tm1638 task; keypad/display stay on direct hardware access.\n");
        return -1;
    }
    if (chan_register_task("tm1638", pid, g_tm1638_req, sizeof(g_tm1638_req),
                           g_tm1638_resp, sizeof(g_tm1638_resp)) != 0) {
        printk("[TM1638] Could not register the tm1638 channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_tm1638_ep = chan_lookup("tm1638");
    g_tm1638_task_pid = pid;
    printk("[TM1638] Driver running as task #%d, reachable via chan_call(\"tm1638\", ...)\n", pid);
    return pid;
}

/* M5 Phase 2's own "Verify" deliverable: does the real tm1638 domain shape
 * (stack + .utext + a 4096-byte SIO window) actually confine the task to
 * GPIO, or does the SIO grant's width accidentally cover more? Modeled
 * directly on drivers/uart_rp2350.c's heartbeat_isolation_test() -- same
 * idea (a deliberate out-of-domain store, asserted to fault), a separate
 * canary rather than reaching into either heartbeat's or kernel/shell.c's,
 * for the same reason the syscall stubs above are hand-rolled per file. */
static volatile uintptr_t g_tm1638_canary = 0xC0FFEE;

TM1638_UATTR static void tm1638_intruder(void) {
    g_tm1638_canary = 0xDEAD;
    for (;;) { } /* only reached if the store was NOT stopped */
}

/* Set only once the task has actually reached U-mode -- without it, a
 * domain that failed to install would read identically to "the write was
 * correctly blocked" (same false-positive risk cmd_usertest_isolation()'s
 * own g_user_entered exists to rule out). */
static volatile bool g_tm1638_intruder_entered;

/* `arg` is the U-mode stack -- allocated by tm1638_isolation_test() below,
 * not here, so it can free it once the task is confirmed DEAD (same shape
 * as heartbeat_isolation_test()'s own probe). */
static void tm1638_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    /* The exact grant real tm1638 runs under -- this is what's on trial. */
    mem_domain_add(&dom, SIO_BASE, 4096, MEM_R | MEM_W);

    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[TM1638Iso] Refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_tm1638_intruder_entered = true;
    arch_enter_user(tm1638_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

/* Runs the probe to completion and reports what actually happened. Returns
 * false if the task never reached U-mode (domain not enforceable on this
 * build/core, or the one-page stack could not be allocated) -- in that
 * case *out_canary and *out_exited_clean say nothing about isolation,
 * matching cmd_usertest_isolation()'s own "INCONCLUSIVE" case. */
bool tm1638_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_tm1638_canary = 0xC0FFEE;
    g_tm1638_intruder_entered = false;

    void *ustack = palloc_pages(1);
    if (!ustack) {
        *out_canary = g_tm1638_canary;
        *out_exited_clean = true;
        return false;
    }

    int pid = task_create("tm1638_intruder", tm1638_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        *out_canary = g_tm1638_canary;
        *out_exited_clean = true;
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) {
        sched_yield();
    }
    long status;
    *out_exited_clean = sched_task_exited_cleanly(pid, &status);
    *out_canary = g_tm1638_canary;
    palloc_free(ustack, 1);
    return g_tm1638_intruder_entered;
}

static int tm1638_call_with_retry(const uint8_t *req, uint32_t req_len,
                                  uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_tm1638_ep, req, req_len, resp, resp_max);
        /* M5 Phase 2: counted here, on the client side, rather than in the
         * server's own serve loop as it was before that loop moved to
         * U-mode -- a U-mode task cannot touch g_tm1638_calls, an ordinary
         * kernel .bss global no domain grants it. Every successful
         * chan_call() here was, by construction, actually served, so the
         * count means the same thing either way. */
        if (n >= 0) { g_tm1638_calls++; return n; }
        sched_yield();
    }
    return -1;
}

void tm1638_display_string(const char *str) {
    size_t len = strlen(str);
    if (len <= TM1638_STR_MAX && tm1638_task_alive()) {
        uint8_t req[1 + TM1638_STR_MAX];
        req[0] = TM1638_OP_DISPLAY_STRING;
        memcpy(&req[1], str, len);
        uint8_t resp[TM1638_RESP_CAP];
        if (tm1638_call_with_retry(req, 1u + (uint32_t)len, resp, sizeof(resp)) >= 0) return;
    }
    tm1638_hw_display_string(str);
}

void tm1638_set_leds(uint8_t mask) {
    if (tm1638_task_alive()) {
        uint8_t req[2] = { TM1638_OP_SET_LEDS, mask };
        uint8_t resp[TM1638_RESP_CAP];
        if (tm1638_call_with_retry(req, sizeof(req), resp, sizeof(resp)) >= 0) return;
    }
    tm1638_hw_set_leds(mask);
}

int tm1638_get_key(void) {
    if (tm1638_task_alive()) {
        uint8_t req[1] = { TM1638_OP_GET_KEY };
        uint8_t resp[TM1638_RESP_CAP];
        int n = tm1638_call_with_retry(req, sizeof(req), resp, sizeof(resp));
        if (n >= 1) return (int)(int8_t)resp[0];
        /* IPC failed -- fall through to direct access. */
    }
    return tm1638_hw_get_key();
}
