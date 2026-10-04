/* user/zmachine/zparse.h — aread (v3 READ): line input + dictionary tokenize
 *
 * v3 semantics (z-spec10.pdf read/sread; frotz z_read + tokenise_line):
 *
 * Text buffer: byte 0 = max typeable length (v1-4 store n and accept
 * n-1... byte 0 holds "max minus 1"; frotz accepts byte0-1 characters);
 * input stored lower-case at byte 1+, NUL-terminated.
 *
 * Parse buffer: byte 0 = max words; byte 1 = number of words parsed;
 * 4-byte entries from byte 2: [word dictionary BYTE address (0 if
 * unknown): 2 bytes][char count][offset in text buffer].
 *
 * Word encoding for lookup: 6 Z-characters = 2 16-bit words, 15 bits
 * used (3 zchars each), bit 15 SET on the final word (frotz
 * encode_text; explains the dictionary entries' odd-looking byte
 * patterns); pad character is Z-char 5.  Uppercase/digits/punctuation
 * carry shift zchars 4 (A1) / 5 (A2) per the v3 alphabet rules.
 */
#ifndef USER_ZMACHINE_ZPARSE_H
#define USER_ZMACHINE_ZPARSE_H

#include "zvm.h"

/* Perform the Version 3 aread (VAR $4) at the given text/parse buffer
 * addresses.  Returns ZVM_OK, or a ZVM_STOP_* code on I/O trouble. */
int zvm_aread(struct z_vm *vm, uint16_t text_buf, uint16_t parse_buf);

#endif /* USER_ZMACHINE_ZPARSE_H */
