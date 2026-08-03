#ifndef RP2350_BINARY_INFO_H
#define RP2350_BINARY_INFO_H

#include <stdint.h>

#define BINARY_INFO_MARKER_START 0x7188EBF2
#define BINARY_INFO_MARKER_END   0xE71AA390

#define BINARY_INFO_TYPE_ID_AND_INT    5
#define BINARY_INFO_TYPE_ID_AND_STRING 6

#define BINARY_INFO_TAG_RP 0x5052  /* 'R' | ('P' << 8) */

#define BINARY_INFO_ID_RP_PROGRAM_NAME 0x02031c86
#define BINARY_INFO_ID_RP_BINARY_END   0x68f465de

struct __attribute__((packed)) rp2350_boot_header_t {
    uint32_t marker_start;   /* 0x7188EBF2 */
    uint32_t info_start;     /* &__binary_info_start */
    uint32_t info_end;       /* &__binary_info_end */
    uint32_t mapping_table;   /* &g_address_mapping_table */
    uint32_t marker_end;     /* 0xE71AA390 */
};

struct __attribute__((packed)) bi_id_and_string_t {
    uint16_t type;
    uint16_t tag;
    uint32_t id;
    uint32_t value;
};

struct __attribute__((packed)) bi_id_and_int_t {
    uint16_t type;
    uint16_t tag;
    uint32_t id;
    uint32_t value;
};

#endif /* RP2350_BINARY_INFO_H */
