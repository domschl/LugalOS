#include "fs/fat32.h"
#include "kernel/printk.h"
#include <string.h>

static void filename_to_83(const char *src, char *dst) {
    if (!src || !dst) return;
    memset(dst, ' ', 11);
    int i = 0;
    while (*src && *src != '.' && i < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i++] = c;
    }
    if (*src == '.') {
        src++;
        int j = 8;
        while (*src && j < 11) {
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
    bpb->fat_sz32 = 8; // 8 sectors per FAT table
    bpb->root_clus = 2;
    bpb->boot_sig = 0x29;
    memcpy(bpb->vol_lab, "LUGALOS_FAT", 11);
    memcpy(bpb->fil_sys_type, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    /* Write Boot Sector */
    dev->write_blocks(dev, sector, 0, 1);

    /* Initialize FAT1 & FAT2 Tables (Cluster 0 & 1 reserved, Cluster 2 EOF for root) */
    memset(sector, 0, 512);
    uint32_t *fat = (uint32_t *)sector;
    fat[0] = 0x0FFFFFF8; // Media type
    fat[1] = 0x0FFFFFFF; // Clean shutdown bit
    fat[2] = 0x0FFFFFFF; // Root cluster EOF

    dev->write_blocks(dev, sector, 32, 1); // FAT1
    dev->write_blocks(dev, sector, 40, 1); // FAT2

    /* Clear Root Directory Cluster (Sector 48) */
    memset(sector, 0, 512);
    dev->write_blocks(dev, sector, 48, 1);

    printk("[FAT32] Volume formatted cleanly as FAT32.\n");
    return 0;
}

int fat32_init(fat32_fs_t *fs, block_dev_t *dev) {
    if (!fs || !dev || !dev->read_blocks) return -1;
    fs->dev = dev;

    uint8_t sector[512];
    dev->read_blocks(dev, sector, 0, 1);
    memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));

    if (fs->bpb.boot_sig != 0x29 && sector[510] != 0x55) {
        printk("[FAT32] Invalid boot sector. Auto-formatting FAT32 volume...\n");
        fat32_format(dev);
        dev->read_blocks(dev, sector, 0, 1);
        memcpy(&fs->bpb, sector, sizeof(fat32_bpb_t));
    }

    fs->fat_start_sector = fs->bpb.reserved_sec_cnt;
    fs->data_start_sector = fs->fat_start_sector + (fs->bpb.num_fats * fs->bpb.fat_sz32);
    fs->root_dir_cluster = fs->bpb.root_clus;
    fs->bytes_per_cluster = fs->bpb.sec_per_clus * fs->bpb.bytes_per_sec;

    printk("[FAT32] Volume Mounted. Label: '%.11s', Data LBA: %u\n",
           fs->bpb.vol_lab, (unsigned int)fs->data_start_sector);
    return 0;
}

int fat32_find_file(fat32_fs_t *fs, const char *filename, fat32_dir_entry_t *out_entry) {
    if (!fs || !filename || !fs->dev || !fs->dev->read_blocks) return -1;
    char name83[11];
    filename_to_83(filename, name83);

    uint8_t sector[512];
    uint32_t root_lba = cluster_to_lba(fs, fs->root_dir_cluster);
    fs->dev->read_blocks(fs->dev, sector, root_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00) break; // End of entries
        if ((uint8_t)entries[i].name[0] == 0xE5) continue; // Deleted

        if (memcmp(entries[i].name, name83, 11) == 0) {
            if (out_entry) *out_entry = entries[i];
            return i;
        }
    }
    return -1;
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

int fat32_read_file(fat32_fs_t *fs, fat32_dir_entry_t *entry, void *buf, uint32_t max_size) {
    if (!fs || !entry || !buf || !fs->dev || !fs->dev->read_blocks) return -1;
    uint32_t cluster = ((uint32_t)entry->fst_clus_hi << 16) | entry->fst_clus_lo;
    uint32_t size = entry->file_size < max_size ? entry->file_size : max_size;

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

int fat32_write_file(fat32_fs_t *fs, const char *filename, const void *buf, uint32_t size) {
    if (!fs || !filename || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    char name83[11];
    filename_to_83(filename, name83);

    uint8_t sector[512];
    uint32_t root_lba = cluster_to_lba(fs, fs->root_dir_cluster);
    fs->dev->read_blocks(fs->dev, sector, root_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    int free_slot = -1;
    uint32_t old_cluster = 0;

    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
            if (free_slot == -1) free_slot = i;
            if (entries[i].name[0] == 0x00) break;
        } else if (memcmp(entries[i].name, name83, 11) == 0) {
            free_slot = i;
            old_cluster = ((uint32_t)entries[i].fst_clus_hi << 16) | entries[i].fst_clus_lo;
            break;
        }
    }

    if (free_slot == -1) return -1;

    /* Find a free starting cluster (scan FAT for 0x00000000 = free) */
    /* For simplicity: assign cluster = 3 + free_slot (1 cluster per directory slot) */
    /* For multi-sector files, chain additional free clusters */
    uint32_t sectors_needed = (size + 511) / 512;
    if (sectors_needed == 0) sectors_needed = 1;

    /* Find 'sectors_needed' consecutive free clusters starting after existing content */
    /* Simple strategy: start cluster from a non-conflicting region */
    /* Each file slot gets a base cluster 3+slot, extended by 16*slot for gap */
    uint32_t base_cluster = 3 + (uint32_t)free_slot * 16;

    uint32_t first_cluster = base_cluster;
    uint32_t cur_clus = first_cluster;

    for (uint32_t s = 0; s < sectors_needed; s++) {
        uint32_t data_lba = cluster_to_lba(fs, cur_clus);
        uint8_t data_sec[512];
        memset(data_sec, 0, 512);
        uint32_t offset = s * 512;
        uint32_t chunk = (size > offset) ? (size - offset) : 0;
        if (chunk > 512) chunk = 512;
        if (buf && chunk > 0) memcpy(data_sec, (const uint8_t *)buf + offset, chunk);
        fs->dev->write_blocks(fs->dev, data_sec, data_lba, 1);

        if (s + 1 < sectors_needed) {
            fat_set_entry(fs, cur_clus, cur_clus + 1); // Link to next cluster
            cur_clus++;
        } else {
            fat_set_entry(fs, cur_clus, 0x0FFFFFFF); // EOF marker
        }
    }

    /* Update directory entry */
    memcpy(entries[free_slot].name, name83, 11);
    entries[free_slot].attr = FAT32_ATTR_ARCHIVE;
    entries[free_slot].fst_clus_hi = (uint16_t)(first_cluster >> 16);
    entries[free_slot].fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    entries[free_slot].file_size = size;

    /* Write back updated root directory */
    fs->dev->write_blocks(fs->dev, sector, root_lba, 1);
    return 0;
}


int fat32_remove_file(fat32_fs_t *fs, const char *filename) {
    if (!fs || !filename || !fs->dev || !fs->dev->read_blocks || !fs->dev->write_blocks) return -1;
    fat32_dir_entry_t entry;
    int idx = fat32_find_file(fs, filename, &entry);
    if (idx < 0) return -1;

    uint8_t sector[512];
    uint32_t root_lba = cluster_to_lba(fs, fs->root_dir_cluster);
    fs->dev->read_blocks(fs->dev, sector, root_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    entries[idx].name[0] = 0xE5; // Mark deleted

    fs->dev->write_blocks(fs->dev, sector, root_lba, 1);
    return 0;
}

void fat32_list_dir(fat32_fs_t *fs) {
    if (!fs || !fs->dev || !fs->dev->read_blocks) return;
    uint8_t sector[512];
    uint32_t root_lba = cluster_to_lba(fs, fs->root_dir_cluster);
    fs->dev->read_blocks(fs->dev, sector, root_lba, 1);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)sector;
    printk("\nDirectory Listing (FAT32):\n");
    printk("Name        Size (Bytes)  Attr\n");
    printk("----------  ------------  -----\n");

    int count = 0;
    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00) break;
        if ((uint8_t)entries[i].name[0] == 0xE5) continue;

        char namebuf[13];
        memcpy(namebuf, entries[i].name, 11);
        namebuf[11] = '\0';
        printk("%s  %12u  0x%x\n", namebuf, (unsigned int)entries[i].file_size, entries[i].attr);
        count++;
    }
    if (count == 0) {
        printk("(empty directory)\n");
    }
    printk("\n");
}
