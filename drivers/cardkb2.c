#include "drivers/cardkb2.h"
#include <stddef.h>
#include "drivers/i2c_bus.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

/* CardKB2 over I2C: see the header for the unit's behaviour.
 *
 * A task polls the unit every POLL_MS and queues the keys; the console pump drains the
 * queue through console_input_t. The unit reports a key once per press (plus its own
 * auto-repeat), so nothing here debounces or repeats.
 */

#define POLL_MS         15u
#define ABSENT_AFTER    100u    /* failed reads in a row (1.5 s) before the unit counts as unplugged */
#define RETRY_MS        1000u
#define KQ_SIZE         32u
#define REG_FIRMWARE    0xF1u

static struct {
    bool     detected;
    bool     registered;
    bool     task_started;
    uint8_t  firmware;
    uint8_t  last_key;
    uint8_t  last_filtered;
    uint32_t keys, dropped, filtered, read_fail;
} g;   /* single task: cardkb2 (the counters and `detected`; `cardkb` status reads them racily, accepted: display only) */

static uint8_t g_kq[KQ_SIZE];
static volatile uint32_t g_kq_head, g_kq_tail;   /* interrupt-shared: none; single producer (the task), single consumer (the console pump), indices written by their owner only */

uint8_t cardkb2_translate(uint8_t raw) {
    /* Values a key can produce: BS, LF, CR, ESC and printable ASCII. Anything else is bus noise
     * (the reference driver saw a stray 0x01 on one host). */
    if (raw == 0x08u) return 0x7Fu;
    if (raw == 0x0Au || raw == 0x0Du || raw == 0x1Bu) return raw;
    if (raw >= 0x20u && raw <= 0x7Eu) return raw;
    return 0;
}

/* The original CardKB sends 0xB4 left, 0xB5 up, 0xB6 down, 0xB7 right for the cursor keys; the
 * console wants the VT sequences the USB keyboard produces. */
const char *cardkb2_arrow(uint8_t raw) {
    switch (raw) {
    case 0xB4u: return "\x1b[D";
    case 0xB5u: return "\x1b[A";
    case 0xB6u: return "\x1b[B";
    case 0xB7u: return "\x1b[C";
    default:    return 0;
    }
}

bool    cardkb2_is_detected(void) { return g.detected; }
uint8_t cardkb2_firmware(void)    { return g.firmware; }

static bool probe(void) {
    uint8_t b, fw;
    uint8_t reg = REG_FIRMWARE;
    if (!i2c_xfer(CARDKB2_ADDR, NULL, 0u, &b, 1u)) return false;
    if (!i2c_xfer(CARDKB2_ADDR, &reg, 1u, &fw, 1u)) return false;
    g.firmware = fw;
    return true;
}

static bool in_has_char(void) { return g_kq_head != g_kq_tail; }

static int in_getc(void) {
    if (g_kq_head == g_kq_tail) return -1;
    uint8_t c = g_kq[g_kq_tail % KQ_SIZE];
    g_kq_tail++;
    return c;
}

static const console_input_t g_in = { "cardkb2", in_has_char, in_getc, 0, 0, false };

static void queue_key(uint8_t c) {
    if (g_kq_head - g_kq_tail >= KQ_SIZE) { g.dropped++; return; }
    g_kq[g_kq_head % KQ_SIZE] = c;
    g_kq_head++;
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
            g.read_fail++;
            if (++fails >= ABSENT_AFTER) {
                g.detected = false;
                printk("[CardKB2] keyboard gone.\n");
            }
        } else {
            fails = 0;
            if (raw) {
                const char *seq = cardkb2_arrow(raw);
                uint8_t c = cardkb2_translate(raw);
                if (seq) {
                    for (; *seq; seq++) queue_key((uint8_t)*seq);
                    g.last_key = raw; g.keys++;
                } else if (c) {
                    queue_key(c); g.last_key = raw; g.keys++;
                } else {
                    g.filtered++; g.last_filtered = raw;
                }
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
    if (!g.registered) {
        if (console_input_register(&g_in) != 0) {
            printk("[CardKB2] the console has no room for another input source\n");
            return true;
        }
        g.registered = true;
    }
    if (!g.task_started) {
        if (task_create_sized("cardkb2", poll_body, NULL, 2) >= 0) g.task_started = true;
    }
    return true;
}

uint32_t cardkb2_selftest(bool report) {
    uint32_t failed = 0;
    static const struct { uint8_t in, out; } t[] = {
        { 0x00, 0 }, { 0x01, 0 }, { 0x08, 0x7F }, { 0x09, 0 }, { 0x0A, 0x0A }, { 0x0D, 0x0D },
        { 0x1B, 0x1B }, { 0x1F, 0 }, { 0x20, 0x20 }, { 'a', 'a' }, { '~', '~' }, { 0x7F, 0 }, { 0xB4, 0 },
    };
    for (uint32_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (cardkb2_translate(t[i].in) != t[i].out) {
            failed++;
            if (report) cprintf("  cardkb2 translate 0x%02x: got 0x%02x, want 0x%02x\n",
                                t[i].in, cardkb2_translate(t[i].in), t[i].out);
        }
    }
    if (report)
        cprintf("cardkb2 selftest: %lu case%s failed\n", (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}

void cardkb2_print_status(void) {
    if (!g.task_started && !g.detected) {
        cprintf("cardkb2: no keyboard found (it must be in I2C mode: Fn+Sym+1, then RST)\n");
        return;
    }
    cprintf("cardkb2: %s at 0x%02x, firmware 0x%02x; %lu keys (last 0x%02x), %lu filtered (last 0x%02x), %lu dropped, %lu read failures\n",
            g.detected ? "present" : "not answering", CARDKB2_ADDR, g.firmware, (unsigned long)g.keys,
            g.last_key, (unsigned long)g.filtered, g.last_filtered, (unsigned long)g.dropped, (unsigned long)g.read_fail);
}
