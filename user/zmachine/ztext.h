/* user/zmachine/ztext.h — Z-character string output for Version 3
 *
 * Decoding model per z-spec10.pdf §3 (authoritative) cross-checked
 * against frotz's decode_text():
 *
 *   z-char 0      space (any alphabet)
 *   z-chars 1,2,3 abbreviation: entry 32(z-1)+x in the table at
 *                 header $18 (x = the FOLLOWING z-character)
 *   z-char 4      shift to A1 for the next character only
 *   z-char 5      shift to A2 for the next character only
 *   z-char 6 in A2  ZSCII: next two z-chars are a 10-bit code
 *   z-char 7 in A2  line break (Versions 2+)
 *   z-chars 6..31  alphabet lookup: A0 lower, A1 upper, A2 punct
 *
 * Version 3 has no shift-lock and no custom alphabet table
 * (z-spec10 §3.2.3, §835 "under Versions 1 to 4 the default table is
 * always used").
 */
#ifndef USER_ZMACHINE_ZTEXT_H
#define USER_ZMACHINE_ZTEXT_H

#include "zvm.h"

/* Print the Z-encoded string starting at the given BYTE address. */
void ztext_print(struct z_vm *vm, uint32_t byte_addr);

/* Output one ZSCII character (ASCII/Latin-1 passthrough, 13 = newline). */
void ztext_put_zscii(struct z_vm *vm, uint16_t zscii);

#endif /* USER_ZMACHINE_ZTEXT_H */
