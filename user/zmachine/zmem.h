/* user/zmachine/zmem.h — Z-machine memory and header model (Version 3)
 *
 * Freestanding: includes stdint.h/stddef.h only, no libc function calls.
 * All I/O in the interpreter core goes through callbacks (see zvm.h);
 * this layer is pure memory arithmetic so the same sources build for
 * the host test harness and for RV32 firmware.
 *
 * Reference: z-spec10.pdf (Andrew Plotz), sections 3 (memory), 11 (header).
 */
#ifndef USER_ZMACHINE_ZMEM_H
#define USER_ZMACHINE_ZMEM_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------
 * Error codes (negative errno-style; 0 = success)
 * ------------------------------------------------------------------------ */
#define Z_OK              0
#define Z_ERR_NULL       -1  /* null argument */
#define Z_ERR_TOO_SMALL  -2  /* story file smaller than a valid header */
#define Z_ERR_TOO_BIG    -3  /* story file larger than the provided buffer */
#define Z_ERR_VERSION    -4  /* unsupported Z-machine version */

/* ------------------------------------------------------------------------
 * Header offsets, Versions 1-4 (z-spec10.pdf §11.2)
 *
 * Values marked "/2" are stored divided by two and must be multiplied
 * by 2 to obtain the byte address.
 * ------------------------------------------------------------------------ */
/* Version 1-4 header field offsets, verified empirically against
 * zork1.zip (release 119) and frotz's H_* definitions: in v1-4 these
 * are RAW BYTE addresses (the dictionary field at $08 holds the odd
 * address $3899, proving no /2 scaling), except the high memory base
 * at $04 which is stored divided by two (zmach06e.txt §2.3, Table 3:
 * "$06 first instruction (byte address)").
 *
 * Note: z-spec10.pdf §11.2 describes $06 as an initial stack frame
 * pointer and puts the PC at $08 with a +8 bias; that is a later
 * reinterpretation.  The era-correct Infocom layout (and what frotz
 * and zmach06e.txt §2.11 do) is: PC = word $06, dictionary at $08. */
#define ZH_VERSION         0x00  /* Z-machine version number */
#define ZH_FLAGS_1         0x01  /* flags 1 (fixed width font bit is 7) */
#define ZH_RELEASE         0x02  /* release number (word) */
#define ZH_HIGH_BASE       0x04  /* high memory base, stored /2 (word) */
#define ZH_INITIAL_PC      0x06  /* first instruction, byte address (word) */
#define ZH_DICT_ADDR       0x08  /* dictionary address, byte (word) */
#define ZH_OBJECTS_ADDR    0x0A  /* object table address, byte (word) */
#define ZH_GLOBALS_ADDR    0x0C  /* globals table address, byte (word) */
#define ZH_DYNAMIC_END     0x0E  /* end of dynamic memory, byte (word) */
#define ZH_SERIAL          0x12  /* 6 ASCII bytes, serial number */
#define ZH_ABBREV_TABLE    0x18  /* abbreviations table address, byte (word) */
#define ZH_FILE_LEN        0x1A  /* story file length /2 (word) */
#define ZH_CHECKSUM        0x1C  /* checksum of file from $40 onwards (word) */
#define ZH_INTERP_NUM      0x1E  /* interpreter number (set by interpreter) */
#define ZH_INTERP_VERS     0x1F  /* interpreter version (set by interpreter) */

/* ------------------------------------------------------------------------
 * Machine state
 *
 * The interpreter keeps the whole story image below `high_base` in RAM
 * (header, dynamic and static segments); high memory beyond it is read
 * through the same buffer.  On embedded targets a future split may keep
 * static + high segments in flash XIP with only the dynamic segment in
 * RAM; the read/write API below hides that detail.
 * ------------------------------------------------------------------------ */
struct z_machine {
    uint8_t *mem;         /* base of runtime memory */
    uint32_t mem_size;    /* bytes allocated at mem */
    uint32_t file_len;    /* logical story length in bytes (from $1a * 2) */
    uint32_t static_base; /* byte address of static memory base */
    uint32_t high_base;   /* byte address of high memory base (PC wraps) */
    uint32_t objects_base;/* object table byte address (header $0c * 2) */
    uint32_t global_base; /* globals table byte address (header $0e * 2) */
    uint32_t dict_base;   /* dictionary byte address (header $08)     */
    uint32_t abbrev_base; /* abbreviations table byte address (header $18) */
    uint16_t checksum;    /* checksum field read from the header */
};

/* ------------------------------------------------------------------------
 * Load and validation
 * ------------------------------------------------------------------------ */

/* Validate the story header, copy `image` into zm->mem (which the caller
 * must provide and keep alive), and cache the header geometry.
 * Returns Z_OK or a negative error code. */
int z_load(struct z_machine *zm, const uint8_t *image, uint32_t image_len);

/* Two's-complement sum of all bytes from $0040 to file_len
 * (modulo 65536), as specified for header validation. */
uint16_t z_checksum(const struct z_machine *zm);

/* ------------------------------------------------------------------------
 * Memory access
 *
 * Reads beyond mem_size yield 0; writes beyond it are dropped.  These
 * bounds make host ASan runs and firmware behaviour agree; the Z-machine
 * itself only ever addresses 18 bits (0..0x1ffff for v1-v4).
 * ------------------------------------------------------------------------ */

static inline uint8_t z_read_byte(const struct z_machine *zm, uint32_t addr)
{
    if (addr >= zm->mem_size) {
        return 0;
    }
    return zm->mem[addr];
}

static inline uint16_t z_read_word(const struct z_machine *zm, uint32_t addr)
{
    return (uint16_t)((uint32_t)z_read_byte(zm, addr) << 8 |
                      z_read_byte(zm, addr + 1));
}

static inline void z_write_byte(struct z_machine *zm, uint32_t addr, uint8_t val)
{
    if (addr < zm->mem_size) {
        zm->mem[addr] = val;
    }
}

static inline void z_write_word(struct z_machine *zm, uint32_t addr, uint16_t val)
{
    z_write_byte(zm, addr, (uint8_t)(val >> 8));
    z_write_byte(zm, addr + 1, (uint8_t)(val & 0xff));
}

/* ------------------------------------------------------------------------
 * Header accessors
 * ------------------------------------------------------------------------ */

static inline uint8_t z_version(const struct z_machine *zm)
{
    return z_read_byte(zm, ZH_VERSION);
}

static inline uint16_t z_release(const struct z_machine *zm)
{
    return z_read_word(zm, ZH_RELEASE);
}

/* Initial program counter: in v1-4 this is simply the byte address
 * stored in the header word at $06 (zmach06e.txt §2.11 and Table 3;
 * confirmed by frotz H_START_PC=6 and zork1.zip tracing). */
static inline uint32_t z_initial_pc(const struct z_machine *zm)
{
    return z_read_word(zm, ZH_INITIAL_PC);
}

/* Copy the 6 ASCII serial-number digits into out[6], NUL-terminated
 * by the caller's buffer size (provide at least 7 bytes). */
static inline void z_serial(const struct z_machine *zm, char *out)
{
    int i;
    for (i = 0; i < 6; i++) {
        out[i] = (char)z_read_byte(zm, ZH_SERIAL + (uint32_t)i);
    }
    out[6] = '\0';
}

/* NOTE: NO program-counter folding.  Versions 1-3 address a flat
 * 128K (0x20000) byte space; the header high-base at $04 exists for
 * interpreter paging hints only.  An earlier revision of this file
 * folded PCs past $10000 back to high_base -- with a story file of
 * $15336 bytes that corrupted every routine reference above 64K
 * (observed as a bogus jump from the vocabulary search routine). */

#endif /* USER_ZMACHINE_ZMEM_H */
