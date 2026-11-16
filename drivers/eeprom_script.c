#include "drivers/eeprom_script.h"
#include "drivers/at24c32.h"
#include "drivers/i2c_bus.h"
#include "kernel/crc32.h"
#include "kernel/console.h"
#include <string.h>

/* See the header for the layout. Every access goes through at24c32_read()/write(), which chunk to
 * the part's transfer and page limits. */

#define MAGIC "LUS1"
#define CHUNK 64u

void eeprom_script_header_encode(uint8_t h[EEPROM_SCRIPT_HDR], uint32_t len, uint32_t crc) {
    memcpy(h, MAGIC, 4);
    h[4] = (uint8_t)len;  h[5] = (uint8_t)(len >> 8);
    h[6] = 0;             h[7] = 0;
    h[8] = (uint8_t)crc;  h[9] = (uint8_t)(crc >> 8);  h[10] = (uint8_t)(crc >> 16);  h[11] = (uint8_t)(crc >> 24);
}

bool eeprom_script_header_decode(const uint8_t h[EEPROM_SCRIPT_HDR], uint32_t *len, uint32_t *crc) {
    if (memcmp(h, MAGIC, 4) != 0 || h[6] != 0 || h[7] != 0) return false;
    uint32_t n = (uint32_t)h[4] | ((uint32_t)h[5] << 8);
    if (n == 0 || n > EEPROM_SCRIPT_MAX) return false;
    if (len) *len = n;
    if (crc) *crc = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
    return true;
}

static bool read_header(uint32_t *len, uint32_t *crc) {
    uint8_t h[EEPROM_SCRIPT_HDR];
    if (at24c32_read(0, h, sizeof(h)) != (int)sizeof(h)) return false;
    return eeprom_script_header_decode(h, len, crc);
}

bool eeprom_script_present(void) {
    return read_header(NULL, NULL);
}

/* Text: printable ASCII, newline, carriage return, tab. A stray control byte or NUL is not a Lisp
 * program that was meant to be there. */
static bool text_ok(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (!((p[i] >= 0x20 && p[i] < 0x7F) || p[i] == '\n' || p[i] == '\r' || p[i] == '\t')) return false;
    return true;
}

/* The CRC-32 of the `len` text bytes in the EEPROM, read in chunks (crc32_update() starts from
 * 0xFFFFFFFF and the result is inverted, which is crc32_compute() for a text held whole). */
static int stored_crc(uint32_t len, uint32_t *out) {
    uint32_t sum = 0xFFFFFFFFu, done = 0;
    uint8_t buf[CHUNK];
    while (done < len) {
        uint32_t n = len - done < CHUNK ? len - done : CHUNK;
        if (at24c32_read((uint16_t)(EEPROM_SCRIPT_HDR + done), buf, n) != (int)n) return EEPROM_SCRIPT_IO;
        sum = crc32_update(sum, buf, n);
        done += n;
    }
    *out = ~sum;
    return 0;
}

int eeprom_script_check(void) {
    uint32_t len, crc, sum;
    if (!read_header(&len, &crc)) return EEPROM_SCRIPT_NONE;
    int r = stored_crc(len, &sum);
    if (r < 0) return r;
    return sum == crc ? (int)len : EEPROM_SCRIPT_BADCRC;
}

int eeprom_script_read(char *buf, uint32_t cap) {
    uint32_t len, crc;
    if (!buf || cap == 0) return EEPROM_SCRIPT_TOOBIG;
    if (!read_header(&len, &crc)) return EEPROM_SCRIPT_NONE;
    if (len + 1u > cap) return EEPROM_SCRIPT_TOOBIG;
    if (at24c32_read(EEPROM_SCRIPT_HDR, (uint8_t *)buf, len) != (int)len) return EEPROM_SCRIPT_IO;
    if (crc32_compute(buf, len) != crc) return EEPROM_SCRIPT_BADCRC;
    buf[len] = '\0';
    return (int)len;
}

bool eeprom_script_clear(void) {
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    return at24c32_write(0, zero, sizeof(zero)) == (int)sizeof(zero);
}

int eeprom_script_put(uint32_t off, const uint8_t *data, uint32_t len) {
    if (!data || off + len > EEPROM_SCRIPT_MAX) return EEPROM_SCRIPT_TOOBIG;
    if (!text_ok(data, len)) return EEPROM_SCRIPT_BADTXT;
    return at24c32_write((uint16_t)(EEPROM_SCRIPT_HDR + off), data, len) == (int)len ? 0 : EEPROM_SCRIPT_IO;
}

int eeprom_script_commit(uint32_t len) {
    if (len == 0 || len > EEPROM_SCRIPT_MAX) return EEPROM_SCRIPT_TOOBIG;
    uint32_t sum;
    int r = stored_crc(len, &sum);
    if (r < 0) return r;
    uint8_t h[EEPROM_SCRIPT_HDR];
    eeprom_script_header_encode(h, len, sum);
    if (at24c32_write(0, h, sizeof(h)) != (int)sizeof(h)) return EEPROM_SCRIPT_IO;
    r = eeprom_script_check();
    return r == (int)len ? 0 : (r < 0 ? r : EEPROM_SCRIPT_IO);
}

int eeprom_script_write(const char *text, uint32_t len) {
    if (!text || len == 0 || len > EEPROM_SCRIPT_MAX) return EEPROM_SCRIPT_TOOBIG;
    if (!text_ok((const uint8_t *)text, len)) return EEPROM_SCRIPT_BADTXT;
    if (!eeprom_script_clear()) return EEPROM_SCRIPT_IO;
    int r = eeprom_script_put(0, (const uint8_t *)text, len);
    return r < 0 ? r : eeprom_script_commit(len);
}

uint32_t eeprom_script_selftest(bool report) {
    uint32_t failed = 0;
#define CHECK(cond, what) do { if (!(cond)) { failed++; if (report) cprintf("  eeprom_script: %s\n", what); } } while (0)

    /* The checksum is the zlib one: crc32("123456789") = 0xCBF43926. */
    CHECK(crc32_compute("123456789", 9) == 0xCBF43926u, "crc32 vector");

    uint8_t h[EEPROM_SCRIPT_HDR];
    uint32_t len = 0, crc = 0;
    eeprom_script_header_encode(h, 0x0123u, 0xDEADBEEFu);
    CHECK(h[0] == 'L' && h[3] == '1' && h[4] == 0x23 && h[5] == 0x01 && h[8] == 0xEF && h[11] == 0xDE, "header bytes");
    CHECK(eeprom_script_header_decode(h, &len, &crc) && len == 0x0123u && crc == 0xDEADBEEFu, "header round trip");
    uint8_t bad[EEPROM_SCRIPT_HDR];
    memcpy(bad, h, sizeof(bad)); bad[0] = 'X';
    CHECK(!eeprom_script_header_decode(bad, NULL, NULL), "bad magic accepted");
    memcpy(bad, h, sizeof(bad)); bad[6] = 1;
    CHECK(!eeprom_script_header_decode(bad, NULL, NULL), "reserved bytes ignored");
    eeprom_script_header_encode(bad, 0, 0);
    CHECK(!eeprom_script_header_decode(bad, NULL, NULL), "length 0 accepted");
    eeprom_script_header_encode(bad, EEPROM_SCRIPT_MAX + 1u, 0);
    CHECK(!eeprom_script_header_decode(bad, NULL, NULL), "oversize length accepted");
    eeprom_script_header_encode(bad, EEPROM_SCRIPT_MAX, 0);
    CHECK(eeprom_script_header_decode(bad, NULL, NULL), "maximum length refused");
    CHECK(EEPROM_SCRIPT_END <= 0x0F00u, "overlaps the calibration blob");

#if !I2C_HAVE_CONTROLLER
    /* The synthetic EEPROM: write, read back, corrupt, clear. */
    static const char prog[] = "(define eeprom-script-test 42)\n";
    char out[64];
    CHECK(eeprom_script_write(prog, sizeof(prog) - 1u) == 0, "write");
    CHECK(eeprom_script_present(), "present after write");
    CHECK(eeprom_script_read(out, sizeof(out)) == (int)(sizeof(prog) - 1u) && strcmp(out, prog) == 0, "read back");
    CHECK(eeprom_script_read(out, 8) == EEPROM_SCRIPT_TOOBIG, "small buffer");
    CHECK(eeprom_script_write("a\001b", 3) == EEPROM_SCRIPT_BADTXT, "control byte accepted");
    CHECK(eeprom_script_read(out, sizeof(out)) == (int)(sizeof(prog) - 1u) || eeprom_script_check() < 0,
          "a refused write must not leave a half-written script");
    CHECK(eeprom_script_write(prog, sizeof(prog) - 1u) == 0, "rewrite");
    (void)at24c32_write(EEPROM_SCRIPT_HDR + 3u, (const uint8_t *)"X", 1);
    CHECK(eeprom_script_check() == EEPROM_SCRIPT_BADCRC, "corruption not caught");
    CHECK(eeprom_script_clear() && !eeprom_script_present() && eeprom_script_check() == EEPROM_SCRIPT_NONE, "clear");
#endif
#undef CHECK
    if (report)
        cprintf("eeprom_script selftest: %lu case%s failed\n", (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}
