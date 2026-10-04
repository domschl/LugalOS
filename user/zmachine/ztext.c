/* user/zmachine/ztext.c — Z-character string output for Version 3
 *
 * Freestanding: memory access through zmem inline helpers, output
 * through the vm io.putc callback only.
 */
#include "ztext.h"

#define ZTEXT_ABBREV_MAX_DEPTH 8u

/* Default A2 (punctuation) alphabet, Versions 2-3 (frotz parity). */
static const char z_alph_a2[] =
    " ^0123456789.,!?_#'\"/\\-:()";  /* 26 characters + NUL */

static void z_putc(struct z_vm *vm, uint8_t ch)
{
    if (vm->io.putc != NULL) {
        vm->io.putc(vm->io.user, ch);
    }
}

void ztext_put_zscii(struct z_vm *vm, uint16_t zscii)
{
    if (zscii == 13u || zscii == 11u) {
        z_putc(vm, '\n');
    } else if (zscii >= 32u && zscii <= 126u) {
        z_putc(vm, (uint8_t)zscii);
    } else if (zscii >= 155u && zscii <= 250u) {
        /* ZSCII 155-250 map one-to-one onto ISO Latin-1 155-250. */
        z_putc(vm, (uint8_t)zscii);
    }
    /* Other ZSCII codes (9 = tab, 151-154 accents, ...) are dropped
     * silently on the byte-stream console. */
}

static uint8_t z_alphabet_char(int alphabet, int index)
{
    if (alphabet == 0) {
        return (uint8_t)('a' + index);
    }
    if (alphabet == 1) {
        return (uint8_t)('A' + index);
    }
    return (uint8_t)z_alph_a2[index];
}

/* Decode and print one Z-string.  Abbreviations recurse; the spec
 * forbids an abbreviation string from using abbreviations itself, but
 * a depth guard keeps corrupt data from looping. */
static void z_decode_text(struct z_vm *vm, uint32_t addr, unsigned depth)
{
    /* status: 0 = normal, 1 = abbreviation index follows,
     *         2 = ext ZSCII high zchar follows,
     *         3 = ext ZSCII low zchar follows                */
    int status = 0;
    unsigned prev = 0;
    int pending_shift = 0;

    for (;;) {
        uint16_t code = z_read_word(&vm->m, addr);
        int sh;

        addr += 2u;
        for (sh = 10; sh >= 0; sh -= 5) {
            unsigned c = (unsigned)((code >> sh) & 0x1fu);
            int alphabet = pending_shift;
            pending_shift = 0;

            if (status == 0) {
                if (alphabet == 2 && c == 6u) {
                    status = 2;           /* 10-bit ZSCII follows */
                } else if (alphabet == 2 && c == 7u) {
                    z_putc(vm, '\n');     /* line break (v2+)     */
                } else if (c >= 6u) {
                    ztext_put_zscii(vm,
                                    z_alphabet_char(alphabet, (int)c - 6));
                } else if (c == 0u) {
                    z_putc(vm, ' ');
                } else if (c >= 4u) {
                    /* Shift characters: 4 = next char in A1,
                     * 5 = next char in A2 (v3, z-spec10 §3.2.3). */
                    pending_shift = (int)(c & 1u) + 1;
                } else if (depth < ZTEXT_ABBREV_MAX_DEPTH) {
                    /* v3: z-chars 1, 2, 3 introduce an abbreviation
                     * whose table entry is 32(z-1)+x. */
                    status = 1;
                    prev = c;
                }
            } else if (status == 1) {
                uint32_t entry = vm->m.abbrev_base + 64u * (prev - 1u)
                                 + 2u * (uint32_t)c;
                uint16_t str_word = z_read_word(&vm->m, entry);
                status = 0;
                if (str_word != 0u) {
                    /* Entries are word addresses in v1-4. */
                    z_decode_text(vm, (uint32_t)str_word * 2u, depth + 1u);
                }
            } else if (status == 2) {
                prev = c;
                status = 3;
            } else {                      /* status == 3 */
                ztext_put_zscii(vm, (uint16_t)((prev << 5) | c));
                status = 0;
            }
        }
        if ((code & 0x8000u) != 0u) {
            break;
        }
    }
}

void ztext_print(struct z_vm *vm, uint32_t byte_addr)
{
    z_decode_text(vm, byte_addr, 0u);
}
