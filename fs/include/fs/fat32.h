#ifndef LUGALOS_FS_FAT32_H
#define LUGALOS_FS_FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/block.h"

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

#define FAT32_EOF 0x0FFFFFF8

#pragma pack(push, 1)
typedef struct {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t reserved_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec16;
    uint8_t  media;
    uint16_t fat_sz16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec32;

    /* FAT32 Extended Fields */
    uint32_t fat_sz32;
    uint16_t ext_flags;
    uint16_t fs_ver;
    uint32_t root_clus;
    uint16_t fs_info;
    uint16_t bk_boot_sec;
    uint8_t  reserved[12];
    uint8_t  drv_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t vol_id;
    char     vol_lab[11];
    char     fil_sys_type[8];
} fat32_bpb_t;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat32_dir_entry_t;
#pragma pack(pop)

typedef struct {
    block_dev_t *dev;
    fat32_bpb_t bpb;
    uint32_t fat_start_sector;
    uint32_t data_start_sector;
    uint32_t root_dir_cluster;
    uint32_t bytes_per_cluster;
} fat32_fs_t;

int fat32_init(fat32_fs_t *fs, block_dev_t *dev);
int fat32_format(block_dev_t *dev);
int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry);
int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size);
int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size);
int fat32_mkdir(fat32_fs_t *fs, const char *path);
int fat32_rmdir(fat32_fs_t *fs, const char *path);
int fat32_remove_file(fat32_fs_t *fs, const char *path);
void fat32_list_dir(fat32_fs_t *fs, const char *path);


#endif /* LUGALOS_FS_FAT32_H */

