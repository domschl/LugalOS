/*
 * drivers/radio/nvs_ram.c on the host (45.3b, plan/phase45_esp32c6.md): the
 * protocol the Wi-Fi blob depends on, under ASan so a blob that leaks or
 * double-frees through it is a failure here.
 *
 * Usage: nvs_host [iterations [seed]].
 */

#include "radio/nvs_ram.h"
#include "shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); abort(); } } while (0)

static void *h_alloc(uint32_t n) { return malloc(n); }
static void h_free(void *p) { free(p); }

static uint64_t g_rs;
static uint32_t rnd(uint32_t n) { return host_rand(&g_rs) % n; }

static void test_protocol(void) {
    nvs_store_t s; nvs_ram_init(&s, (nvs_env_t){ h_alloc, h_free });
    uint32_t h = 0, h2 = 0;

    /* Reading a namespace that was never written: NOT_FOUND, in read-only mode. */
    CHECK(nvs_ram_open(&s, "phy", NVS_READONLY, &h) == NVS_ERR_NOT_FOUND, "read-only open of a missing namespace");
    CHECK(nvs_ram_open(&s, "phy", NVS_READWRITE, &h) == NVS_OK && h != 0, "read-write open creates it");
    CHECK(nvs_ram_open(&s, "", NVS_READWRITE, &h2) == NVS_ERR_INVALID_NAME, "empty namespace name");
    CHECK(nvs_ram_open(&s, "0123456789abcdef", NVS_READWRITE, &h2) == NVS_ERR_INVALID_NAME, "16-char namespace name");

    /* The thing the blob keys off: a missing key is NOT_FOUND, then found once set. */
    uint8_t u8 = 0; int8_t i8 = 0; uint16_t u16 = 0;
    CHECK(nvs_ram_get_u8(&s, h, "country", &u8) == NVS_ERR_NOT_FOUND, "missing key");
    CHECK(nvs_ram_set_u8(&s, h, "country", 49) == NVS_OK && nvs_ram_get_u8(&s, h, "country", &u8) == NVS_OK && u8 == 49, "u8 round trip");
    CHECK(nvs_ram_set_i8(&s, h, "gain", -12) == NVS_OK && nvs_ram_get_i8(&s, h, "gain", &i8) == NVS_OK && i8 == -12, "i8 round trip");
    CHECK(nvs_ram_set_u16(&s, h, "chan", 0xbeef) == NVS_OK && nvs_ram_get_u16(&s, h, "chan", &u16) == NVS_OK && u16 == 0xbeef, "u16 round trip");
    CHECK(nvs_ram_get_i8(&s, h, "country", &i8) == NVS_ERR_TYPE_MISMATCH, "reading a u8 as an i8");
    CHECK(nvs_ram_get_u8(&s, h, "x234567890123456", &u8) == NVS_ERR_KEY_TOO_LONG, "16-char key");

    /* Blobs: the PHY's calibration data. */
    uint8_t cal[1900], back[1900];
    for (size_t i = 0; i < sizeof cal; i++) cal[i] = (uint8_t)(i * 31);
    size_t len = 0;
    CHECK(nvs_ram_set_blob(&s, h, "cal_data", cal, sizeof cal) == NVS_OK, "set blob");
    CHECK(nvs_ram_get_blob(&s, h, "cal_data", NULL, &len) == NVS_OK && len == sizeof cal, "length query with a NULL buffer");
    len = 10;
    CHECK(nvs_ram_get_blob(&s, h, "cal_data", back, &len) == NVS_ERR_INVALID_LENGTH && len == sizeof cal, "a short buffer reports the needed length");
    len = sizeof back;
    CHECK(nvs_ram_get_blob(&s, h, "cal_data", back, &len) == NVS_OK && len == sizeof cal && !memcmp(cal, back, len), "blob round trip");
    CHECK(nvs_ram_get_u8(&s, h, "cal_data", &u8) == NVS_ERR_TYPE_MISMATCH, "a blob read as a u8");

    /* Overwrite with a different size and type; the old blob must not leak (ASan/LSan). */
    CHECK(nvs_ram_set_blob(&s, h, "cal_data", cal, 100) == NVS_OK, "overwrite with a smaller blob");
    len = sizeof back;
    CHECK(nvs_ram_get_blob(&s, h, "cal_data", back, &len) == NVS_OK && len == 100, "new length");
    CHECK(nvs_ram_set_u8(&s, h, "cal_data", 1) == NVS_OK && nvs_ram_get_u8(&s, h, "cal_data", &u8) == NVS_OK, "a blob key reused as a u8");
    CHECK(nvs_ram_set_blob(&s, h, "empty", NULL, 0) == NVS_OK, "an empty blob");
    len = 5; CHECK(nvs_ram_get_blob(&s, h, "empty", back, &len) == NVS_OK && len == 0, "reads back empty");

    /* Namespaces are separate; handles can be closed; erase. */
    CHECK(nvs_ram_open(&s, "nvs.net80211", NVS_READWRITE, &h2) == NVS_OK, "second namespace");
    CHECK(nvs_ram_get_u8(&s, h2, "country", &u8) == NVS_ERR_NOT_FOUND, "the same key in another namespace is not found");
    CHECK(nvs_ram_erase_key(&s, h, "country") == NVS_OK && nvs_ram_get_u8(&s, h, "country", &u8) == NVS_ERR_NOT_FOUND, "erase");
    CHECK(nvs_ram_erase_key(&s, h, "country") == NVS_ERR_NOT_FOUND, "erase twice");
    nvs_ram_close(&s, h2);
    CHECK(nvs_ram_get_u8(&s, h2, "x", &u8) == NVS_ERR_INVALID_HANDLE && nvs_ram_commit(&s, h2) == NVS_ERR_INVALID_HANDLE, "a closed handle");
    CHECK(nvs_ram_get_u8(&s, 0, "x", &u8) == NVS_ERR_INVALID_HANDLE && nvs_ram_get_u8(&s, 99, "x", &u8) == NVS_ERR_INVALID_HANDLE, "forged handles");
    CHECK(nvs_ram_commit(&s, h) == NVS_OK, "commit");

    /* A read-only handle on an existing namespace refuses writes. */
    uint32_t ro;
    CHECK(nvs_ram_open(&s, "phy", NVS_READONLY, &ro) == NVS_OK, "read-only open of an existing namespace");
    CHECK(nvs_ram_set_u8(&s, ro, "country", 1) == NVS_ERR_READ_ONLY, "write through a read-only handle");
    CHECK(nvs_ram_get_u8(&s, ro, "chan", &u8) == NVS_ERR_TYPE_MISMATCH && nvs_ram_get_u16(&s, ro, "chan", &u16) == NVS_OK && u16 == 0xbeef, "but reads work");
    nvs_ram_deinit(&s);
}

static void test_exhaustion(void) {
    nvs_store_t s; nvs_ram_init(&s, (nvs_env_t){ h_alloc, h_free });
    uint32_t h; nvs_ram_open(&s, "a", NVS_READWRITE, &h);
    char key[16]; int n = 0;
    for (; n < 200; n++) {
        snprintf(key, sizeof key, "k%d", n);
        if (nvs_ram_set_u8(&s, h, key, 1) != NVS_OK) break;
    }
    CHECK(n == NVS_MAX_ENTRIES, "the entry table holds exactly %d, got %d", NVS_MAX_ENTRIES, n);
    CHECK(nvs_ram_set_u8(&s, h, "k0", 2) == NVS_OK, "overwriting an existing key needs no free entry");
    uint32_t hs[16]; int ok = 0;
    for (int i = 0; i < 16; i++) if (nvs_ram_open(&s, "a", NVS_READWRITE, &hs[i]) == NVS_OK) ok++;
    CHECK(ok == NVS_MAX_HANDLES - 1, "handle table: %d more opens succeeded", ok);
    nvs_ram_deinit(&s);
}

/* Random operations against a model, with blob contents checked and every
 * allocation accounted for (LSan at exit). */
static void test_random(int iters) {
    nvs_store_t s; nvs_ram_init(&s, (nvs_env_t){ h_alloc, h_free });
    uint32_t h; nvs_ram_open(&s, "m", NVS_READWRITE, &h);
    struct { int present; int kind; int32_t val; uint8_t blob[40]; size_t blen; } m[16] = {{0}};
    char key[8];
    for (int it = 0; it < iters; it++) {
        int k = (int)rnd(16); snprintf(key, sizeof key, "key%d", k);
        switch (rnd(5)) {
        case 0: { int v = (int)rnd(256);
            CHECK(nvs_ram_set_u8(&s, h, key, (uint8_t)v) == NVS_OK, "set"); m[k] = (typeof(m[0])){ 1, 1, v, {0}, 0 }; break; }
        case 1: { size_t n = rnd(40); uint8_t b[40]; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(256);
            CHECK(nvs_ram_set_blob(&s, h, key, b, n) == NVS_OK, "set blob"); m[k].present = 1; m[k].kind = 2; m[k].blen = n; memcpy(m[k].blob, b, n); break; }
        case 2: { uint8_t v = 0; int r = nvs_ram_get_u8(&s, h, key, &v);
            if (!m[k].present) CHECK(r == NVS_ERR_NOT_FOUND, "absent: %x", r);
            else if (m[k].kind == 1) CHECK(r == NVS_OK && v == m[k].val, "u8 %d vs %d", v, m[k].val);
            else CHECK(r == NVS_ERR_TYPE_MISMATCH, "blob as u8: %x", r);
            break; }
        case 3: { uint8_t b[64]; size_t n = sizeof b; int r = nvs_ram_get_blob(&s, h, key, b, &n);
            if (!m[k].present) CHECK(r == NVS_ERR_NOT_FOUND, "absent blob");
            else if (m[k].kind == 2) CHECK(r == NVS_OK && n == m[k].blen && !memcmp(b, m[k].blob, n), "blob contents");
            else CHECK(r == NVS_ERR_TYPE_MISMATCH, "u8 as blob");
            break; }
        case 4: { int r = nvs_ram_erase_key(&s, h, key);
            CHECK(r == (m[k].present ? NVS_OK : NVS_ERR_NOT_FOUND), "erase"); m[k].present = 0; break; }
        }
    }
    nvs_ram_deinit(&s);
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    g_rs = seed | 1;
    printf("nvs_host: seed %#llx\n", (unsigned long long)seed);
    test_protocol();
    test_exhaustion();
    test_random(iterations * 50);
    printf("nvs_host: protocol, limits and a %d-step randomized run pass\n", iterations * 50);
    return 0;
}
