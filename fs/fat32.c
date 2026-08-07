#include "fs/fat32.h"
#include "kernel/printk.h"
#include <string.h>

static void filename_to_83(const char *src, char *dst) {
    if (!src || !dst) return;
    memset(dst, ' ', 11);
    int i = 0;
    while (*src && *src != '.' && *src != '/' && i < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i++] = c;
    }
    if (*src == '.') {
        src++;
        int j = 8;
        while (*src && *src != '/' && j < 11) {
            char c = *src++;
            if (c >= 'a' && c <= 'z') c -= 32;
            dst[j++] = c;
        }
    }
}

static uint32_t cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    if (!fs) return 0;
    return fs->data_start_sector + (cluster - 2) * fs->bpb.sec_per_clus;
}

/* Read a 32-bit FAT entry for the given cluster */
static uint32_t fat_get_entry(fat32_fs_t *fs, uint32_t cluster) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return 0x0FFFFFFF;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    uint8_t fat_sec[512];
    fs->dev->read_blocks(fs->dev, fat_sec, fat_sector, 1);
    return ((uint32_t *)fat_sec)[fat_offset / 4] & 0x0FFFFFFF;
}

/* Write a 32-bit FAT entry for the given cluster (both FAT1 and FAT2) */
static void fat_set_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fs->dev || !fs->dev->write_blocks) return;
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    uint8_t fat_sec[512];
    fs->dev->read_blocks(fs->dev, fat_sec, fat_sector, 1);
    ((uint32_t *)fat_sec)[fat_offset / 4] = value;
    fs->dev->write_blocks(fs->dev, fat_sec, fat_sector, 1);      // FAT1
    fs->dev->write_blocks(fs->dev, fat_sec, fat_sector + 8, 1);  // FAT2
}

/* Dynamically allocate a free cluster from the FAT table */
static uint32_t fat_alloc_cluster(fat32_fs_t *fs) {
    if (!fs) return 0;
    uint32_t total = fs->bpb.tot_sec32 ? fs->bpb.tot_sec32 : 1024;
    for (uint32_t c = 3; c < total; c++) {
        if (fat_get_entry(fs, c) == 0x00000000) {
            fat_set_entry(fs, c, 0x0FFFFFFF);
            return c;
        }
    }
    return 0;
}

/* Extract parent cluster and target file/directory name from a path */
static uint32_t fat32_get_parent_cluster(fat32_fs_t *fs, const char *path, char *out_name) {
    if (!fs || !path) return 0;
    while (*path == '/') path++;

    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    uint32_t cur_clus = fs->root_dir_cluster;
    char *curr = path_copy;

    while (*curr) {
        char *slash = strchr(curr, '/');
        if (!slash) {
            if (out_name) strncpy(out_name, curr, 63);
            return cur_clus;
        }

        *slash = '\0';
        if (strlen(curr) > 0) {
            char name83[11];
            filename_to_83(curr, name83);

            uint32_t clus_iter = cur_clus;
            uint32_t found_clus = 0;

            while (clus_iter < 0x0FFFFFF8 && found_clus == 0) {
                uint32_t lba = cluster_to_lba(fs, clus_iter);
                uint8_t sector[512];
                fs->dev->read_blocks(fs->dev, sector, lba, 1);

                fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
                for (int i = 0; i < 16; i++) {
                    if (entries[i].name[0] == 0x00) break;
                    if ((uint8_t)entries[i].name[0] == 0xE5) continue;
                    if ((entries[i].attr & 0x0F) == 0x0F) continue;
                    if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue;

                    if (memcmp(entries[i].name, name83, 11) == 0) {
                        if (entries[i].attr & FAT32_ATTR_DIRECTORY) {
                            found_clus = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
                            if (found_clus == 0) found_clus = fs->root_dir_cluster;
                        }
                        break;
                    }
                }
                clus_iter = fat_get_entry(fs, clus_iter);
            }

            if (found_clus == 0) return 0; // Component not found
            cur_clus = found_clus;
        }
        curr = slash + 1;
    }

    if (out_name) out_name[0] = '\0';
    return cur_clus;
}

int fat32_format(block_dev_t *dev) {
    if (!dev || !dev->write_blocks) return -1;
    uint8_t sector[512];
    memset(sector, 0, 512);

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector;
    bpb->jmp_boot[0] = 0xEB; bpb->jmp_boot[1] = 0x58; bpb->jmp_boot[2] = 0x90;
    memcpy(bpb->oem_name, "MSWIN4.1", 8);
    bpb->bytes_per_sec = 512;
    bpb->sec_per_clus = 1;
    bpb->reserved_sec_cnt = 32;
    bpb->num_fats = 2;
    bpb->tot_sec32 = dev->num_blocks;
    bpb->fat_sz32 = 8;
    bpb->root_clus = 2;
    bpb->boot_sig = 0x29;
    memcpy(bpb->vol_lab, "LUGALOS_FAT", 11);
    memcpy(bpb->fil_sys_type, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    dev->write_blocks(dev, sector, 0, 1);

    memset(sector, 0, 512);
    uint32_t *fat = (uint32_t *)sector;
    fat[0] = 0x0FFFFFF8;
    fat[1] = 0x0FFFFFFF;
    fat[2] = 0x0FFFFFFF;

    dev->write_blocks(dev, sector, 32, 1);
    dev->write_blocks(dev, sector, 40, 1);

    memset(sector, 0, 512);
    dev->write_blocks(dev, sector, 48, 1);

    printk("[FAT32] Device '%s': Volume formatted cleanly as FAT32.\n", dev->name ? dev->name : "unknown");
    return 0;
}

int fat32_init(fat32_fs_t *fs, block_dev_t *dev) {
    if (!fs || !dev || !dev->read_blocks) return -1;
    fs->dev = dev;

    uint8_t sector[512];
    dev->read_blocks(dev, sector, 0, 1);

    uint32_t partition_lba = 0;

    /* Check if LBA 0 is an MBR Master Boot Record with partition table at offset 0x1BE */
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        uint8_t *part1 = &sector[0x01BE];
        uint8_t part_type = part1[4];
        uint32_t start_lba = (uint32_t)part1[8] | ((uint32_t)part1[9] << 8) |
                             ((uint32_t)part1[10] << 16) | ((uint32_t)part1[11] << 24);

        if ((part_type == 0x0B || part_type == 0x0C || part_type == 0x07 || part_type == 0x0E) && start_lba > 0 && start_lba < 0x0FFFFFFF) {
            partition_lba = start_lba;
            dev->read_blocks(dev, sector, partition_lba, 1);
            printk("[FAT32] Device '%s': Parsed MBR Partition 1 at LBA %u (Type 0x%02X)\n",
                   dev->name ? dev->name : "unknown", (unsigned int)partition_lba, part_type);
        }
    }

    memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));

    if (fs->bpb.boot_sig != 0x29 && sector[510] != 0x55) {
        printk("[FAT32] Device '%s': Invalid boot sector. Auto-formatting FAT32 volume...\n", dev->name ? dev->name : "unknown");
        fat32_format(dev);
        partition_lba = 0;
        dev->read_blocks(dev, sector, 0, 1);
        memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));
    }

    fs->fat_start_sector = partition_lba + fs->bpb.reserved_sec_cnt;
    fs->data_start_sector = fs->fat_start_sector + (fs->bpb.num_fats * fs->bpb.fat_sz32);
    fs->root_dir_cluster = fs->bpb.root_clus;
    fs->bytes_per_cluster = fs->bpb.sec_per_clus * fs->bpb.bytes_per_sec;

    printk("[FAT32] Device '%s': Volume Mounted. Label: '%.11s', Data LBA: %u\n",
           dev->name ? dev->name : "unknown", fs->bpb.vol_lab, (unsigned int)fs->data_start_sector);
    return 0;
}


int fat32_find_file(fat32_fs_t *fs, const char *path, fat32_dir_entry_t *out_entry) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks) return -1;
    while (*path == '/') path++;
    if (*path == '\0') {
        if (out_entry) {
            memset(out_entry, 0, sizeof(fat32_dir_entry_t));
            out_entry->attr = FAT32_ATTR_DIRECTORY;
            out_entry->fst_clus_hi = (uint16_t)(fs->root_dir_cluster >> 16);
            out_entry->fst_clus_lo = (uint16_t)(fs->root_dir_cluster & 0xFFFF);
        }
        return 0;
    }

    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    uint32_t clus_iter = parent_clus;
    while (clus_iter < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(fs, clus_iter);
        uint8_t sector[512];
        fs->dev->read_blocks(fs->dev, sector, lba, 1);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if ((entries[i].attr & 0x0F) == 0x0F) continue;
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue;

            if (memcmp(entries[i].name, name83, 11) == 0) {
                if (out_entry) *out_entry = entries[i];
                return i;
            }
        }
        clus_iter = fat_get_entry(fs, clus_iter);
    }
    return -1;
}

int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size) {
    if (!fs || !entry || !buf || !fs->dev || !fs->dev->read_blocks) return -1;
    if (max_size == 0) return 0;
    uint32_t cluster = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
    /* Reserve the last byte of the caller's buffer for the NUL terminator
     * written below, regardless of what max_size the caller passed. Most
     * callers already pass sizeof(buf) - 1 themselves, but at least one
     * (arch/riscv/common/elf.c) passed sizeof(file_buf) directly, which
     * wrote one byte past the end of its buffer whenever the file being
     * read was >= that size (see B7 in
     * plan/2026-08-07_review_and_remediation.md). Clamping here makes the
     * function safe regardless of what any given caller passes. */
    uint32_t capacity = max_size - 1;
    uint32_t size = entry->file_size < capacity ? entry->file_size : capacity;

    uint32_t bytes_read = 0;
    while (cluster < 0x0FFFFFF8 && bytes_read < size) {
        uint32_t lba = cluster_to_lba(fs, cluster);
        uint8_t data_sec[512];
        fs->dev->read_blocks(fs->dev, data_sec, lba, 1);

        uint32_t chunk = size - bytes_read;
        if (chunk > 512) chunk = 512;
        memcpy((uint8_t *)buf + bytes_read, data_sec, chunk);
        bytes_read += chunk;

        cluster = fat_get_entry(fs, cluster);
    }
    ((char *)buf)[bytes_read] = '\0';
    return (int)bytes_read;
}

int fat32_write_file(fat32_fs_t *fs, const char *path, const void *buf, uint32_t size) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    uint8_t sector[512];
    uint32_t parent_lba = cluster_to_lba(fs, parent_clus);
    fs->dev->read_blocks(fs->dev, sector, parent_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    int free_slot = -1;

    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
            if (free_slot == -1) free_slot = i;
            if (entries[i].name[0] == 0x00) break;
        } else if (memcmp(entries[i].name, name83, 11) == 0) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) return -1;

    uint32_t sectors_needed = (size + 511) / 512;
    if (sectors_needed == 0) sectors_needed = 1;

    uint32_t first_cluster = 0;
    uint32_t cur_clus = 0;

    for (uint32_t s = 0; s < sectors_needed; s++) {
        uint32_t next_clus = fat_alloc_cluster(fs);
        if (next_clus == 0) return -1;

        if (s == 0) {
            first_cluster = next_clus;
        } else {
            fat_set_entry(fs, cur_clus, next_clus);
        }
        cur_clus = next_clus;

        uint32_t data_lba = cluster_to_lba(fs, cur_clus);
        uint8_t data_sec[512];
        memset(data_sec, 0, 512);
        uint32_t offset = s * 512;
        uint32_t chunk = (size > offset) ? (size - offset) : 0;
        if (chunk > 512) chunk = 512;
        if (buf && chunk > 0) memcpy(data_sec, (const uint8_t *)buf + offset, chunk);
        fs->dev->write_blocks(fs->dev, data_sec, data_lba, 1);
    }
    fat_set_entry(fs, cur_clus, 0x0FFFFFFF);

    memcpy(entries[free_slot].name, name83, 11);
    entries[free_slot].attr = FAT32_ATTR_ARCHIVE;
    entries[free_slot].fst_clus_hi = (uint16_t)(first_cluster >> 16);
    entries[free_slot].fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    entries[free_slot].file_size = size;

    fs->dev->write_blocks(fs->dev, sector, parent_lba, 1);
    return 0;
}

int fat32_mkdir(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char new_dir_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, new_dir_name);
    if (parent_clus == 0 || new_dir_name[0] == '\0') return -1;

    fat32_dir_entry_t existing;
    if (fat32_find_file(fs, path, &existing) >= 0) return -1;

    uint32_t new_clus = fat_alloc_cluster(fs);
    if (new_clus == 0) return -1;

    uint8_t dir_sec[512];
    memset(dir_sec, 0, 512);

    fat32_dir_entry_t *dot = (fat32_dir_entry_t *)&dir_sec[0];
    memcpy(dot->name, ".          ", 11);
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->fst_clus_hi = (uint16_t)(new_clus >> 16);
    dot->fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);

    fat32_dir_entry_t *dotdot = (fat32_dir_entry_t *)&dir_sec[32];
    memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    uint32_t up_clus = (parent_clus == fs->root_dir_cluster) ? 0 : parent_clus;
    dotdot->fst_clus_hi = (uint16_t)(up_clus >> 16);
    dotdot->fst_clus_lo = (uint16_t)(up_clus & 0xFFFF);

    fs->dev->write_blocks(fs->dev, dir_sec, cluster_to_lba(fs, new_clus), 1);

    char name83[11];
    filename_to_83(new_dir_name, name83);

    uint8_t sector[512];
    uint32_t parent_lba = cluster_to_lba(fs, parent_clus);
    fs->dev->read_blocks(fs->dev, sector, parent_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0) return -1;

    memcpy(entries[free_slot].name, name83, 11);
    entries[free_slot].attr = FAT32_ATTR_DIRECTORY;
    entries[free_slot].fst_clus_hi = (uint16_t)(new_clus >> 16);
    entries[free_slot].fst_clus_lo = (uint16_t)(new_clus & 0xFFFF);
    entries[free_slot].file_size = 0;

    fs->dev->write_blocks(fs->dev, sector, parent_lba, 1);
    printk("[FAT32] Device '%s': Subdirectory created: '%s' (Cluster %u)\n",
           fs->dev->name ? fs->dev->name : "unknown", new_dir_name, (unsigned int)new_clus);
    return 0;

}

int fat32_remove_file(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char target_name[64];
    uint32_t parent_clus = fat32_get_parent_cluster(fs, path, target_name);
    if (parent_clus == 0 || target_name[0] == '\0') return -1;

    char name83[11];
    filename_to_83(target_name, name83);

    uint8_t sector[512];
    uint32_t parent_lba = cluster_to_lba(fs, parent_clus);
    fs->dev->read_blocks(fs->dev, sector, parent_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00) break;
        if ((uint8_t)entries[i].name[0] == 0xE5) continue;

        if (memcmp(entries[i].name, name83, 11) == 0) {
            entries[i].name[0] = 0xE5; // Mark deleted
            fs->dev->write_blocks(fs->dev, sector, parent_lba, 1);
            return 0;
        }
    }
    return -1;
}

int fat32_rmdir(fat32_fs_t *fs, const char *path) {
    if (!fs || !path || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    fat32_dir_entry_t entry;
    if (fat32_find_file(fs, path, &entry) < 0) return -1;

    if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
        printk("rmdir: '%s' is not a directory\n", path);
        return -1;
    }

    uint32_t dir_clus = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
    if (dir_clus == 0 || dir_clus == fs->root_dir_cluster) {
        printk("rmdir: cannot remove root directory\n");
        return -1;
    }

    /* Check if directory is empty (ignoring '.' and '..') */
    uint32_t clus_iter = dir_clus;
    while (clus_iter < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(fs, clus_iter);
        uint8_t sector[512];
        fs->dev->read_blocks(fs->dev, sector, lba, 1);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;

            if (memcmp(entries[i].name, ".          ", 11) == 0 ||
                memcmp(entries[i].name, "..         ", 11) == 0) {
                continue;
            }

            printk("rmdir: directory '%s' is not empty\n", path);
            return -1;
        }
        clus_iter = fat_get_entry(fs, clus_iter);
    }

    /* Free directory cluster chain in FAT */
    clus_iter = dir_clus;
    while (clus_iter < 0x0FFFFFF8) {
        uint32_t next = fat_get_entry(fs, clus_iter);
        fat_set_entry(fs, clus_iter, 0x00000000);
        clus_iter = next;
    }

    /* Mark entry in parent directory deleted */
    return fat32_remove_file(fs, path);
}

int fat32_statfs(fat32_fs_t *fs, uint32_t *total_bytes, uint32_t *free_bytes) {
    if (!fs || !fs->dev) return -1;
    uint32_t total_sec = fs->bpb.tot_sec32 ? fs->bpb.tot_sec32 : fs->bpb.tot_sec16;
    uint32_t total_sz = total_sec * fs->bpb.bytes_per_sec;
    if (total_bytes) *total_bytes = total_sz;

    uint32_t free_clusters = 0;
    uint32_t total_clusters = (total_sec > fs->data_start_sector) ?
        ((total_sec - fs->data_start_sector) / fs->bpb.sec_per_clus) : 0;
    uint32_t fat_sec = fs->fat_start_sector;
    uint32_t fat_buf[128];

    uint32_t checked = 0;
    while (checked < total_clusters && (fat_sec < fs->data_start_sector)) {
        if (fs->dev->read_blocks(fs->dev, fat_buf, fat_sec, 1) != 0) break;
        for (int i = 0; i < 128 && checked < total_clusters; i++, checked++) {
            uint32_t val = fat_buf[i] & 0x0FFFFFFF;
            if (val == 0) free_clusters++;
        }
        fat_sec++;
    }

    if (free_bytes) *free_bytes = free_clusters * fs->bytes_per_cluster;
    return 0;
}


void fat32_list_dir(fat32_fs_t *fs, const char *path) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return;

    uint32_t target_clus = fs->root_dir_cluster;
    if (path && *path && strcmp(path, "/") != 0) {
        fat32_dir_entry_t entry;
        if (fat32_find_file(fs, path, &entry) < 0) {
            printk("ls: path '%s' not found\n", path);
            return;
        }
        if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
            printk("ls: '%s' is not a directory\n", path);
            return;
        }
        target_clus = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
        if (target_clus == 0) target_clus = fs->root_dir_cluster;
    }

    printk("\nDirectory Listing (FAT32):\n");
    printk("Name        Size (Bytes)  Attr   Type\n");
    printk("----------  ------------  -----  -----\n");

    int count = 0;
    uint32_t clus_iter = target_clus;

    while (clus_iter < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(fs, clus_iter);
        uint8_t sector[512];
        fs->dev->read_blocks(fs->dev, sector, lba, 1);

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00) break;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if ((entries[i].attr & 0x0F) == 0x0F) continue; // Skip VFAT Long File Name (LFN) metadata
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue; // Skip Volume Label entry

            char namebuf[13];
            memcpy(namebuf, entries[i].name, 11);
            namebuf[11] = '\0';
            bool is_dir = (entries[i].attr & FAT32_ATTR_DIRECTORY) != 0;
            printk("%s  %12u  0x%02x   %s\n",
                   namebuf,
                   (unsigned int)entries[i].file_size,
                   entries[i].attr,
                   is_dir ? "<DIR>" : "<FILE>");
            count++;
        }
        clus_iter = fat_get_entry(fs, clus_iter);
    }
    if (count == 0) {
        printk("(empty directory)\n");
    }
    printk("\n");
}
