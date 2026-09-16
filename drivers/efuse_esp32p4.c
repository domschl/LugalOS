/* The ESP32-P4's eFuse, read-only, for the two things this kernel wants from
 * silicon: a unique id for the node, and the factory MAC address.
 *
 * Z5, plan/phase28_esp32p4_ethernet.md §1.5. Until this file existed the P4
 * had no board_unique_id(), so kernel/identity.c fell through to derive_mac()
 * and the board answered ARP with a locally-administered address derived from
 * a build seed -- which two P4s flashed from one build would *share*. There
 * is an IEEE-registered address burned into the silicon; this reads it.
 *
 * Every address and bit position below comes from ESP-IDF's generated
 * headers, not from inference -- the same rule the EMAC driver follows and
 * for the same reason (plan/phase27_esp32p4_bringup.md, and the two
 * clock-gate bits that were nearly wrong from a plausible guess):
 *
 *   eFuse base            soc/esp32p4/.../reg_base.h:
 *                         DR_REG_EFUSE_BASE = DR_REG_LPPERIPH_BASE + 0xD000,
 *                         DR_REG_LPPERIPH_BASE = 0x50120000, so 0x5012D000.
 *   BLK1 (MAC) words      efuse_reg.h: EFUSE_RD_MAC_SYS_0_REG = base + 0x44,
 *                         EFUSE_RD_MAC_SYS_1_REG = base + 0x48.
 *   BLK2 (unique id)      efuse_reg.h: EFUSE_RD_SYS_PART1_DATA0_REG =
 *                         base + 0x5c, EFUSE_OPTIONAL_UNIQUE_ID at bits
 *                         [31:0] of it and the three words that follow.
 *
 * The MAC's byte order is the part worth being careful about, because a
 * reversed MAC is still a plausible-looking MAC. efuse/esp32p4/
 * esp_efuse_table.csv defines the field as six single-byte rows in
 * *descending* bit order -- 40, 32, 24, 16, 8, 0 -- so that reading the field
 * into a six-byte buffer yields mac[0] from bits 40..47 down to mac[5] from
 * bits 0..7. That is the mapping below, and it is checked on the board by the
 * OUI: a byte-swapped read would put the low byte of the serial number where
 * the vendor prefix belongs, which does not resemble an Espressif OUI.
 */

#include "kernel/identity.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define REG(a) (*(const volatile uint32_t *)(uintptr_t)(a))

#define EFUSE_BASE              0x5012d000u
#define EFUSE_RD_MAC_SYS_0      (EFUSE_BASE + 0x44)   /* BLK1 bits 31..0  */
#define EFUSE_RD_MAC_SYS_1      (EFUSE_BASE + 0x48)   /* BLK1 bits 63..32 */
#define EFUSE_RD_SYS_PART1_DATA0 (EFUSE_BASE + 0x5c)  /* BLK2 bits 31..0  */

/* True when `out` was filled with the factory MAC. False means the eFuse read
 * came back as all-zero or all-ones, which is not an address -- it is an
 * unprogrammed part or a window that did not respond, and saying so leaves
 * kernel/identity.c on its derived floor rather than having every board of a
 * batch claim the same "factory" address. The same guard idstore_rp2350.c's
 * board_unique_id() applies to the RP2350's chip id, for the same reason. */
bool board_factory_mac(uint8_t out[6]) {
    uint32_t w0 = REG(EFUSE_RD_MAC_SYS_0);
    uint32_t w1 = REG(EFUSE_RD_MAC_SYS_1);

    uint8_t mac[6] = {
        (uint8_t)(w1 >> 8),   /* bits 47..40 */
        (uint8_t)(w1),        /* bits 39..32 */
        (uint8_t)(w0 >> 24),  /* bits 31..24 */
        (uint8_t)(w0 >> 16),  /* bits 23..16 */
        (uint8_t)(w0 >> 8),   /* bits 15..8  */
        (uint8_t)(w0),        /* bits 7..0   */
    };

    uint8_t any = 0, all = 0xff;
    for (unsigned i = 0; i < 6; i++) { any |= mac[i]; all &= mac[i]; }
    if (any == 0 || all == 0xff) return false;

    /* A factory MAC is a unicast, globally-administered address: bit 0 of the
     * first byte clear (unicast) and bit 1 clear (not locally administered).
     * If those do not hold, the read is wrong -- most likely byte-reversed --
     * and handing it out would produce an address the segment may refuse in
     * ways that look like a driver bug. Refusing here costs a derived address
     * and keeps the failure legible. */
    if (mac[0] & 0x03u) return false;

    memcpy(out, mac, 6);
    return true;
}

bool board_unique_id(uint8_t out[8]) {
    /* BLK2's OPTIONAL_UNIQUE_ID first: it is 128 bits of factory randomness
     * and exists precisely to be this. It is *optional*, though -- the name is
     * not decoration, and on a part where it was never burned it reads as
     * zero. */
    uint32_t u0 = REG(EFUSE_RD_SYS_PART1_DATA0);
    uint32_t u1 = REG(EFUSE_RD_SYS_PART1_DATA0 + 4);
    if (u0 != 0 || u1 != 0) {
        for (unsigned i = 0; i < 4; i++) {
            out[i]     = (uint8_t)(u0 >> (8 * i));
            out[i + 4] = (uint8_t)(u1 >> (8 * i));
        }
        return true;
    }

    /* Otherwise the factory MAC, which is also unique per chip and is
     * IEEE-registered, so it is a legitimate identifier rather than a
     * fallback in the apologetic sense. Six bytes into eight, high two zero.
     * Deliberately *not* padded with anything derived: the whole value of a
     * unique id is that it comes from the part, and mixing in a build seed
     * would quietly undo that. */
    uint8_t mac[6];
    if (!board_factory_mac(mac)) return false;
    memset(out, 0, 8);
    memcpy(out + 2, mac, 6);
    return true;
}
