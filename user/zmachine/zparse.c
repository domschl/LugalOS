/* user/zmachine/zparse.c — aread (v3 READ): line input + dictionary tokenize
 *
 * Freestanding core: keyboard via vm->io.getc, echo via vm->io.putc
 * (the host harness relies on terminal echo; getc is the authoritative
 * source either way).
 */
#include "zparse.h"

/* Default v3 alphabets (must mirror ztext.c; A2 shared table). */
static const char z_a2[] = " ^0123456789.,!?_#'\"/\\-:()";

#define Z_LINE_MAX 180

/* Locate a printable character in the three default alphabets.
 * Returns set (0/1/2) and index, or -1.  Mirrors frotz alphabet(). */
static int z_find_alpha(char c, int *index_out)
{
    int set, i;

    for (set = 0; set < 3; set++) {
        for (i = 0; i < 26; i++) {
            char probe;
            if (set == 0) {
                probe = (char)('a' + i);
            } else if (set == 1) {
                probe = (char)('A' + i);
            } else {
                probe = z_a2[i];
            }
            if (c == probe) {
                *index_out = i;
                return set;
            }
        }
    }
    return -1;
}

/* Encode up to 6 Z-characters of a word from the text buffer; returns
 * the number of zchars produced (max 6).  Alphabet characters carry a
 * shift zchar (4 = A1, 5 = A2) when not in A0; characters absent from
 * the alphabets are encoded as the 10-bit extended form. */
static int z_encode_word(const uint8_t *text, int length, uint16_t out[2])
{
    uint8_t zc[6];
    int n = 0;
    int i;

    for (i = 0; i < length && n < 6; i++) {
        char c = (char)text[i];
        int index;
        int set = z_find_alpha(c, &index);

        if (set < 0) {
            /* Not representable: encode its ZSCII in extended form.
             * (ASCII input on the host always hits an alphabet.) */
            if (n + 4 <= 6) {
                zc[n++] = 5;
                zc[n++] = 6;
                zc[n++] = (uint8_t)((uint8_t)c >> 5);
                zc[n++] = (uint8_t)((uint8_t)c & 0x1f);
            } else {
                n = 6;            /* truncate */
            }
            continue;
        }
        if (set != 0) {
            zc[n++] = (uint8_t)(3 + set);   /* v3: 4 = A1, 5 = A2 */
        }
        zc[n++] = (uint8_t)(index + 6);
    }
    while (n < 6) {
        zc[n++] = 5;                        /* pad character */
    }

    out[0] = (uint16_t)((zc[0] << 10) | (zc[1] << 5) | zc[2]);
    out[1] = (uint16_t)(((zc[3] << 10) | (zc[4] << 5) | zc[5]) | 0x8000u);
    return 6;
}

/* Binary-search the main dictionary for the encoded word; returns the
 * ENTRY byte address (not just the text field), or 0 if not present.
 * A negative entry count marks an unsorted dictionary (linear scan). */
static uint16_t z_dict_lookup(struct z_vm *vm, const uint16_t word[2])
{
    uint32_t dct = vm->m.dict_base;
    uint8_t sep_count = z_read_byte(&vm->m, dct);
    uint8_t entry_len;
    uint16_t entry_count;
    int32_t count_signed;
    int32_t lower = 0;
    int32_t upper;
    int sorted;

    dct += 1u + sep_count;
    entry_len = z_read_byte(&vm->m, dct);
    dct += 1u;
    entry_count = z_read_word(&vm->m, dct);
    dct += 2u;

    count_signed = (int16_t)entry_count;    /* sign marks sort order */
    if (count_signed < 0) {
        sorted = 0;
        entry_count = (uint16_t)(-count_signed);
    } else {
        sorted = 1;
    }
    upper = (int32_t)entry_count - 1;

    while (lower <= upper) {
        int32_t mid = sorted ? ((lower + upper) / 2) : lower;
        uint32_t entry = dct + (uint32_t)mid * entry_len;
        uint16_t e0 = z_read_word(&vm->m, entry);
        uint16_t e1 = z_read_word(&vm->m, entry + 2u);
        int cmp;

        if (e0 == word[0]) {
            cmp = (e1 == word[1]) ? 0
                                  : ((e1 < word[1]) ? -1 : 1);
        } else {
            cmp = (e0 < word[0]) ? -1 : 1;
        }
        if (cmp == 0) {
            return (uint16_t)entry;
        }
        if (sorted) {
            if (cmp < 0) {
                lower = mid + 1;
            } else {
                upper = mid - 1;
            }
        } else {
            lower = mid + 1;                /* linear fallback */
        }
    }
    return 0;
}

/* Append one token entry to the parse buffer (frotz tokenise_text). */
static void z_emit_token(struct z_vm *vm, uint16_t parse_buf,
                         uint16_t addr, uint8_t length, uint8_t from)
{
    uint8_t token_max = z_read_byte(&vm->m, parse_buf);
    uint8_t token_count = z_read_byte(&vm->m, parse_buf + 1u);

    if (token_count >= token_max) {
        return;
    }
    z_write_byte(&vm->m, parse_buf + 1u, (uint8_t)(token_count + 1u));

    /* Entry = word addr, char count, text-buffer offset. */
    {
        uint32_t e = parse_buf + 2u + 4u * (uint32_t)token_count;
        z_write_word(&vm->m, e, addr);
        z_write_byte(&vm->m, e + 2u, length);
        z_write_byte(&vm->m, e + 3u, from);
    }
}

/* Tokenize the NUL-terminated text at text_buf+1 into parse_buf.
 * Separators from the dictionary header are words in their own right. */
static void z_tokenise(struct z_vm *vm, uint16_t text_buf, uint16_t parse_buf)
{
    uint32_t dct = vm->m.dict_base;
    uint8_t sep_count = z_read_byte(&vm->m, dct);
    uint32_t from;
    uint32_t i;

    z_write_byte(&vm->m, parse_buf + 1u, 0);

    i = 1u;                     /* text starts at text_buf+1 */
    from = 0;                   /* 0 = not in a word */
    for (;;) {
        uint8_t c = z_read_byte(&vm->m, text_buf + i);
        uint8_t sep_hit = 0;
        uint32_t s;

        /* Is this character a word separator? */
        for (s = 1u; s <= sep_count; s++) {
            if (z_read_byte(&vm->m, dct + s) == c && c != 0u) {
                sep_hit = 1;
                break;
            }
        }

        if (c == 0u) {
            break;
        }
        if (sep_hit != 0u) {
            /* Close any word in progress first (frotz order) */
            if (from != 0u) {
                uint16_t word[2];
                z_encode_word(&vm->m.mem[text_buf + from],
                              (int)(i - from), word);
                z_emit_token(vm, parse_buf, z_dict_lookup(vm, word),
                             (uint8_t)(i - from), (uint8_t)from);
                from = 0;
            }
            /* The separator itself is one word */
            {
                uint16_t word[2];
                z_encode_word(&vm->m.mem[text_buf + i], 1, word);
                z_emit_token(vm, parse_buf, z_dict_lookup(vm, word),
                             1, (uint8_t)i);
            }
            i++;
            continue;
        }
        if (c == ' ') {
            if (from != 0u) {
                uint16_t word[2];
                z_encode_word(&vm->m.mem[text_buf + from],
                              (int)(i - from), word);
                z_emit_token(vm, parse_buf, z_dict_lookup(vm, word),
                             (uint8_t)(i - from), (uint8_t)from);
                from = 0;
            }
            i++;
            continue;
        }
        /* ordinary word character */
        if (from == 0u) {
            from = i;
        }
        i++;
    }
    if (from != 0u) {
        uint16_t word[2];
        z_encode_word(&vm->m.mem[text_buf + from],
                      (int)(i - from), word);
        z_emit_token(vm, parse_buf, z_dict_lookup(vm, word),
                     (uint8_t)(i - from), (uint8_t)from);
    }
}

int zvm_aread(struct z_vm *vm, uint16_t text_buf, uint16_t parse_buf)
{
    uint8_t line[Z_LINE_MAX];
    uint8_t max_chars;
    int len = 0;
    int ch;

    /* v1-4: byte 0 = maximum typed length minus 1 (frotz accepts one
     * fewer: max = byte0 - 1 after its decrement). */
    max_chars = z_read_byte(&vm->m, text_buf);
    if (max_chars > 1) {
        max_chars = (uint8_t)(max_chars - 1u);
    }
    if (max_chars >= Z_LINE_MAX) {
        max_chars = Z_LINE_MAX - 1;
    }

    for (;;) {
        if (vm->io.getc == NULL) {
            return ZVM_STOP_ERROR;
        }
        ch = vm->io.getc(vm->io.user);
        if (ch < 0) {
            return ZVM_STOP_QUIT;    /* EOF: end the session cleanly */
        }
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch == 127 || ch == '\b') {          /* erase last char */
            if (len > 0) {
                len--;
                if (vm->io.putc != NULL) {
                    vm->io.putc(vm->io.user, '\b');
                    vm->io.putc(vm->io.user, ' ');
                    vm->io.putc(vm->io.user, '\b');
                }
            }
            continue;
        }
        if (len < max_chars) {
            line[len++] = (uint8_t)ch;
            if (vm->io.putc != NULL) {
                vm->io.putc(vm->io.user, (uint8_t)ch);
            }
        }
    }
    if (vm->io.putc != NULL) {
        vm->io.putc(vm->io.user, '\n');
    }

    /* Store lower-case, NUL-terminated, at text_buf+1 (zmach06e READ). */
    {
        int i;
        for (i = 0; i < len; i++) {
            uint8_t c = line[i];
            if (c >= 'A' && c <= 'Z') {
                c = (uint8_t)(c + 32);
            }
            z_write_byte(&vm->m, (uint32_t)text_buf + 1u + (uint32_t)i, c);
        }
        z_write_byte(&vm->m, (uint32_t)text_buf + 1u + (uint32_t)len, 0);
    }

    if (parse_buf != 0u) {
        z_tokenise(vm, text_buf, parse_buf);
    }
    return ZVM_OK;
}
