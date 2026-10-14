/* Host test of the update image's and the boot record's logic
 * (kernel/ota_image.c): what a loader boots for every state the two record
 * sectors can be in, including a power cut between any two flash writes, and
 * the header's checks. Then random sectors, which must never choose a slot
 * that does not exist. */
#include "kernel/ota_image.h"
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

    if (fails) { printf("%d failures (seed %#llx)\n", fails, (unsigned long long)seed); return 1; }
    printf("ota_host OK\n");
    return 0;
}
