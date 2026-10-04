/* user/zmachine/zmem.c — Z-machine memory and header model (Version 3)
 *
 * See zmem.h for the interface notes.  Freestanding: no libc calls,
 * the copy loop is open-coded so this file compiles identically for
 * the host harness and for LugalOS firmware.
 */
#include "zmem.h"

/* Currently the interpreter core implements Version 3 only (the version
 * of zork1.zip).  Extend this gate when newer opcode sets are added. */
#define Z_SUPPORTED_VERSION 3

/* Story files may not exceed the 18-bit Z-machine address space, and
 * must fit the caller-provided buffer as well. */
#define Z_MAX_ADDRESS_SPACE 0x20000u

int z_load(struct z_machine *zm, const uint8_t *image, uint32_t image_len)
{
    uint32_t i;
    uint8_t version;
    uint32_t file_len;

    if (zm == NULL || image == NULL || zm->mem == NULL) {
        return Z_ERR_NULL;
    }
    if (image_len <= ZH_CHECKSUM) {
        return Z_ERR_TOO_SMALL;
    }

    version = image[ZH_VERSION];
    if (version != Z_SUPPORTED_VERSION) {
        return Z_ERR_VERSION;
    }

    /* Logical story length: word at $1a, stored divided by two.  It may
     * legitimately disagree with the physical file size (overlay builds
     * and padded images); trust the header but never overrun inputs. */
    file_len = (uint32_t)((uint32_t)image[ZH_FILE_LEN] << 8 |
                          image[ZH_FILE_LEN + 1]) * 2u;
    if (file_len > image_len) {
        file_len = image_len;
    }
    if (file_len > zm->mem_size || file_len > Z_MAX_ADDRESS_SPACE) {
        return Z_ERR_TOO_BIG;
    }

    for (i = 0; i < file_len; i++) {
        zm->mem[i] = image[i];
    }
    /* Byte beyond the loaded image read as 0 for bounds-checked access. */

    zm->file_len = file_len;
    /* v1-4: end of dynamic memory (word $0e) serves as our static base. */
    zm->static_base =
        (uint32_t)z_read_word(zm, ZH_DYNAMIC_END);
    zm->high_base =
        (uint32_t)z_read_word(zm, ZH_HIGH_BASE) * 2u;
    zm->objects_base =
        (uint32_t)z_read_word(zm, ZH_OBJECTS_ADDR);
    zm->global_base =
        (uint32_t)z_read_word(zm, ZH_GLOBALS_ADDR);
    zm->dict_base =
        (uint32_t)z_read_word(zm, ZH_DICT_ADDR);
    zm->abbrev_base =
        (uint32_t)z_read_word(zm, ZH_ABBREV_TABLE);
    zm->checksum = z_read_word(zm, ZH_CHECKSUM);

    /* Note: we deliberately do NOT require initial_pc >= high_base.
     * The spec says execution happens in high memory, but real Infocom
     * stories interleave code below the high memory mark (z-spec10.pdf
     * §3 mentions this explicitly); zork1.zip starts executing at
     * $50d5, well below its high base of $12d50.  Addresses are flat
     * across the whole 128K space -- see the note in zmem.h. */

    return Z_OK;
}

uint16_t z_checksum(const struct z_machine *zm)
{
    uint32_t sum = 0;
    uint32_t addr;

    for (addr = 0x40; addr < zm->file_len; addr++) {
        sum += zm->mem[addr];
    }
    return (uint16_t)(sum & 0xffffu);
}
