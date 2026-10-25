/* Host test of the update image's and the boot record's logic
 * (kernel/ota_image.c): what a loader boots for every state the two record
 * sectors can be in, including a power cut between any two flash writes, and
 * the header's checks. Then random sectors, which must never choose a slot
 * that does not exist. */
#include "kernel/ota_image.h"
#include "kernel/ota_sig.h"
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ota_boot_choice_t choose(const ota_bootrec_t *a, const ota_bootrec_t *b) {
    const ota_bootrec_t *rec[2] = { a, b };
    return ota_boot_choose(rec, 2);
}

/* What the loader does with a choice: clear the word it names. */
static void apply(ota_bootrec_t *sec[2], ota_boot_choice_t c) {
    if (c.mark_sector < 0) return;
    uint8_t *p = (uint8_t *)sec[c.mark_sector] + c.mark_off;
    memset(p, 0, 4);
}

static void erased(ota_bootrec_t *r) { memset(r, 0xff, sizeof *r); }

/* kernel/crypto.c is linked for the signature check, which needs no entropy:
 * verification only. A board without a source would refuse key generation,
 * and nothing here generates a key. */
bool random_is_hardware(void) { return false; }

static void unhex(uint8_t *out, size_t n, const char *hex) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* Signed by tools/lota.py's sign_image() (Python `cryptography`), with the
 * test key d = SHA-256("lugalos ota test release key"): the host tool and the
 * node's check agree on the format, not merely with themselves. */
static const char *SIG_PUB =
    "7c2f01ea7be77828a36f58789519eec94c1e7b4c48b5df2b94a261faa5e07e8c"
    "24754218eb65690567c668fd686f5875aa374b9fc9c7bfeee9466fe635e301a2";
static const char *SIG_HDR =
    "4c4f544101000001657370333263362d7a65726f000000000000000000000000"
    "6573703332633600000000000000ffff640000003f000000746573742d627569"
    "6c642d31000000000000000000000000000000000000000095ccd3b5984d4759"
    "39629ec928de9e799e2fbb7db6e961cdf6e9c0c6469a2cfc010000002e981780"
    "c349efc07c32e00b69198426b78d191e15ae1eb58808bfd155f499bd68cd85b1"
    "af25986b38dfa38e414c470435dfaa43490b525815182337486874e20a9f04e7"
    "d7031b2700000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000004ee00418";

static void signature_tests(void) {
    char hex[513];
    size_t k = 0;
    for (const char *c = SIG_HDR; *c; c++) if (*c != ' ') hex[k++] = *c;
    hex[k] = 0;
    CHECK(k == 512, "vector is 256 bytes (%zu hex digits)", k);

    ota_image_header_t h, t;
    uint8_t pub[64], other[64];
    unhex((uint8_t *)&h, sizeof h, hex);
    unhex(pub, sizeof pub, SIG_PUB);
    CHECK(ota_header_valid(&h), "signed header is still a valid version-1 header");
    CHECK(ota_sig_check(&h, pub) == OTA_SIG_RESULT_OK, "lota.py's signature verifies");
    CHECK(ota_sig_check(&h, NULL) == OTA_SIG_RESULT_NO_KEY, "no key: signed but unverifiable");
    memcpy(other, pub, sizeof other);
    other[63] ^= 1;
    CHECK(ota_sig_check(&h, other) == OTA_SIG_RESULT_OTHER_KEY, "another key: refused by key id");

    /* Every byte the signature covers, and every signature byte: one flipped
     * bit each must be refused. */
    for (uint32_t i = 0; i < OTA_SIGNED_LEN + 64u; i++) {
        uint32_t at = i < OTA_SIGNED_LEN ? i : (uint32_t)__builtin_offsetof(ota_image_header_t, sig) + (i - OTA_SIGNED_LEN);
        t = h;
        ((uint8_t *)&t)[at] ^= (uint8_t)(1u << (i % 8));
        ota_sig_result_t r = ota_sig_check(&t, pub);
        CHECK(r != OTA_SIG_RESULT_OK, "flipped bit at header byte %u accepted", at);
    }

    t = h;
    t.reserved[3] = 1;
    CHECK(ota_sig_check(&t, pub) == OTA_SIG_RESULT_MALFORMED, "non-zero reserved bytes are refused");
    t = h;
    t.sig_alg = 7;
    CHECK(ota_sig_check(&t, pub) == OTA_SIG_RESULT_MALFORMED, "an unknown algorithm is refused");

    t = h;
    t.sig_alg = OTA_SIG_NONE;
    memset(t.key_id, 0, sizeof t.key_id);
    memset(t.sig, 0, sizeof t.sig);
    CHECK(ota_sig_check(&t, pub) == OTA_SIG_RESULT_UNSIGNED, "an unsigned header reads as unsigned");
    t.sig[0] = 1;
    CHECK(ota_sig_check(&t, pub) == OTA_SIG_RESULT_MALFORMED, "unsigned with signature bytes set is refused");
}

int main(int argc, char **argv) {
    unsigned iters = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;

    CHECK(ota_crc32("123456789", 9) == 0xcbf43926u, "CRC-32 check value: %08x", ota_crc32("123456789", 9));

    ota_bootrec_t s0, s1;
    ota_bootrec_t *sec[2] = { &s0, &s1 };

    /* Fresh from the header pins: both sectors erased -> slot 0, nothing to mark. */
    erased(&s0); erased(&s1);
    ota_boot_choice_t c = choose(&s0, &s1);
    CHECK(c.slot == 0 && !c.trial && !c.fell_back && c.current == -1 && c.mark_sector < 0, "fresh");

    /* An update to slot 1: a record with pending = 1 in sector 0. */
    ota_bootrec_make(&s0, 1, 0, 1);
    c = choose(&s0, &s1);
    CHECK(c.slot == 1 && c.trial && c.mark_sector == 0 && c.mark_off == OTA_BOOTREC_TRIED_OFF, "first trial boot");
    apply(sec, c);
    CHECK(ota_bootrec_valid(&s0, 2), "clearing tried keeps the record valid");

    /* The trial reset without confirming: back to slot 0, failed marked once. */
    c = choose(&s0, &s1);
    CHECK(c.slot == 0 && !c.trial && c.fell_back && c.mark_off == OTA_BOOTREC_FAILED_OFF, "fall back");
    apply(sec, c);
    c = choose(&s0, &s1);
    CHECK(c.slot == 0 && c.fell_back && c.mark_sector < 0, "fall back again, nothing more to mark");

    /* Another try, then a confirm: the new record goes to the other sector. */
    ota_bootrec_make(&s1, 2, 0, 1);
    c = choose(&s0, &s1);
    CHECK(c.slot == 1 && c.trial && c.mark_sector == 1, "the higher sequence number is current");
    apply(sec, c);
    ota_bootrec_make(&s0, 3, 1, OTA_SLOT_NONE);
    c = choose(&s0, &s1);
    CHECK(c.slot == 1 && !c.trial && !c.fell_back && c.current == 0, "confirmed");

    /* A power cut while the confirm was written: sector 0 erased, or half written. */
    erased(&s0);
    c = choose(&s0, &s1);
    CHECK(c.slot == 0 && c.fell_back, "cut during the erase: the tried record stands, slot 0");
    ota_bootrec_make(&s0, 3, 1, OTA_SLOT_NONE);
    memset((uint8_t *)&s0 + 8, 0xff, 4);        /* active/pending never landed */
    c = choose(&s0, &s1);
    CHECK(c.current == 1, "cut during the write: a record that fails its CRC is not current");

    /* Sequence numbers across the wrap. */
    ota_bootrec_make(&s0, 0xffffffffu, 0, OTA_SLOT_NONE);
    ota_bootrec_make(&s1, 0, 1, OTA_SLOT_NONE);
    c = choose(&s0, &s1);
    CHECK(c.slot == 1, "0 follows 0xffffffff");

    /* A record naming a slot that does not exist is not a record. */
    ota_bootrec_make(&s0, 9, 2, OTA_SLOT_NONE);
    erased(&s1);
    c = choose(&s0, &s1);
    CHECK(c.slot == 0 && c.current == -1, "slot 2 of 2");

    /* The header. */
    ota_image_header_t h;
    memset(&h, 0, sizeof h);
    h.magic = OTA_IMAGE_MAGIC;
    h.version = OTA_IMAGE_VERSION;
    h.header_len = OTA_HEADER_LEN;
    strcpy(h.board, "esp32c6-mini");
    h.crc = ota_crc32(&h, OTA_HEADER_LEN - 4);
    CHECK(ota_header_valid(&h), "a good header");
    h.board[0] ^= 1;
    CHECK(!ota_header_valid(&h), "one flipped bit");
    h.board[0] ^= 1;
    h.version = 2;
    h.crc = ota_crc32(&h, OTA_HEADER_LEN - 4);
    CHECK(!ota_header_valid(&h), "a version this code does not know");

    /* Random sectors: never a slot that does not exist, never a mark outside them. */
    for (unsigned i = 0; i < iters; i++) {
        uint8_t raw[2][sizeof(ota_bootrec_t)];
        for (unsigned k = 0; k < sizeof raw; k++) ((uint8_t *)raw)[k] = (uint8_t)host_rand(&seed);
        ota_bootrec_t r0, r1;
        memcpy(&r0, raw[0], sizeof r0);
        memcpy(&r1, raw[1], sizeof r1);
        if (i & 1) ota_bootrec_make(&r0, host_rand(&seed), (uint8_t)(host_rand(&seed) % 3), (uint8_t)host_rand(&seed));
        c = choose(&r0, (i & 2) ? &r1 : NULL);
        CHECK(c.slot == 0 || c.slot == 1, "iteration %u chose slot %d", i, c.slot);
        CHECK(c.mark_sector < 2, "iteration %u marks sector %d", i, c.mark_sector);
    }

    /* --- ESP-IDF partition table & otadata (`esp_ota_select_entry_t`) --- */
    {
        esp_part_entry_t tbl[5];
        memset(tbl, 0xff, sizeof tbl);
        /* Entry 0: nvs */
        tbl[0].magic = ESP_PART_MAGIC;
        tbl[0].type = ESP_PART_TYPE_DATA;
        tbl[0].subtype = 0x02;
        tbl[0].offset = 0x9000;
        tbl[0].size = 0x4000;
        strcpy(tbl[0].label, "nvs");
        tbl[0].flags = 0;
        /* Entry 1: otadata */
        tbl[1].magic = ESP_PART_MAGIC;
        tbl[1].type = ESP_PART_TYPE_DATA;
        tbl[1].subtype = ESP_PART_SUBTYPE_OTA;
        tbl[1].offset = 0xd000;
        tbl[1].size = 0x2000;
        strcpy(tbl[1].label, "otadata");
        tbl[1].flags = 0;
        /* Entry 2: phy_init */
        tbl[2].magic = ESP_PART_MAGIC;
        tbl[2].type = ESP_PART_TYPE_DATA;
        tbl[2].subtype = 0x01;
        tbl[2].offset = 0xf000;
        tbl[2].size = 0x1000;
        strcpy(tbl[2].label, "phy_init");
        tbl[2].flags = 0;
        /* Entry 3: ota_0 */
        tbl[3].magic = ESP_PART_MAGIC;
        tbl[3].type = ESP_PART_TYPE_APP;
        tbl[3].subtype = ESP_PART_SUBTYPE_OTA0;
        tbl[3].offset = 0x10000;
        tbl[3].size = 0x1c0000;
        strcpy(tbl[3].label, "ota_0");
        tbl[3].flags = 0;
        /* Entry 4: ota_1 */
        tbl[4].magic = ESP_PART_MAGIC;
        tbl[4].type = ESP_PART_TYPE_APP;
        tbl[4].subtype = ESP_PART_SUBTYPE_OTA1;
        tbl[4].offset = 0x1d0000;
        tbl[4].size = 0x1c0000;
        strcpy(tbl[4].label, "ota_1");
        tbl[4].flags = 0;

        ota_idf_layout_t lay;
        CHECK(ota_idf_parse_partitions(tbl, sizeof tbl, &lay), "parse standard ESP-IDF partition table");
        CHECK(lay.otadata_base == 0xd000 && lay.otadata_size == 0x2000, "otadata at 0xd000");
        CHECK(lay.slot_base[0] == 0x10000 && lay.slot_size[0] == 0x1c0000, "ota_0 at 0x10000");
        CHECK(lay.slot_base[1] == 0x1d0000 && lay.slot_size[1] == 0x1c0000, "ota_1 at 0x1d0000");
        CHECK(!ota_idf_parse_partitions(tbl, 3 * sizeof(esp_part_entry_t), &lay), "truncated partition table rejected");

        /* otadata selection and sequence numbers */
        esp_ota_select_entry_t s[2];
        memset(s, 0xff, sizeof s);
        CHECK(!ota_idf_select_valid(&s[0]), "erased otadata is not valid");
        CHECK(ota_idf_active_sector(s) == -1, "both erased -> -1 (boot ota_0)");
        CHECK(ota_idf_next_seq(0, 0) == 1 && ota_idf_next_seq(0, 1) == 2, "next_seq from 0");
        CHECK(ota_idf_next_seq(1, 1) == 2 && ota_idf_next_seq(2, 0) == 3, "next_seq alternating");
        CHECK(ota_idf_next_seq(2, 1) == 4 && ota_idf_next_seq(3, 0) == 5, "next_seq same parity");

        ota_idf_select_make(&s[0], 1, ESP_OTA_IMG_VALID, false);
        CHECK(ota_idf_select_valid(&s[0]) && !ota_idf_select_is_trial(&s[0]), "valid s[0] seq=1");
        CHECK(ota_idf_active_sector(s) == 0, "s[0] active when s[1] erased");

        ota_idf_select_make(&s[1], 2, ESP_OTA_IMG_UNDEFINED, true);
        CHECK(ota_idf_select_valid(&s[1]) && ota_idf_select_is_trial(&s[1]), "trial s[1] seq=2 valid before boot");
        CHECK(ota_idf_active_sector(s) == 1, "s[1] active on trial boot");

        /* Simulated in-place bit clear of ota_state to ESP_OTA_IMG_ABORTED (4) on trial start */
        s[1].ota_state = ESP_OTA_IMG_ABORTED;
        CHECK(!ota_idf_select_valid(&s[1]), "aborted trial rejected by bootloader");
        CHECK(ota_idf_active_sector(s) == 0, "bootloader falls back to s[0]");
    }

    signature_tests();

    if (fails) { printf("%d failures (seed %#llx)\n", fails, (unsigned long long)seed); return 1; }
    printf("ota_host OK\n");
    return 0;
}
