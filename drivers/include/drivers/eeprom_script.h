#ifndef DRIVERS_EEPROM_SCRIPT_H
#define DRIVERS_EEPROM_SCRIPT_H

#include <stdint.h>
#include <stdbool.h>

/* A small Lisp program kept in the I2C EEPROM (AT24C32, 4 KB at 0x57) and run at boot, for a node
 * with no filesystem. Layout at EEPROM offset 0:
 *
 *   0   "LUS1"                    magic
 *   4   length, u16 little endian  of the text that follows
 *   6   0x0000                     reserved
 *   8   CRC-32, u32 little endian  of the text (the zlib polynomial)
 *   12  the Lisp text, no NUL
 *
 * The text ends below EEPROM_SCRIPT_END; 0x0F00 and up hold the sensor calibration blob.
 * The text is written before the header, so a write cut short leaves no valid script. */

#define EEPROM_SCRIPT_HDR   12u
#define EEPROM_SCRIPT_END   0x0F00u
#define EEPROM_SCRIPT_MAX   (EEPROM_SCRIPT_END - EEPROM_SCRIPT_HDR)

enum {
    EEPROM_SCRIPT_NONE   = -1,   /* no magic, or a header that cannot be right */
    EEPROM_SCRIPT_BADCRC = -2,   /* a header, but the text does not match its checksum */
    EEPROM_SCRIPT_TOOBIG = -3,   /* empty, or longer than EEPROM_SCRIPT_MAX, or does not fit the buffer */
    EEPROM_SCRIPT_IO     = -4,   /* the EEPROM did not answer, or a write did not read back */
    EEPROM_SCRIPT_BADTXT = -5,   /* a byte that is not text (NUL, other control characters) */
};

/* True when the header carries the magic and a sane length (a 12-byte read; no checksum). */
bool eeprom_script_present(void);

/* Validates the stored script without holding it: its length, or a negative EEPROM_SCRIPT_*. */
int  eeprom_script_check(void);

/* Reads the script into buf (cap bytes, NUL added); its length, or a negative EEPROM_SCRIPT_*.
 * The checksum is verified. */
int  eeprom_script_read(char *buf, uint32_t cap);

/* Replaces the script: invalidates the old one, writes the text, then the header, and reads it
 * back. 0 or a negative EEPROM_SCRIPT_*. */
int  eeprom_script_write(const char *text, uint32_t len);

/* Removes the script (the magic is zeroed). */
bool eeprom_script_clear(void);

/* Staged writing for a host that sends the text in pieces: put() writes at text offset `off`,
 * commit() reads the `len` bytes back, checksums them and writes the header. Call clear() first. */
int  eeprom_script_put(uint32_t off, const uint8_t *data, uint32_t len);
int  eeprom_script_commit(uint32_t len);

/* The header, as pure functions (tests). */
void eeprom_script_header_encode(uint8_t h[EEPROM_SCRIPT_HDR], uint32_t len, uint32_t crc);
bool eeprom_script_header_decode(const uint8_t h[EEPROM_SCRIPT_HDR], uint32_t *len, uint32_t *crc);

/* Pure checks always; the write/read/corrupt/clear round trip only where the EEPROM is the
 * synthetic one (QEMU), never over a script on real hardware. */
uint32_t eeprom_script_selftest(bool report);

#endif /* DRIVERS_EEPROM_SCRIPT_H */
