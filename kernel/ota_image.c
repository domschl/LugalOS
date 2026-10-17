/* The update image's and the boot record's logic (kernel/include/kernel/ota_image.h).
 * Freestanding and pure -- no kernel call, no library call -- because a board's
 * loader links it too, and tests/host/ota_host.c runs it under the sanitizers. */

#include "kernel/ota_image.h"

/* The zlib/Ethernet CRC-32, bitwise: a loader has no room to spare for a table. */
uint32_t ota_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xffffffffu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

bool ota_bootrec_valid(const ota_bootrec_t *r, uint32_t nslots) {
    if (!r || r->magic != OTA_BOOTREC_MAGIC) return false;
    if (r->crc != ota_crc32(r, (uint32_t)__builtin_offsetof(ota_bootrec_t, crc))) return false;
    if (r->active >= nslots) return false;
    return r->pending == OTA_SLOT_NONE || r->pending < nslots;
}

int ota_bootrec_current(const ota_bootrec_t *rec[2], uint32_t nslots) {
    bool v0 = ota_bootrec_valid(rec[0], nslots), v1 = ota_bootrec_valid(rec[1], nslots);
    if (v0 && v1) return (int32_t)(rec[1]->seq - rec[0]->seq) > 0 ? 1 : 0;   /* wraps safely */
    return v0 ? 0 : v1 ? 1 : -1;
}

ota_boot_choice_t ota_boot_choose(const ota_bootrec_t *rec[2], uint32_t nslots) {
    ota_boot_choice_t c = { .slot = 0, .trial = false, .fell_back = false,
                            .current = -1, .mark_sector = -1, .mark_off = 0 };
    int cur = ota_bootrec_current(rec, nslots);
    if (cur < 0) return c;                      /* a board fresh from the header pins: slot 0 */
    const ota_bootrec_t *r = rec[cur];
    c.current = cur;
    c.slot = r->active;
    if (r->pending == OTA_SLOT_NONE || r->pending == r->active) return c;
    if (r->tried == 0xffffffffu) {
        c.slot = r->pending;
        c.trial = true;
        c.mark_sector = cur;
        c.mark_off = OTA_BOOTREC_TRIED_OFF;
        return c;
    }
    c.fell_back = true;
    if (r->failed == 0xffffffffu) {
        c.mark_sector = cur;
        c.mark_off = OTA_BOOTREC_FAILED_OFF;
    }
    return c;
}

void ota_bootrec_make(ota_bootrec_t *r, uint32_t seq, uint8_t active, uint8_t pending) {
    r->magic = OTA_BOOTREC_MAGIC;
    r->seq = seq;
    r->active = active;
    r->pending = pending;
    r->pad[0] = r->pad[1] = 0;
    r->crc = ota_crc32(r, (uint32_t)__builtin_offsetof(ota_bootrec_t, crc));
    r->tried = 0xffffffffu;
    r->failed = 0xffffffffu;
}

bool ota_header_valid(const ota_image_header_t *h) {
    return h && h->magic == OTA_IMAGE_MAGIC && h->version == OTA_IMAGE_VERSION &&
           h->header_len == OTA_HEADER_LEN &&
           h->crc == ota_crc32(h, (uint32_t)__builtin_offsetof(ota_image_header_t, crc));
}

bool ota_idf_parse_partitions(const void *table, uint32_t len, ota_idf_layout_t *out) {
    if (!table || !out) return false;
    out->otadata_base = out->otadata_size = 0;
    out->slot_base[0] = out->slot_size[0] = 0;
    out->slot_base[1] = out->slot_size[1] = 0;
    const esp_part_entry_t *e = (const esp_part_entry_t *)table;
    uint32_t max_entries = len / (uint32_t)sizeof(esp_part_entry_t);
    for (uint32_t i = 0; i < max_entries; i++) {
        if (e[i].magic != ESP_PART_MAGIC) break;
        if (e[i].type == ESP_PART_TYPE_DATA && e[i].subtype == ESP_PART_SUBTYPE_OTA) {
            out->otadata_base = e[i].offset;
            out->otadata_size = e[i].size;
        } else if (e[i].type == ESP_PART_TYPE_APP && e[i].subtype == ESP_PART_SUBTYPE_OTA0) {
            out->slot_base[0] = e[i].offset;
            out->slot_size[0] = e[i].size;
        } else if (e[i].type == ESP_PART_TYPE_APP && e[i].subtype == ESP_PART_SUBTYPE_OTA1) {
            out->slot_base[1] = e[i].offset;
            out->slot_size[1] = e[i].size;
        }
    }
    return out->otadata_base >= 0x8000u && (out->otadata_base & 0xfffu) == 0 &&
           out->otadata_size >= 0x2000u &&
           out->slot_base[0] >= 0x10000u && (out->slot_base[0] & 0xffffu) == 0 && out->slot_size[0] >= 0x100000u &&
           out->slot_base[1] >= 0x10000u && (out->slot_base[1] & 0xffffu) == 0 && out->slot_size[1] >= 0x100000u &&
           out->slot_base[0] != out->slot_base[1];
}

/* ESP-IDF's bootloader_common_ota_select_crc() calls esp_rom_crc32_le(UINT32_MAX, &s->ota_seq, 4).
 * esp_rom_crc32_le(crc, buf, len) starts with c = ~crc, so passing UINT32_MAX starts with c = 0. */
uint32_t ota_idf_crc32(uint32_t seq) {
    uint32_t c = 0u;
    for (int i = 0; i < 4; i++) {
        c ^= (seq >> (8 * i)) & 0xffu;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

bool ota_idf_select_valid(const esp_ota_select_entry_t *s) {
    if (!s || s->ota_seq == 0xFFFFFFFFu || s->ota_seq == 0u) return false;
    if (s->ota_state == ESP_OTA_IMG_INVALID || s->ota_state == ESP_OTA_IMG_ABORTED) return false;
    return s->crc == ota_idf_crc32(s->ota_seq);
}

int ota_idf_active_sector(const esp_ota_select_entry_t s[2]) {
    if (!s) return -1;
    bool v0 = ota_idf_select_valid(&s[0]), v1 = ota_idf_select_valid(&s[1]);
    if (v0 && v1) return s[1].ota_seq > s[0].ota_seq ? 1 : 0;
    return v0 ? 0 : v1 ? 1 : -1;
}

uint32_t ota_idf_next_seq(uint32_t cur_seq, int target_slot) {
    uint32_t slot = (uint32_t)(target_slot & 1);
    uint32_t next = (cur_seq == 0xFFFFFFFFu ? 0u : cur_seq) + 1u;
    if (((next - 1u) & 1u) != slot) next++;
    return next;
}

void ota_idf_select_make(esp_ota_select_entry_t *s, uint32_t seq, uint32_t state, bool trial_tag) {
    s->ota_seq = seq;
    for (int i = 0; i < 20; i++) s->seq_label[i] = 0xffu;
    if (trial_tag) {
        s->seq_label[0] = 'L'; s->seq_label[1] = 'T'; s->seq_label[2] = 'R'; s->seq_label[3] = 'L';
    }
    s->ota_state = state;
    s->crc = ota_idf_crc32(seq);
}

bool ota_idf_select_is_trial(const esp_ota_select_entry_t *s) {
    return s && s->seq_label[0] == 'L' && s->seq_label[1] == 'T' &&
           s->seq_label[2] == 'R' && s->seq_label[3] == 'L';
}
