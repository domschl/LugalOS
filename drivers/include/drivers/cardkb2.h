#ifndef DRIVERS_CARDKB2_H
#define DRIVERS_CARDKB2_H

#include <stdint.h>
#include <stdbool.h>

/* M5Stack CardKB2 (SKU U215): a 42-key QWERTY keyboard, Grove connector GND, 5V, G26 (yellow),
 * G25 (white).
 *
 * UART mode (Fn+Sym+2, then the RST button; the unit remembers): 115200 8N1, the unit sends
 * only, one 5-byte packet per key press and release -- {0xAA, 0x03, key_id, state, checksum},
 * state 1 pressed / 2 released, checksum = (0x03 + key_id + state) & 0xFF -- and repeats the
 * press packet every 50 ms after 300 ms. On the unit tested here its TX was the yellow wire
 * (G26), the opposite of M5's page, which labels G26 "RX" (measured: with the host's pull-down
 * on, only the yellow wire held the pin high; `cardkb scope`). The host gets raw key events, including the Aa,
 * Fn and Sym keys themselves, so the layout is ours (cardkb2_keys.c).
 *
 * I2C mode (Fn+Sym+1, the factory default): 0x5F, a read returns one finished ASCII byte.
 * The unit sends no cursor keys or Ctrl there. Used on a board without CONFIG_CARDKB2_RX_GPIO.
 *
 * Board facts: CONFIG_CARDKB2_RX_GPIO names the pin the unit's TX is wired to (UART1's RX).
 * Nothing is driven on any pin; the unit's other data wire (white) can stay unconnected.
 */

#define CARDKB2_ADDR  0x5Fu

bool     cardkb2_start(void);
bool     cardkb2_is_detected(void);
uint8_t  cardkb2_firmware(void);

/* --- The key logic, shared by both transports and testable without a unit --- */

#define CARDKB2_KEY_COUNT  43u
#define CARDKB2_KEY_AA     22u
#define CARDKB2_KEY_FN     33u
#define CARDKB2_KEY_SYM    34u

typedef struct { uint8_t id; bool pressed; } cardkb2_event_t;

typedef struct {
    /* packet framing */
    uint8_t  pkt[5];
    uint8_t  pkt_n;
    uint32_t bad_packets;
    /* layout state */
    bool     fn, sym_held, sym_latch, sym_clean;
    bool     aa_held, aa_clean, aa_oneshot, aa_lock;
    uint32_t aa_last_tap_ms;
    uint64_t down;                  /* keys currently held, one bit per id */
} cardkb2_keys_t;

void cardkb2_keys_reset(cardkb2_keys_t *k);

/* Feeds one byte of the UART stream; true when it completed a valid packet (in *ev).
 * A bad checksum or a stray byte resynchronizes on the next 0xAA and counts in bad_packets. */
bool cardkb2_pkt_feed(cardkb2_keys_t *k, uint8_t b, cardkb2_event_t *ev);

/* Applies a key event; writes the console bytes it produces (at most 4) into out. Layers:
 *   plain                letters, digits, space, Enter (CR), Backspace (DEL)
 *   Aa tapped            the next letter is a capital; tapped twice, caps lock; held, shifted
 *   Sym held or latched  punctuation as printed (a tap toggles the latch)
 *   Fn + Z X C D         cursor left, down, right, up (VT sequences)
 *   Fn + 1               ESC           Fn + Backspace   forward delete
 *   Fn + Sym + letter    Ctrl-letter */
uint32_t cardkb2_keys_event(cardkb2_keys_t *k, const cardkb2_event_t *ev, uint32_t now_ms,
                            uint8_t *out, uint32_t cap);

/* Counts level changes on the RX pin for `ms` ms (diagnostic; UART transport only). */
void     cardkb2_scope(uint32_t ms);

uint32_t cardkb2_selftest(bool report);
void     cardkb2_print_status(void);

#endif /* DRIVERS_CARDKB2_H */
