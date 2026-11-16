#include "drivers/cardkb2.h"
#include "drivers/i2c_bus.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <stddef.h>

/* CardKB2 transports. With CONFIG_CARDKB2_RX_GPIO the unit is read on UART1 (its UART mode);
 * without, it is polled over I2C (its factory mode). Either way a task turns the unit's
 * output into console bytes through the key logic and queues them for the console pump.
 */

#define KQ_SIZE         64u

static struct {
    bool     detected;
    bool     registered;
    bool     task_started;
    uint8_t  firmware;
    uint8_t  last_key;
    uint32_t keys, dropped, packets, last_id;
    uint32_t i2c_filtered, i2c_read_fail;
} g;   /* single task: cardkb2 (display counters are read racily by `cardkb`, accepted) */

/* accepted: a single-producer single-consumer ring. Only the cardkb2 task writes g_kq and g_kq_head;
 * only the console pump writes g_kq_tail, and it runs one at a time under the console's input
 * lock, whichever task is pumping. Each side only reads the other's index, and a stale read
 * errs toward "empty" or "full", never toward a torn byte. */
static uint8_t g_kq[KQ_SIZE];
/* accepted: as g_kq above. */
static volatile uint32_t g_kq_head, g_kq_tail;

bool    cardkb2_is_detected(void) { return g.detected; }
uint8_t cardkb2_firmware(void)    { return g.firmware; }

static bool in_has_char(void) { return g_kq_head != g_kq_tail; }

static int in_getc(void) {
    if (g_kq_head == g_kq_tail) return -1;
    uint8_t c = g_kq[g_kq_tail % KQ_SIZE];
    g_kq_tail++;
    return c;
}

/* A Ctrl-C waiting in the queue, answered without consuming, so it latches even when the console's
 * ring is full (as the USB keyboard's does). Reads the queue the task fills: indices only move forward. */
static bool in_peek_interrupt(void) {
    for (uint32_t i = g_kq_tail; i != g_kq_head; i++)
        if (g_kq[i % KQ_SIZE] == 0x03u) return true;
    return false;
}

static const console_input_t g_in = { "cardkb2", in_has_char, in_getc, in_peek_interrupt, 0, false };

static void queue_key(uint8_t c) {
    if (g_kq_head - g_kq_tail >= KQ_SIZE) { g.dropped++; return; }
    g_kq[g_kq_head % KQ_SIZE] = c;
    g_kq_head++;
}

static void register_input(void) {
    if (g.registered) return;
    if (console_input_register(&g_in) != 0) {
        printk("[CardKB2] the console has no room for another input source\n");
        return;
    }
    g.registered = true;
}

#if defined(CONFIG_CARDKB2_RX_GPIO)
/* ---- UART transport: UART1, receive only -------------------------------------------------
 *
 * Registers from IDF's soc/esp32c6/register/soc/uart_reg.h and pcr_reg.h; the matrix signal
 * U1RXD_IN is 9 (gpio_sig_map.h). Clock: the 40 MHz XTAL source, divider 347.25 for 115200
 * (40e6 / 115200 = 347.22; 40e6 / 347.25 = 115,190 baud, 0.01 % off). The pad gets the weak
 * pull-up so a pin with no unit reads idle (high) instead of floating into noise.
 * Nothing is transmitted and no TX pin is configured: the unit's white wire may be left
 * unconnected. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define U1_BASE            0x60001000UL
#define U1_FIFO            (U1_BASE + 0x00)
#define U1_INT_CLR         (U1_BASE + 0x10)
#define U1_CLKDIV          (U1_BASE + 0x14)     /* integer 11:0, fraction 23:20 */
#define U1_STATUS          (U1_BASE + 0x1c)     /* RXFIFO_CNT 7:0 */
#define U1_CONF0           (U1_BASE + 0x20)     /* 8N1 is the reset value; bit 21 clock on, bit 22 RXFIFO_RST */
#define U1_CLK_CONF        (U1_BASE + 0x88)     /* bits 24/25: TX/RX source clock enable */
#define U1_REG_UPDATE      (U1_BASE + 0x98)

#define PCR_UART1_CONF     0x6009600CUL          /* bit 0 clock, bit 1 reset */
#define PCR_UART1_SCLK     0x60096010UL          /* [21:20] source (3 = XTAL), bit 22 enable, divider [19:12] */

#define GPIO_BASE          0x60091000UL
#define GPIO_ENABLE_W1TC   (GPIO_BASE + 0x28)
#define GPIO_IN_SEL(sig)   (GPIO_BASE + 0x154u + 4u * (sig))
#define IOMUX_PAD(n)       (0x60090000UL + 0x4 + 4u * (n))
#define U1RXD_IN_SIG       9u

static void u1_update(void) {
    REG(U1_REG_UPDATE) = 1u;
    for (unsigned n = 0; (REG(U1_REG_UPDATE) & 1u) && n < 100000u; n++) { }
}

static void uart_bringup(void) {
    REG(PCR_UART1_CONF) |= 1u;                                   /* clock on */
    REG(PCR_UART1_CONF) |= 2u;                                   /* pulse reset */
    for (volatile unsigned i = 0; i < 100u; i++) { }
    REG(PCR_UART1_CONF) &= ~2u;
    REG(PCR_UART1_SCLK) = (3u << 20) | (1u << 22);               /* XTAL, enabled, divider 1 */
    REG(U1_CLK_CONF) |= (1u << 24) | (1u << 25);
    REG(U1_CONF0) |= (1u << 21);                                 /* register clock always on */
    REG(U1_CLKDIV) = (REG(U1_CLKDIV) & ~((0xFFFu) | (0xFu << 20))) | 347u | (4u << 20);
    u1_update();
    REG(U1_CONF0) |= (1u << 22);                                 /* flush the RX FIFO */
    u1_update();
    REG(U1_CONF0) &= ~(1u << 22);
    u1_update();
    REG(U1_INT_CLR) = 0xFFFFFFFFu;

    uint32_t gpio = CONFIG_CARDKB2_RX_GPIO;
    uint32_t pad = REG(IOMUX_PAD(gpio));
    pad &= ~(7u << 12);
    REG(IOMUX_PAD(gpio)) = pad | (1u << 12) | (1u << 9) | (1u << 8);   /* GPIO function, input enable, pull-up */
    REG(GPIO_ENABLE_W1TC) = (1u << gpio);                        /* an input */
    REG(GPIO_IN_SEL(U1RXD_IN_SIG)) = gpio | (1u << 7);           /* bit 7: take it from the matrix */
}

/* Counts transitions on the RX pin for `ms` milliseconds, to tell a silent line from a unit that
 * talks but is not heard (a wrong pin, a wrong mode). Polls the pad directly, in the caller's task. */
void cardkb2_scope(uint32_t ms) {
    uint32_t pin = CONFIG_CARDKB2_RX_GPIO, edges = 0, high = 0, n = 0;
    /* Is something driving the pin? Swap the pull-up for the pad's pull-down (IO_MUX bit 7) for a
     * moment: a driven-high line stays high, a floating or unconnected one falls. */
    uint32_t pad = REG(IOMUX_PAD(pin));
    REG(IOMUX_PAD(pin)) = (pad & ~(1u << 8)) | (1u << 7);
    time_delay_us(2000);
    uint32_t held = (REG(GPIO_BASE + 0x3cu) >> pin) & 1u;
    REG(IOMUX_PAD(pin)) = pad;
    cprintf("cardkb scope: with the pull-down on, GPIO%u reads %s\n", (unsigned)pin,
            held ? "HIGH (something drives it)" : "LOW (nothing drives it: unconnected, or the unit is not transmitting)");
    uint32_t last = (REG(GPIO_BASE + 0x3cu) >> pin) & 1u;
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) {
        if (console_interrupt_requested()) {
            console_interrupt_clear();
            cprintf("cardkb scope: interrupted\n");
            break;
        }
        for (unsigned i = 0; i < 2000u; i++) {
            uint32_t v = (REG(GPIO_BASE + 0x3cu) >> pin) & 1u;
            if (v != last) { edges++; last = v; }
            high += v; n++;
        }
        sched_yield();
    }
    cprintf("cardkb scope: GPIO%u, %lu edges in %lu ms, high %lu of %lu samples; UART1 in_sel(9)=0x%08lx rxcnt=%lu int_raw=0x%lx\n",
            (unsigned)pin, (unsigned long)edges, (unsigned long)ms, (unsigned long)high, (unsigned long)n,
            (unsigned long)REG(GPIO_IN_SEL(U1RXD_IN_SIG)), (unsigned long)(REG(U1_STATUS) & 0xFFu),
            (unsigned long)REG(U1_BASE + 0x04));
}

static bool uart_get(uint8_t *b) {
    if ((REG(U1_STATUS) & 0xFFu) == 0u) return false;
    *b = (uint8_t)(REG(U1_FIFO) & 0xFFu);
    return true;
}

static cardkb2_keys_t g_keys;   /* single task: cardkb2 (bad_packets is read racily by `cardkb`, accepted) */

static void poll_body(void *arg) {
    (void)arg;
    cardkb2_keys_t *keys = &g_keys;
    cardkb2_keys_reset(keys);
    for (;;) {
        uint8_t b;
        while (uart_get(&b)) {
            cardkb2_event_t ev;
            if (!cardkb2_pkt_feed(keys, b, &ev)) continue;
            g.packets++;
            g.last_id = ev.id;
            uint8_t out[4];
            uint32_t n = cardkb2_keys_event(keys, &ev, (uint32_t)time_get_ms(), out, sizeof(out));
            for (uint32_t i = 0; i < n; i++) queue_key(out[i]);
            if (n) { g.keys++; g.last_key = out[0]; }
        }
        g.detected = g.packets != 0u;
        task_sleep_ms(5u);
    }
}

bool cardkb2_start(void) {
    if (!g.task_started) {
        uart_bringup();
        register_input();
        if (task_create_sized("cardkb2", poll_body, NULL, 2) >= 0) g.task_started = true;
        printk("[CardKB2] listening on UART1 RX GPIO%u (115200 8N1; the unit must be in UART mode: Fn+Sym+2, RST).\n",
               (unsigned)CONFIG_CARDKB2_RX_GPIO);
    }
    return g.task_started;
}

void cardkb2_print_status(void) {
    cprintf("cardkb2: UART1 RX on GPIO%u, %s; %lu packets (last key id %lu), %lu keys out (last 0x%02x), "
            "%lu bad bytes/packets, %lu dropped\n",
            (unsigned)CONFIG_CARDKB2_RX_GPIO,
            g.packets ? "unit heard" : "nothing heard yet (is it in UART mode? press a key)",
            (unsigned long)g.packets, (unsigned long)g.last_id, (unsigned long)g.keys, g.last_key,
            (unsigned long)g_keys.bad_packets, (unsigned long)g.dropped);
}

#else
void cardkb2_scope(uint32_t ms) { (void)ms; cprintf("cardkb scope: only for the UART transport\n"); }
/* ---- I2C transport: the unit's factory mode ------------------------------------------------
 * A read returns one byte: 0 for no key, else the ASCII of a key press (auto-repeat after
 * 300 ms at 50 ms steps); register 0xF1 is the firmware version. Backspace (0x08) becomes DEL
 * as the USB keyboard sends it; values no key produces (the reference driver saw a stray 0x01)
 * are dropped. The unit sends no cursor keys or Ctrl in this mode.
 */
#define POLL_MS         15u
#define ABSENT_AFTER    100u    /* failed reads in a row (1.5 s) before the unit counts as unplugged */
#define RETRY_MS        1000u
#define REG_FIRMWARE    0xF1u

static uint8_t i2c_translate(uint8_t raw) {
    if (raw == 0x08u) return 0x7Fu;
    if (raw == 0x0Au) return '\r';
    if (raw == 0x0Du || raw == 0x1Bu) return raw;
    if (raw >= 0x20u && raw <= 0x7Eu) return raw;
    return 0;
}

/* A real unit answers the firmware register with a version (0x01 on the one tested). An address that
 * only ACKs, a floating or loaded bus, reads back all ones (or zeros) there, so those and a reading that
 * changes between two tries are not a keyboard; without this a phantom ACK at 0x5F types noise. */
static bool probe(void) {
    uint8_t b, fw, fw2;
    uint8_t reg = REG_FIRMWARE;
    if (!i2c_xfer(CARDKB2_ADDR, NULL, 0u, &b, 1u)) return false;
    if (!i2c_xfer(CARDKB2_ADDR, &reg, 1u, &fw, 1u)) return false;
    if (fw == 0x00u || fw == 0xFFu) return false;
    if (!i2c_xfer(CARDKB2_ADDR, &reg, 1u, &fw2, 1u) || fw2 != fw) return false;
    g.firmware = fw;
    return true;
}

static void poll_body(void *arg) {
    (void)arg;
    uint32_t fails = 0;
    for (;;) {
        if (!g.detected) {
            task_sleep_ms(RETRY_MS);
            if (probe()) {
                g.detected = true;
                fails = 0;
                printk("[CardKB2] keyboard at 0x%02x, firmware 0x%02x.\n", CARDKB2_ADDR, g.firmware);
            }
            continue;
        }
        uint8_t raw = 0;
        if (!i2c_xfer(CARDKB2_ADDR, NULL, 0u, &raw, 1u)) {
            g.i2c_read_fail++;
            if (++fails >= ABSENT_AFTER) {
                g.detected = false;
                printk("[CardKB2] keyboard gone.\n");
            }
        } else {
            fails = 0;
            if (raw) {
                uint8_t c = i2c_translate(raw);
                if (c) { queue_key(c); g.last_key = raw; g.keys++; }
                else g.i2c_filtered++;
            }
        }
        task_sleep_ms(POLL_MS);
    }
}

bool cardkb2_start(void) {
    if (!g.task_started && !probe()) return false;
    if (!g.detected) {
        g.detected = true;
        printk("[CardKB2] keyboard at 0x%02x, firmware 0x%02x.\n", CARDKB2_ADDR, g.firmware);
    }
    register_input();
    if (!g.task_started) {
        if (task_create_sized("cardkb2", poll_body, NULL, 2) >= 0) g.task_started = true;
    }
    return true;
}

void cardkb2_print_status(void) {
    if (!g.task_started && !g.detected) {
        cprintf("cardkb2: no keyboard found over I2C (it must be in I2C mode: Fn+Sym+1, then RST)\n");
        return;
    }
    cprintf("cardkb2: I2C %s at 0x%02x, firmware 0x%02x; %lu keys (last 0x%02x), %lu filtered, %lu dropped, %lu read failures\n",
            g.detected ? "present" : "not answering", CARDKB2_ADDR, g.firmware, (unsigned long)g.keys,
            g.last_key, (unsigned long)g.i2c_filtered, (unsigned long)g.dropped, (unsigned long)g.i2c_read_fail);
}
#endif
