#ifndef KERNEL_OTA_IMAGE_H
#define KERNEL_OTA_IMAGE_H

/* The update image and the boot record: the formats shared by the kernel's
 * update path (kernel/ota.c), a board's loader (arch/riscv/esp32c6/loader.c),
 * the host tools (tools/lota.py) and tests/host/ota_host.c. Freestanding: no
 * kernel header, so the loader can build it.
 *
 * ## The update image
 *
 *   header        256 bytes, below
 *   part 0        part_len[0] bytes
 *   part 1        part_len[1] bytes
 *
 * What a part is belongs to the board: on the ESP32-C6, part 0 is the kernel's
 * RAM half in the boot ROM's image format and part 1 its XIP half. Where a
 * part lands in a slot is the board's too (drivers/ota_esp32c6.c). The
 * header says which board the image is for, so a node refuses another
 * board's image before it erases anything.
 *
 * ## The boot record
 *
 * Two flash sectors, each holding at most one record; the valid one with the
 * higher sequence number is current. A new record goes to the other sector
 * (erase, write), so a power cut at any point leaves the old one current. The
 * loader moves a trial along without erasing: `tried` and `failed` start as
 * all ones and are cleared to zero in place, which flash allows without an
 * erase.
 *
 *   pending == none                  boot `active`
 *   pending, tried still set         clear tried, boot `pending` on trial
 *   pending, tried clear             the trial did not confirm: clear
 *                                    failed, boot `active`
 *
 * A kernel booted on trial confirms by writing a new record whose `active` is
 * its slot and `pending` none; until it does, a watchdog the loader armed
 * will reset it, and the next boot falls back. */

#include <stdbool.h>
#include <stdint.h>

#define OTA_IMAGE_MAGIC    0x41544f4cu         /* "LOTA" */
#define OTA_IMAGE_VERSION  1u
#define OTA_HEADER_LEN     256u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    char     board[24];             /* the build's board, e.g. "esp32c6-mini"; NUL-padded */
    char     chip[12];              /* e.g. "esp32c6" */
    uint16_t chip_rev_min;          /* major * 100 + minor, inclusive */
    uint16_t chip_rev_max;
    uint32_t part_len[2];
    char     build_id[32];          /* /proc/buildid's second field */
    uint8_t  sha256[32];            /* over part 0 then part 1, as they follow */
    uint8_t  reserved[132];
    uint32_t crc;                   /* CRC-32 of the 252 bytes before it */
} ota_image_header_t;

_Static_assert(sizeof(ota_image_header_t) == OTA_HEADER_LEN, "the header is 256 bytes on the wire");

#define OTA_BOOTREC_MAGIC  0x42544f4cu         /* "LOTB" */
#define OTA_SLOT_NONE      0xffu

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  active;                /* slot index */
    uint8_t  pending;               /* slot index on trial, or OTA_SLOT_NONE */
    uint8_t  pad[2];
    uint32_t crc;                   /* CRC-32 of the 12 bytes before it */
    uint32_t tried;                 /* all ones until the loader boots `pending` */
    uint32_t failed;                /* all ones until the loader falls back from it */
} ota_bootrec_t;

#define OTA_BOOTREC_TRIED_OFF   ((uint32_t)__builtin_offsetof(ota_bootrec_t, tried))
#define OTA_BOOTREC_FAILED_OFF  ((uint32_t)__builtin_offsetof(ota_bootrec_t, failed))

/* What a loader does with the two records (rec[i] is NULL if sector i could
 * not be read). `mark` is a word to clear in sector `mark_sector` before
 * booting, at offset `mark_off` (OTA_BOOTREC_TRIED_OFF or _FAILED_OFF), or
 * mark_sector < 0 for none. */
typedef struct {
    int      slot;                  /* the slot to boot */
    bool     trial;                 /* booting `pending` for the first time */
    bool     fell_back;             /* a trial did not confirm; booting `active` */
    int      current;               /* which sector holds the current record, -1 for none */
    int      mark_sector;
    uint32_t mark_off;
} ota_boot_choice_t;

uint32_t ota_crc32(const void *data, uint32_t len);
bool ota_bootrec_valid(const ota_bootrec_t *r, uint32_t nslots);
/* Index (0/1) of the current record, or -1. */
int ota_bootrec_current(const ota_bootrec_t *rec[2], uint32_t nslots);
ota_boot_choice_t ota_boot_choose(const ota_bootrec_t *rec[2], uint32_t nslots);
/* A record for `seq`, `active`, `pending`, with its CRC and tried/failed set. */
void ota_bootrec_make(ota_bootrec_t *r, uint32_t seq, uint8_t active, uint8_t pending);
/* The header's magic, version, length and CRC (not the board, nor the parts). */
bool ota_header_valid(const ota_image_header_t *h);

/* --- 52.1 / 52.2: ESP-IDF Application Descriptor, Partition Table & otadata --- */

#define ESP_APP_DESC_MAGIC       0xABCD5432u

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char     version[32];
    char     project_name[32];
    char     time[16];
    char     date[16];
    char     idf_ver[32];
    uint8_t  app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full;
    uint16_t max_efuse_blk_rev_full;
    uint8_t  mmu_page_size;
    uint8_t  spi_flash_mode;
    uint8_t  reserv3[2];
    uint32_t reserv2[18];
} esp_app_desc_t;

_Static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t is 256 bytes");

#define ESP_PART_MAGIC           0x50AAu
#define ESP_PART_TYPE_APP        0x00u
#define ESP_PART_SUBTYPE_OTA0    0x10u
#define ESP_PART_SUBTYPE_OTA1    0x11u
#define ESP_PART_TYPE_DATA       0x01u
#define ESP_PART_SUBTYPE_OTA     0x00u

typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    char     label[16];
    uint32_t flags;
} esp_part_entry_t;

_Static_assert(sizeof(esp_part_entry_t) == 32, "esp_part_entry_t is 32 bytes");

typedef struct {
    uint32_t otadata_base;
    uint32_t otadata_size;
    uint32_t slot_base[2];
    uint32_t slot_size[2];
} ota_idf_layout_t;

#define ESP_OTA_IMG_NEW             0x0u
#define ESP_OTA_IMG_PENDING_VERIFY  0x1u
#define ESP_OTA_IMG_VALID           0x2u
#define ESP_OTA_IMG_INVALID         0x3u
#define ESP_OTA_IMG_ABORTED         0x4u
#define ESP_OTA_IMG_UNDEFINED       0xFFFFFFFFu

typedef struct {
    uint32_t ota_seq;
    uint8_t  seq_label[20];
    uint32_t ota_state;
    uint32_t crc;                   /* esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4) */
} esp_ota_select_entry_t;

_Static_assert(sizeof(esp_ota_select_entry_t) == 32, "esp_ota_select_entry_t is 32 bytes");

bool ota_idf_parse_partitions(const void *table, uint32_t len, ota_idf_layout_t *out);
uint32_t ota_idf_crc32(uint32_t seq);
bool ota_idf_select_valid(const esp_ota_select_entry_t *s);
int ota_idf_active_sector(const esp_ota_select_entry_t s[2]);
uint32_t ota_idf_next_seq(uint32_t cur_seq, int target_slot);
void ota_idf_select_make(esp_ota_select_entry_t *s, uint32_t seq, uint32_t state, bool trial_tag);
bool ota_idf_select_is_trial(const esp_ota_select_entry_t *s);

#endif
