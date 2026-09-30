/* USB CRC5 and CRC16 -- 36.7, plan/phase36_rp2350_lcd7_terminal.md.
 * See drivers/include/drivers/usb_crc.h. Both are the reflected forms USB
 * sends LSB first: CRC5 polynomial x^5+x^2+1 (0x05, reflected 0x14), CRC16
 * x^16+x^15+x^2+1 (0x8005, reflected 0xA001), both starting all-ones and
 * sent complemented. */

#include "drivers/usb_crc.h"
#include "kernel/console.h"
#include "kernel/printk.h"

#include <stdbool.h>

uint8_t usb_crc5(uint16_t v11) {
    uint8_t crc = 0x1f;
    for (unsigned i = 0; i < 11; i++) {
        unsigned bit = (v11 >> i) & 1u;
        crc = ((crc ^ bit) & 1u) ? (uint8_t)((crc >> 1) ^ 0x14u) : (uint8_t)(crc >> 1);
    }
    return (uint8_t)(crc ^ 0x1fu);
}

uint16_t usb_crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= byte;
    for (unsigned i = 0; i < 8; i++)
        crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    return crc;
}

uint16_t usb_crc16(const uint8_t *data, uint32_t len) {
    uint16_t crc = 0xffff;
    for (uint32_t i = 0; i < len; i++) crc = usb_crc16_update(crc, data[i]);
    return (uint16_t)(crc ^ 0xffffu);
}

void usb_crc16_table(uint16_t table[256]) {
    for (unsigned i = 0; i < 256; i++) table[i] = usb_crc16_update(0, (uint8_t)i);
}

/* A token's two bytes after the PID, as they go on the wire. */
static uint16_t token_bytes(uint8_t addr, uint8_t ep) {
    uint16_t v = (uint16_t)(addr | (ep << 7));
    return (uint16_t)((v & 0xffu) | ((uint16_t)((usb_crc5(v) << 3) | (v >> 8)) << 8));
}

int usb_crc_selftest(void) {
    int fails = 0;
#define CHECK(name, cond) do { bool ok_ = (cond); cprintf("  %-44s %s\n", name, ok_ ? "ok" : "FAIL"); if (!ok_) fails++; } while (0)
    /* SETUP/IN to address 0, endpoint 0: "2D 00 10" / "69 00 10" in every
     * enumeration capture. */
    CHECK("token addr 0 ep 0 = 00 10", token_bytes(0, 0) == 0x1000u);
    /* The USB CRC whitepaper's examples, whose bit strings are written in
     * transmission order (hence reversed against these values): address
     * 0x15 endpoint 0xe -> 10111; SOF frame 0x710 -> 10100. */
    CHECK("crc5(addr 0x15, ep 0xe) = 0x1d", usb_crc5(0x15u | (0xeu << 7)) == 0x1du);
    CHECK("crc5(frame 0x710) = 0x05", usb_crc5(0x710u) == 0x05u);
    /* GET_DESCRIPTOR(device): "80 06 00 01 00 00 40 00 DD 94" and, with
     * wLength 18, "... 12 00 E0 F4". */
    static const uint8_t gd64[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00 };
    static const uint8_t gd18[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
    CHECK("crc16(GET_DESCRIPTOR 64) = DD 94", usb_crc16(gd64, 8) == 0x94DDu);
    CHECK("crc16(GET_DESCRIPTOR 18) = E0 F4", usb_crc16(gd18, 8) == 0xF4E0u);
    CHECK("crc16(zero-length packet) = 00 00", usb_crc16(gd64, 0) == 0x0000u);
    /* The receive path's check: payload plus received CRC ends at the
     * residual, computed through the table as the engine does. */
    uint16_t table[256];
    usb_crc16_table(table);
    uint16_t c = 0xffff, wire = usb_crc16(gd64, 8);
    uint8_t pkt[10];
    for (unsigned i = 0; i < 8; i++) pkt[i] = gd64[i];
    pkt[8] = (uint8_t)wire; pkt[9] = (uint8_t)(wire >> 8);
    for (unsigned i = 0; i < 10; i++) c = (uint16_t)((c >> 8) ^ table[(c ^ pkt[i]) & 0xffu]);
    CHECK("table crc16(payload + crc) = residual 0xB001", c == USB_CRC16_RESIDUAL);
    pkt[3] ^= 0x04;
    c = 0xffff;
    for (unsigned i = 0; i < 10; i++) c = (uint16_t)((c >> 8) ^ table[(c ^ pkt[i]) & 0xffu]);
    CHECK("one flipped bit misses the residual", c != USB_CRC16_RESIDUAL);
#undef CHECK
    cprintf("%s\n", fails ? "USB_SELFTEST_FAIL" : "USB_SELFTEST_OK");
    return fails;
}
