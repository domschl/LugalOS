#include "drivers/cardkb2.h"
#include "kernel/console.h"
#include <string.h>

/* CardKB2 packet framing and key layout. The key ids are the unit's scan positions; the
 * plain, shifted and symbol characters are the unit's own printed legends (the table after
 * M5Stack's M5Unit-KEYBOARD, MIT). Pure functions of their arguments, so `cardkb selftest`
 * covers them without a unit. */

#define PKT_HEADER   0xAAu
#define PKT_LEN_FLD  0x03u
#define TAP_DOUBLE_MS 400u

#define ID_1      0u
#define ID_DELETE 21u
#define ID_D      25u
#define ID_ENTER  32u
#define ID_Z      35u
#define ID_X      36u
#define ID_C      37u

/* plain, shifted, symbol per key id; 0 where the key has no character (a modifier or a gap). */
static const char k_map[CARDKB2_KEY_COUNT][3] = {
    {'1','1','!'}, {'2','2','@'}, {'3','3','#'}, {'4','4','$'}, {'5','5','%'},
    {'6','6','^'}, {'7','7','&'}, {'8','8','*'}, {'9','9','('}, {'0','0',')'},
    {0,0,0},
    {'q','Q','~'}, {'w','W','`'}, {'e','E','?'}, {'r','R','\\'}, {'t','T','/'},
    {'y','Y','|'}, {'u','U','_'}, {'i','I','-'}, {'o','O','+'}, {'p','P','='},
    {0x7F,0x7F,0x7F},                                                    /* 21 delete */
    {0,0,0},                                                             /* 22 Aa */
    {'a','A','{'}, {'s','S','}'}, {'d','D','^'}, {'f','F','['}, {'g','G',']'},
    {'h','H','"'}, {'j','J','\''}, {'k','K',';'}, {'l','L',':'},
    {'\r','\r','\r'},                                                    /* 32 enter */
    {0,0,0}, {0,0,0},                                                    /* 33 Fn, 34 Sym */
    {'z','Z','z'}, {'x','X','x'}, {'c','C','c'}, {'v','V','<'}, {'b','B','>'},
    {'n','N',','}, {'m','M','.'},
    {' ',' ',' '},
};

void cardkb2_keys_reset(cardkb2_keys_t *k) { memset(k, 0, sizeof(*k)); }

bool cardkb2_pkt_feed(cardkb2_keys_t *k, uint8_t b, cardkb2_event_t *ev) {
    if (k->pkt_n == 0u) {
        if (b == PKT_HEADER) k->pkt[k->pkt_n++] = b;
        else k->bad_packets++;
        return false;
    }
    k->pkt[k->pkt_n++] = b;
    if (k->pkt_n < 5u) return false;
    k->pkt_n = 0;
    uint8_t sum = (uint8_t)(k->pkt[1] + k->pkt[2] + k->pkt[3]);
    if (k->pkt[1] != PKT_LEN_FLD || sum != k->pkt[4] || k->pkt[2] >= CARDKB2_KEY_COUNT ||
        (k->pkt[3] != 1u && k->pkt[3] != 2u)) {
        k->bad_packets++;
        /* The bytes after a bad header may hold the next real header: rescan them. */
        uint8_t tail[4];
        memcpy(tail, &k->pkt[1], 4);
        for (uint32_t i = 0; i < 4u; i++) {
            if (tail[i] == PKT_HEADER) {
                k->pkt_n = 0;
                for (uint32_t j = i; j < 4u; j++) k->pkt[k->pkt_n++] = tail[j];
                break;
            }
        }
        return false;
    }
    ev->id = k->pkt[2];
    ev->pressed = (k->pkt[3] == 1u);
    return true;
}

static bool sym_active(const cardkb2_keys_t *k) { return k->sym_held || k->sym_latch; }
static bool shift_active(const cardkb2_keys_t *k) { return k->aa_held || k->aa_oneshot || k->aa_lock; }

static uint32_t put_seq(uint8_t *out, uint32_t cap, const char *s) {
    uint32_t n = 0;
    while (*s && n < cap) out[n++] = (uint8_t)*s++;
    return n;
}

uint32_t cardkb2_keys_event(cardkb2_keys_t *k, const cardkb2_event_t *ev, uint32_t now_ms,
                            uint8_t *out, uint32_t cap) {
    uint8_t id = ev->id;
    if (id >= CARDKB2_KEY_COUNT) return 0;
    uint64_t bit = 1ull << id;
    bool repeat = ev->pressed && (k->down & bit);       /* the unit re-sends a held key's press */
    if (ev->pressed) k->down |= bit; else k->down &= ~bit;

    if (id == CARDKB2_KEY_FN) { k->fn = ev->pressed; return 0; }

    if (id == CARDKB2_KEY_SYM) {
        if (ev->pressed && !repeat) { k->sym_held = true; k->sym_clean = true; }
        else if (!ev->pressed) {
            k->sym_held = false;
            if (k->sym_clean) k->sym_latch = !k->sym_latch;     /* a tap with no key between */
        }
        return 0;
    }
    if (id == CARDKB2_KEY_AA) {
        if (ev->pressed && !repeat) { k->aa_held = true; k->aa_clean = true; }
        else if (!ev->pressed) {
            k->aa_held = false;
            if (k->aa_clean) {                                  /* a tap */
                bool dbl = k->aa_oneshot && (now_ms - k->aa_last_tap_ms) <= TAP_DOUBLE_MS;
                if (k->aa_lock) { k->aa_lock = false; k->aa_oneshot = false; }
                else if (dbl) { k->aa_lock = true; k->aa_oneshot = false; }
                else k->aa_oneshot = true;
                k->aa_last_tap_ms = now_ms;
            }
        }
        return 0;
    }

    if (!ev->pressed || cap == 0u) return 0;
    if (k->sym_held) k->sym_clean = false;
    if (k->aa_held) k->aa_clean = false;

    if (k->fn) {
        if (sym_active(k)) {                                    /* Ctrl */
            char c = k_map[id][0];
            if (c >= 'a' && c <= 'z') { out[0] = (uint8_t)(c - 'a' + 1); return 1; }
            return 0;
        }
        switch (id) {
        case ID_1:      out[0] = 0x1Bu; return 1;
        case ID_Z:      return put_seq(out, cap, "\x1b[D");
        case ID_X:      return put_seq(out, cap, "\x1b[B");
        case ID_C:      return put_seq(out, cap, "\x1b[C");
        case ID_D:      return put_seq(out, cap, "\x1b[A");
        case ID_DELETE: return put_seq(out, cap, "\x1b[3~");
        default:        return 0;
        }
    }

    char c = sym_active(k) ? k_map[id][2] : (shift_active(k) ? k_map[id][1] : k_map[id][0]);
    if (!c) return 0;
    if (!sym_active(k) && k->aa_oneshot && !k->aa_lock && c >= 'A' && c <= 'Z') k->aa_oneshot = false;
    out[0] = (uint8_t)c;
    return 1;
}

static void send(cardkb2_keys_t *k, uint8_t id, bool pressed, uint32_t t) {
    uint8_t o[4];
    cardkb2_event_t e = { id, pressed };
    (void)cardkb2_keys_event(k, &e, t, o, 4u);
}

/* One press and release; the bytes the press produced. */
static uint32_t tap(cardkb2_keys_t *k, uint8_t id, uint32_t t, uint8_t *out) {
    cardkb2_event_t e = { id, true };
    uint32_t n = cardkb2_keys_event(k, &e, t, out, 4u);
    send(k, id, false, t);
    return n;
}

uint32_t cardkb2_selftest(bool report) {
    uint32_t failed = 0;
#define CHECK(cond, what) do { if (!(cond)) { failed++; if (report) cprintf("  cardkb2: %s\n", what); } } while (0)
    cardkb2_keys_t k;
    uint8_t o[4];
    cardkb2_event_t ev;

    /* Packets: a valid one, a bad checksum, garbage before a header, a bad packet hiding a good one. */
    cardkb2_keys_reset(&k);
    static const uint8_t good[5] = { 0xAA, 0x03, 0x17, 0x01, 0x1B };   /* 'a' pressed */
    bool got = false;
    for (uint32_t i = 0; i < 5u; i++) got = cardkb2_pkt_feed(&k, good[i], &ev);
    CHECK(got && ev.id == 0x17 && ev.pressed, "valid packet");
    got = false;
    static const uint8_t bad[5] = { 0xAA, 0x03, 0x17, 0x01, 0x00 };
    for (uint32_t i = 0; i < 5u; i++) got |= cardkb2_pkt_feed(&k, bad[i], &ev);
    CHECK(!got && k.bad_packets == 1u, "bad checksum rejected");
    static const uint8_t noisy[8] = { 0x00, 0x55, 0xAA, 0x03, 0x17, 0x02, 0x1C, 0x00 };
    got = false;
    for (uint32_t i = 0; i < 8u; i++) got |= cardkb2_pkt_feed(&k, noisy[i], &ev);
    CHECK(got && ev.id == 0x17 && !ev.pressed, "resync after noise");
    static const uint8_t hidden[9] = { 0xAA, 0x03, 0xAA, 0x03, 0x17, 0x01, 0x1B, 0x00, 0x00 };
    got = false;
    for (uint32_t i = 0; i < 9u; i++) got |= cardkb2_pkt_feed(&k, hidden[i], &ev);
    CHECK(got && ev.id == 0x17, "header inside a bad packet");

    /* Layout. */
    cardkb2_keys_reset(&k);
    CHECK(tap(&k, 23, 0, o) == 1 && o[0] == 'a', "plain a");
    CHECK(tap(&k, ID_ENTER, 0, o) == 1 && o[0] == '\r', "enter is CR");
    CHECK(tap(&k, ID_DELETE, 0, o) == 1 && o[0] == 0x7F, "backspace is DEL");

    send(&k, CARDKB2_KEY_AA, true, 100); send(&k, CARDKB2_KEY_AA, false, 150);
    CHECK(tap(&k, 23, 200, o) == 1 && o[0] == 'A', "Aa tap: next letter capital");
    CHECK(tap(&k, 23, 300, o) == 1 && o[0] == 'a', "Aa tap: only one letter");

    send(&k, CARDKB2_KEY_AA, true, 1000); send(&k, CARDKB2_KEY_AA, false, 1050);
    send(&k, CARDKB2_KEY_AA, true, 1100); send(&k, CARDKB2_KEY_AA, false, 1150);
    CHECK(tap(&k, 23, 1200, o) == 1 && o[0] == 'A' && tap(&k, 24, 1300, o) == 1 && o[0] == 'S', "Aa double tap: lock");
    send(&k, CARDKB2_KEY_AA, true, 2000); send(&k, CARDKB2_KEY_AA, false, 2050);
    CHECK(tap(&k, 23, 2100, o) == 1 && o[0] == 'a', "Aa tap unlocks");

    send(&k, CARDKB2_KEY_AA, true, 3000);
    CHECK(tap(&k, 23, 3010, o) == 1 && o[0] == 'A', "Aa held: shifted");
    send(&k, CARDKB2_KEY_AA, false, 3100);
    CHECK(tap(&k, 23, 3200, o) == 1 && o[0] == 'a', "Aa held with a key: no latch left");

    send(&k, CARDKB2_KEY_SYM, true, 4000);
    CHECK(tap(&k, 11, 4010, o) == 1 && o[0] == '~', "Sym held: ~");
    send(&k, CARDKB2_KEY_SYM, false, 4100);
    CHECK(tap(&k, 11, 4200, o) == 1 && o[0] == 'q', "Sym held with a key: no latch");
    send(&k, CARDKB2_KEY_SYM, true, 5000); send(&k, CARDKB2_KEY_SYM, false, 5050);
    CHECK(tap(&k, 26, 5100, o) == 1 && o[0] == '[', "Sym tap: latched");
    send(&k, CARDKB2_KEY_SYM, true, 5200); send(&k, CARDKB2_KEY_SYM, false, 5250);
    CHECK(tap(&k, 26, 5300, o) == 1 && o[0] == 'f', "Sym tap again: unlatched");

    send(&k, CARDKB2_KEY_FN, true, 6000);
    CHECK(tap(&k, ID_Z, 6010, o) == 3 && memcmp(o, "\x1b[D", 3) == 0, "Fn+Z left");
    CHECK(tap(&k, ID_X, 6020, o) == 3 && memcmp(o, "\x1b[B", 3) == 0, "Fn+X down");
    CHECK(tap(&k, ID_C, 6030, o) == 3 && memcmp(o, "\x1b[C", 3) == 0, "Fn+C right");
    CHECK(tap(&k, ID_D, 6040, o) == 3 && memcmp(o, "\x1b[A", 3) == 0, "Fn+D up");
    CHECK(tap(&k, ID_1, 6050, o) == 1 && o[0] == 0x1B, "Fn+1 ESC");
    CHECK(tap(&k, ID_DELETE, 6060, o) == 4 && memcmp(o, "\x1b[3~", 4) == 0, "Fn+Backspace forward delete");
    CHECK(tap(&k, 23, 6070, o) == 0, "Fn+A nothing");
    send(&k, CARDKB2_KEY_SYM, true, 6100);
    CHECK(tap(&k, ID_C, 6110, o) == 1 && o[0] == 0x03, "Fn+Sym+C is Ctrl-C");
    CHECK(tap(&k, 23, 6120, o) == 1 && o[0] == 0x01, "Fn+Sym+A is Ctrl-A");
    send(&k, CARDKB2_KEY_SYM, false, 6200);
    send(&k, CARDKB2_KEY_FN, false, 6300);
    CHECK(tap(&k, 23, 6400, o) == 1 && o[0] == 'a', "layers clear after release");
#undef CHECK
    if (report)
        cprintf("cardkb2 selftest: %lu case%s failed\n", (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}
