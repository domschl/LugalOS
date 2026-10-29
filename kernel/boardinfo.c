#include "lugalos_config.h"
#include "kernel/boardinfo.h"
#include "kernel/identity.h"
#include "kernel/printk.h"
#include "kernel/taint.h"
#include "kernel/version.h"
#include <stddef.h>

#if defined(CONFIG_BOARD_ESP32P4)
unsigned esp32p4_chip_rev(void);
#endif

#ifndef CONFIG_PRESET
#define CONFIG_PRESET LUGALOS_BOARD_ID
#endif
#ifndef CONFIG_BOARD_DESC
#define CONFIG_BOARD_DESC LUGALOS_BOARD_ID
#endif

/* The silicon, as a short string. */
static void chip_string(char *out, uint32_t cap) {
#if defined(CONFIG_BOARD_ESP32P4)
    unsigned rev = esp32p4_chip_rev();
    ksnprintf(out, cap, "ESP32-P4 v%u.%u", rev / 100u, rev % 100u);
#elif defined(CONFIG_BOARD_ESP32C6)
    ksnprintf(out, cap, "ESP32-C6");
#elif defined(CONFIG_BOARD_RP2350)
    ksnprintf(out, cap, "RP2350");
#elif defined(CONFIG_TARGET_RV32)
    ksnprintf(out, cap, "QEMU rv32");
#else
    ksnprintf(out, cap, "QEMU rv64");
#endif
}

static void uid_string(char *out, uint32_t cap) {
    static const char hex[] = "0123456789abcdef";
    uint8_t uid[NODE_UID_LEN];
    if (!node_uid(uid) || cap < NODE_UID_LEN * 2u + 1u) { ksnprintf(out, cap, "none"); return; }
    for (unsigned i = 0; i < NODE_UID_LEN; i++) {
        out[i * 2] = hex[uid[i] >> 4];
        out[i * 2 + 1] = hex[uid[i] & 15u];
    }
    out[NODE_UID_LEN * 2] = 0;
}

uint32_t board_info_text(char *out, uint32_t cap) {
    char chip[32], uid[NODE_UID_LEN * 2 + 1];
    chip_string(chip, sizeof chip);
    uid_string(uid, sizeof uid);
    int n = ksnprintf(out, cap,
                      "preset=%s\nboard=%s\nhardware=%s\nchip=%s\nversion=%s\nbuild=%s\ntainted=%s\nname=%s\nuid=%s\n",
                      CONFIG_PRESET, LUGALOS_BOARD_ID, CONFIG_BOARD_DESC, chip, LUGALOS_VERSION, LUGALOS_BUILD_ID,
                      LUGALOS_TAINTED ? "yes" : "no", node_name(), uid);
    return n < 0 ? 0u : (uint32_t)(n < (int)cap ? n : (int)cap - 1);
}

uint32_t board_info_line(char *out, uint32_t cap) {
    char chip[32], uid[NODE_UID_LEN * 2 + 1];
    chip_string(chip, sizeof chip);
    uid_string(uid, sizeof uid);
    int n = ksnprintf(out, cap, "%s v%s (%s, %s) \"%s\" uid %s", CONFIG_PRESET, LUGALOS_VERSION,
                      CONFIG_BOARD_DESC, chip, node_name(), uid);
    return n < 0 ? 0u : (uint32_t)(n < (int)cap ? n : (int)cap - 1);
}
