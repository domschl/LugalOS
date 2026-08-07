#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/block.h"
#include "drivers/flashdisk.h"
#include "drivers/uart.h"
#include "drivers/at24c32.h"
#include "drivers/loopback_net.h"
#include "drivers/uart_net.h"
#include "kernel/printk.h"
#include "kernel/ipc.h"
#include "kernel/sched.h"
#include "kernel/version.h"
#include <string.h>
#include <stdbool.h>

static fat32_fs_t g_fat32_sd;
static fat32_fs_t g_fat32_ram;
static fat32_fs_t g_fat32_flash;
static bool g_sd_mounted = false;
static bool g_ram_mounted = false;
static bool g_flash_mounted = false;

typedef struct {
    char name[32];
    int target_pid;
} service_entry_t;

#define MAX_SERVICES 8
static service_entry_t g_services[MAX_SERVICES];
static int g_num_services = 0;

/* --- Handle table (A1, plan/phase5_distributed_design.md) ---
 * A flat array, not a union: at 8 handles this is ~5.5KB (dominated by the
 * 512-byte proc_buf per slot), which is affordable even on RP2350's tight
 * SRAM budget and keeps every field trivially inspectable. `type` is one of
 * parse_prefix()'s return values (0=root, 1=/sd0, 2=/ram0, 3=/proc, 4=/dev,
 * 6=/flash0) -- 5=/srv/ never appears here, since /srv/ stays on its own
 * direct-dispatch path in vfs_read()/vfs_write() (see fs/include/fs/vfs.h). */
typedef struct {
    bool in_use;
    int type;
    int flags;
    bool is_dir;
    fat32_fs_t *fs;              /* valid for type 1/2/6 */
    fat32_dir_entry_t entry;     /* valid for type 1/2/6, non-dir */
    uint32_t dir_cluster;        /* valid for type 1/2/6, is_dir */
    char rel_path[128];          /* path within volume (type 1/2/6) or device name (type 4) */
    char proc_buf[512];          /* generated /proc file content (type 3, non-dir) */
    uint32_t proc_len;
} vfs_handle_t;

static vfs_handle_t g_handles[VFS_MAX_HANDLES];

#include "drivers/spisd.h"

static int vfs_mount_flashdisk(void) {
    block_dev_t *flash_dev = flashdisk_get_device();
    if (flash_dev) {
        if (fat32_init(&g_fat32_flash, flash_dev) == 0) {
            g_flash_mounted = true;
            printk("[VFS Server] Mounted FAT32 Filesystem on /flash0/ (Device: Embedded Flash ROMDisk, Size: %d KB)\n",
                   (flash_dev->num_blocks * flash_dev->block_size) / 1024);
            return 0;
        }
    }
    return -1;
}

void vfs_server_init(void) {
    g_sd_mounted = false;
    g_ram_mounted = false;
    g_flash_mounted = false;

    /* Mount embedded Flash ROM filesystem on /flash0/ */
    vfs_mount_flashdisk();

#if defined(CONFIG_BOARD_RP2350)
    /* Try mounting physical MicroSD card via SPI1 (GP10-GP13) on RP2350 */
    block_dev_t *sd_dev = spisd_get_device();
    if (sd_dev) {
        if (fat32_init(&g_fat32_sd, sd_dev) == 0) {
            g_sd_mounted = true;
            printk("[VFS Server] Mounted FAT32 Filesystem on /sd0/ (Device: SPI1 MicroSD Card Reader)\n");
        }
    }
#else
    /* VirtIO block device is available on QEMU targets */
    block_dev_t *sd_dev = virtio_blk_get_device();
    if (sd_dev) {
        if (fat32_init(&g_fat32_sd, sd_dev) == 0) {
            g_sd_mounted = true;
            printk("[VFS Server] Mounted FAT32 Filesystem on /sd0/ (Device: VirtIO SD Block Engine)\n");
        }
    }
#endif

    g_num_services = 0;
    vfs_register_service("lisp", 2);
    loopback_net_init();
    vfs_register_service("p9_loopback", 9);
    uart_net_init();
    vfs_register_service("uart_9p", 10);

    printk("[VFS Server] Universal Namespace Resolver (Plan 9 Model) initialized (PID %d).\n", VFS_PID);
}

int vfs_mount_ramdisk(int size_kb) {
    block_dev_t *ram_dev = ramdisk_get_device();
    if (ram_dev) {
        if (size_kb > 0) {
            uint32_t requested_blocks = ((uint32_t)size_kb * 1024) / ram_dev->block_size;
            uint32_t max_blocks = ramdisk_max_blocks();
            if (requested_blocks > max_blocks) {
                uint32_t max_kb = (max_blocks * ram_dev->block_size) / 1024;
                printk("[VFS Server] Warning: requested /ram0/ size %d KB exceeds physical "
                       "RAMDisk capacity (%u KB); clamping.\n", size_kb, (unsigned int)max_kb);
                requested_blocks = max_blocks;
                size_kb = (int)max_kb;
            }
            ram_dev->num_blocks = requested_blocks;
        }
        if (fat32_init(&g_fat32_ram, ram_dev) != 0 || g_fat32_ram.bpb.tot_sec32 != ram_dev->num_blocks) {
            fat32_format(ram_dev);
            fat32_init(&g_fat32_ram, ram_dev);
        }

        g_ram_mounted = true;
        printk("[VFS Server] Mounted FAT32 Filesystem on /ram0/ (Device: RAMDisk Engine, Size: %d KB)\n", size_kb);
        return 0;
    }
    return -1;
}

int vfs_register_service(const char *service_name, int target_pid) {
    if (!service_name || g_num_services >= MAX_SERVICES) return -1;
    strncpy(g_services[g_num_services].name, service_name, 31);
    g_services[g_num_services].name[31] = '\0';
    g_services[g_num_services].target_pid = target_pid;
    g_num_services++;
    return 0;
}

/* Parse prefix:
 * 0: Root "/"
 * 1: "/sd0/" (VirtIO SD Card Storage)
 * 2: "/ram0/" (In-Memory RAMDisk Storage)
 * 3: "/proc/" (Metrics)
 * 4: "/dev/" (Hardware Devices)
 * 5: "/srv/" (IPC Services)
 * 6: "/flash0/" (Embedded Flash ROM Storage)
 */
static int parse_prefix(const char *path, const char **rel_path) {
    static const char *empty_str = "";
    if (!rel_path) rel_path = &empty_str;

    if (!path || strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        *rel_path = empty_str;
        return 0; // Root mount table
    }

    if (strncmp(path, "/flash0/", 8) == 0) {
        *rel_path = path + 8;
        return 6;
    } else if (strcmp(path, "/flash0") == 0) {
        *rel_path = empty_str;
        return 6;
    } else if (strncmp(path, "/sd0/", 5) == 0) {
        *rel_path = path + 5;
        return 1;
    } else if (strcmp(path, "/sd0") == 0) {
        *rel_path = empty_str;
        return 1;
    } else if (strncmp(path, "/ram0/", 6) == 0) {
        *rel_path = path + 6;
        return 2;
    } else if (strcmp(path, "/ram0") == 0) {
        *rel_path = empty_str;
        return 2;
    } else if (strncmp(path, "/proc/", 6) == 0) {
        *rel_path = path + 6;
        return 3;
    } else if (strcmp(path, "/proc") == 0) {
        *rel_path = empty_str;
        return 3;
    } else if (strncmp(path, "/dev/", 5) == 0) {
        *rel_path = path + 5;
        return 4;
    } else if (strcmp(path, "/dev") == 0) {
        *rel_path = empty_str;
        return 4;
    } else if (strncmp(path, "/srv/", 5) == 0) {
        *rel_path = path + 5;
        return 5;
    } else if (strcmp(path, "/srv") == 0) {
        *rel_path = empty_str;
        return 5;
    }

    if (path[0] == '/') {
        *rel_path = path + 1;
    } else {
        *rel_path = path;
    }
    return 6; // Default un-prefixed paths to /flash0/ first
}

/* Generates the current content of a /proc/<name> virtual file directly into
 * a caller-owned buffer using ksnprintf() instead of printk() -- so /proc
 * files are real, readable byte streams a caller (eventually a remote 9P
 * client) can vfs_pread() in pieces, not a printk() side effect (see V5 in
 * plan/completed/2026-08-07_review_and_remediation.md). Returns the number of
 * bytes generated, or -1 if `rel` doesn't name a known /proc file. */
static int vfs_generate_proc_content(const char *rel, char *buf, uint32_t cap) {
    if (!rel || !buf || cap == 0) return -1;
    uint32_t used = 0;

    if (strcmp(rel, "ps") == 0) {
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "PID  State    Name\n---  -------  ------------\n 0   RUNNING  kernel_idle\n 1   READY    lsh_console\n 2   READY    lisp_engine\n 3   READY    vfs_server (FAT32)\n");
        return (int)used;
    } else if (strcmp(rel, "df") == 0) {
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "Filesystem     512-blocks       Used  Available Capacity Mounted on\n");
        if (g_flash_mounted) {
            uint32_t total = 0, free = 0;
            fat32_statfs(&g_fat32_flash, &total, &free);
            uint32_t total_b = total / 512;
            uint32_t free_b = free / 512;
            uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
            uint32_t pct = total_b ? (used_b * 100 / total_b) : 100;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "/flash0/         %9u  %9u  %9u     %3u%% /flash0/\n", total_b, used_b, free_b, pct);
        }
        if (g_ram_mounted) {
            uint32_t total = 0, free = 0;
            fat32_statfs(&g_fat32_ram, &total, &free);
            uint32_t total_b = total / 512;
            uint32_t free_b = free / 512;
            uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
            uint32_t pct = total_b ? (used_b * 100 / total_b) : 0;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "/ram0/           %9u  %9u  %9u     %3u%% /ram0/\n", total_b, used_b, free_b, pct);
        }
        if (g_sd_mounted) {
            uint32_t total = 0, free = 0;
            fat32_statfs(&g_fat32_sd, &total, &free);
            uint32_t total_b = total / 512;
            uint32_t free_b = free / 512;
            uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
            uint32_t pct = total_b ? (used_b * 100 / total_b) : 0;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "/sd0/            %9u  %9u  %9u     %3u%% /sd0/\n", total_b, used_b, free_b, pct);
        }
        return (int)used;
    } else if (strcmp(rel, "meminfo") == 0) {
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "Heap & Storage Status:\n  Page Size: 4096 bytes\n  VMM Status: Active\n  Storage: /flash0/ (Flash ROM), /sd0/ (VirtIO SD), /ram0/ (RAMDisk)\n");
        return (int)used;
    } else if (strcmp(rel, "version") == 0) {
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "LugalOS v%s (Bare-Metal RISC-V Lisp Machine)\n", LUGALOS_VERSION);
        return (int)used;
    }
    return -1;
}

static const char *g_root_names[6] = { "flash0", "sd0", "ram0", "proc", "dev", "srv" };
static const char *g_proc_names[4] = { "ps", "meminfo", "version", "df" };
static const char *g_dev_names[4]  = { "uart", "null", "zero", "eeprom" };

static fat32_fs_t *vfs_volume_for_type(int type, bool *mounted_out) {
    switch (type) {
        case 6: *mounted_out = g_flash_mounted; return &g_fat32_flash;
        case 1: *mounted_out = g_sd_mounted;    return &g_fat32_sd;
        case 2: *mounted_out = g_ram_mounted;   return &g_fat32_ram;
        default: *mounted_out = false;          return NULL;
    }
}

/* Opens `path` into a fresh handle, returning a small non-negative fd (index
 * into g_handles[]) on success or -1 on failure. /srv/ (type 5, IPC
 * channels) is deliberately not handle-addressable -- see the comment on
 * this API in fs/include/fs/vfs.h. */
int vfs_open(const char *path, int flags) {
    if (!path) return -1;

    int slot = -1;
    for (int i = 0; i < VFS_MAX_HANDLES; i++) {
        if (!g_handles[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    vfs_handle_t *h = &g_handles[slot];
    memset(h, 0, sizeof(*h));

    if (type == 1 || type == 2 || type == 6) {
        bool mounted = false;
        fat32_fs_t *fs = vfs_volume_for_type(type, &mounted);
        if (!mounted) return -1;
        if (type == 6 && (flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC))) return -1; // /flash0/ is read-only

        fat32_dir_entry_t entry;
        if (fat32_find_file(fs, rel, &entry) < 0) {
            if (!(flags & VFS_O_CREATE)) return -1;
            if (fat32_write_file(fs, rel, NULL, 0) != 0) return -1;
            if (fat32_find_file(fs, rel, &entry) < 0) return -1;
        } else if ((flags & VFS_O_TRUNC) && !(entry.attr & FAT32_ATTR_DIRECTORY)) {
            if (fat32_truncate(fs, rel) != 0) return -1;
            if (fat32_find_file(fs, rel, &entry) < 0) return -1;
        }

        h->fs = fs;
        h->entry = entry;
        h->is_dir = (entry.attr & FAT32_ATTR_DIRECTORY) != 0;
        if (h->is_dir) {
            h->dir_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
        }
        strncpy(h->rel_path, rel, sizeof(h->rel_path) - 1);
        h->rel_path[sizeof(h->rel_path) - 1] = '\0';
    } else if (type == 0) { // root "/" -- readdir-only pseudo-directory over the mount table
        h->is_dir = true;
    } else if (type == 3) { // /proc/
        if (rel[0] == '\0') {
            h->is_dir = true;
        } else {
            int len = vfs_generate_proc_content(rel, h->proc_buf, sizeof(h->proc_buf));
            if (len < 0) return -1;
            h->proc_len = (uint32_t)len;
        }
    } else if (type == 4) { // /dev/
        if (rel[0] == '\0') {
            h->is_dir = true;
        } else {
            strncpy(h->rel_path, rel, sizeof(h->rel_path) - 1);
            h->rel_path[sizeof(h->rel_path) - 1] = '\0';
        }
    } else {
        return -1; // /srv/ (type 5) and anything unrecognized
    }

    h->type = type;
    h->flags = flags;
    h->in_use = true;
    return slot;
}

int vfs_pread(int fd, void *buf, uint32_t count, uint64_t offset) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use) return -1;
    vfs_handle_t *h = &g_handles[fd];
    if (h->is_dir) return -1;
    if (count == 0) return 0;
    if (!buf) return -1;

    switch (h->type) {
        case 1: case 2: case 6:
            return fat32_read_at(h->fs, &h->entry, buf, count, offset);
        case 3: {
            if (offset >= h->proc_len) return 0;
            uint32_t avail = h->proc_len - (uint32_t)offset;
            uint32_t n = (avail < count) ? avail : count;
            memcpy(buf, h->proc_buf + offset, n);
            return (int)n;
        }
        case 4:
            if (strcmp(h->rel_path, "uart") == 0) {
                ((char *)buf)[0] = uart_getc();
                return 1;
            } else if (strcmp(h->rel_path, "eeprom") == 0) {
                return at24c32_read((uint16_t)offset, (uint8_t *)buf, count);
            } else if (strcmp(h->rel_path, "null") == 0 || strcmp(h->rel_path, "zero") == 0) {
                return 0;
            }
            return -1;
        default:
            return -1;
    }
}

int vfs_pwrite(int fd, const void *buf, uint32_t count, uint64_t offset) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use) return -1;
    vfs_handle_t *h = &g_handles[fd];
    if (h->is_dir) return -1;
    if (!(h->flags & VFS_O_WRITE)) return -1;
    if (count == 0) return 0;
    if (!buf) return -1;

    switch (h->type) {
        case 1: case 2: {
            int n = fat32_write_at(h->fs, h->rel_path, buf, count, offset);
            if (n > 0) fat32_find_file(h->fs, h->rel_path, &h->entry); // refresh cached size
            return n;
        }
        case 4:
            if (strcmp(h->rel_path, "uart") == 0) {
                const char *str = (const char *)buf;
                for (uint32_t i = 0; i < count; i++) uart_putc(str[i]);
                return (int)count;
            } else if (strcmp(h->rel_path, "eeprom") == 0) {
                return at24c32_write((uint16_t)offset, (const uint8_t *)buf, count);
            } else if (strcmp(h->rel_path, "null") == 0) {
                return (int)count;
            }
            return -1;
        default:
            return -1; // /flash0/ (read-only) and anything else not writable
    }
}

int vfs_readdir(int fd, uint32_t index, char *name_out, uint32_t name_max, vfs_stat_t *stat_out) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use) return -1;
    vfs_handle_t *h = &g_handles[fd];
    if (!h->is_dir) return -1;

    switch (h->type) {
        case 0: {
            if (index >= 6) return -1;
            if (name_out && name_max > 0) {
                strncpy(name_out, g_root_names[index], name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            if (stat_out) { stat_out->size = 0; stat_out->is_dir = 1; }
            return 0;
        }
        case 1: case 2: case 6: {
            fat32_dir_entry_t entry;
            if (fat32_readdir(h->fs, h->dir_cluster, index, name_out, name_max, &entry) != 0) return -1;
            if (stat_out) {
                stat_out->size = entry.file_size;
                stat_out->is_dir = (entry.attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
            }
            return 0;
        }
        case 3: {
            if (index >= 4) return -1;
            if (name_out && name_max > 0) {
                strncpy(name_out, g_proc_names[index], name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
            return 0;
        }
        case 4: {
            if (index >= 4) return -1;
            if (name_out && name_max > 0) {
                strncpy(name_out, g_dev_names[index], name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
            return 0;
        }
        default:
            return -1;
    }
}

int vfs_fstat(int fd, vfs_stat_t *out) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use || !out) return -1;
    vfs_handle_t *h = &g_handles[fd];
    out->is_dir = h->is_dir ? 1 : 0;
    switch (h->type) {
        case 1: case 2: case 6: out->size = h->entry.file_size; break;
        case 3: out->size = h->proc_len; break;
        default: out->size = 0; break;
    }
    return 0;
}

int vfs_stat(const char *path, vfs_stat_t *out) {
    if (!out) return -1;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;
    int r = vfs_fstat(fd, out);
    vfs_close(fd);
    return r;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use) return -1;
    memset(&g_handles[fd], 0, sizeof(g_handles[fd]));
    return 0;
}

/* --- Legacy whole-file API: thin compat wrappers over the handle API above,
 * except the /srv/ branch of vfs_read()/vfs_write(), which bypasses it
 * entirely (message-oriented IPC channels aren't handle-addressable). --- */

int vfs_read(const char *path, void *buf, uint32_t max_len) {
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 5) { // /srv/ IPC channels
        if (strcmp(rel, "p9_loopback") == 0 || strcmp(rel, "loopback_9p") == 0 || strcmp(rel, "p9") == 0 || strcmp(rel, "net") == 0) {
            return loopback_9p_rpc(NULL, (char *)buf, max_len);
        } else if (strcmp(rel, "uart_9p") == 0 || strcmp(rel, "net0") == 0) {
            return uart_net_rpc(NULL, (char *)buf, max_len);
        }
        for (int i = 0; i < g_num_services; i++) {
            if (strcmp(rel, g_services[i].name) == 0) {
                printk("[VFS Router] IPC Channel '/srv/%s' read routed to PID %d\n",
                       g_services[i].name, g_services[i].target_pid);
                return 0;
            }
        }
        return -1;
    }

    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;

    int n;
    if (buf && max_len > 0) {
        uint32_t cap = max_len - 1; // reserve a byte for the NUL, matching the old fat32_read_file() convention
        n = vfs_pread(fd, buf, cap, 0);
        if (n >= 0) ((char *)buf)[n] = '\0';
    } else {
        n = vfs_pread(fd, buf, max_len, 0);
    }
    vfs_close(fd);
    return n;
}

int vfs_write(const char *path, const void *buf, uint32_t len) {
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 6) { // /flash0/
        printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) is read-only\n");
        return -1;
    }

    if (type == 5) { // /srv/ IPC channels
        if (strcmp(rel, "p9_loopback") == 0 || strcmp(rel, "loopback_9p") == 0 || strcmp(rel, "p9") == 0 || strcmp(rel, "net") == 0) {
            return loopback_9p_rpc((const char *)buf, NULL, 0);
        } else if (strcmp(rel, "uart_9p") == 0 || strcmp(rel, "net0") == 0) {
            return uart_net_rpc((const char *)buf, NULL, 0);
        }
        for (int i = 0; i < g_num_services; i++) {
            if (strcmp(rel, g_services[i].name) == 0) {
                printk("[VFS Router] Forwarding %d byte payload to /srv/%s (PID %d) over IPC...\n",
                       len, g_services[i].name, g_services[i].target_pid);

                ipc_msg_t msg_in = { .tag = VFS_TAG_WRITE, .data = { (uintptr_t)buf, len, 0, 0, 0 } };
                ipc_msg_t msg_out = {0};
                sys_ipc_call(g_services[i].target_pid, &msg_in, &msg_out);
                return 0;
            }
        }
        return -1;
    }

    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return -1;
    int n = vfs_pwrite(fd, buf, len, 0);
    vfs_close(fd);
    if (n < 0) return -1;
    /* Preserve the old fat32_write_file()-style "0 on full success" return
     * convention (several Lisp primitives and user/ed/ed.c check `== 0`),
     * even though vfs_pwrite() itself reports an honest byte count. */
    return ((uint32_t)n == len) ? 0 : -1;
}

int vfs_append(const char *path, const void *buf, uint32_t len) {
    if (!path) return -1;
    if (!buf || len == 0) return 0; // matches fat32_append_file()'s own no-op guard

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 6) { // /flash0/
        printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) is read-only\n");
        return -1;
    }
    if (type != 1 && type != 2) return -1;

    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE);
    if (fd < 0) return -1;
    vfs_stat_t st;
    if (vfs_fstat(fd, &st) != 0) { vfs_close(fd); return -1; }
    int n = vfs_pwrite(fd, buf, len, st.size);
    vfs_close(fd);
    return n; // byte-count convention, matching the old fat32_append_file() passthrough
}

/* Explicit volume initialization, replacing the auto-format-on-invalid-boot-
 * sector behavior fat32_init() used to have (see B10 in
 * plan/completed/2026-08-07_review_and_remediation.md) -- a corrupt or blank card
 * inserted at /sd0/ now just fails to mount instead of being silently
 * wiped; this is the only way (aside from vfs_mount_ramdisk()'s own
 * deliberate fallback for the always-starts-blank RAM disk) a volume gets
 * formatted now. */
int vfs_format(const char *path) {
    if (!path) return -1;
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);

    if (type == 1) { // /sd0/
#if defined(CONFIG_BOARD_RP2350)
        block_dev_t *dev = spisd_get_device();
#else
        block_dev_t *dev = virtio_blk_get_device();
#endif
        if (!dev || fat32_format(dev) != 0) return -1;
        g_sd_mounted = (fat32_init(&g_fat32_sd, dev) == 0);
        return g_sd_mounted ? 0 : -1;
    } else if (type == 2) { // /ram0/
        block_dev_t *dev = ramdisk_get_device();
        if (!dev || fat32_format(dev) != 0) return -1;
        g_ram_mounted = (fat32_init(&g_fat32_ram, dev) == 0);
        return g_ram_mounted ? 0 : -1;
    } else if (type == 6) { // /flash0/
        printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) cannot be formatted (read-only)\n");
        return -1;
    }
    printk("[VFS Error] format: '%s' is not a formattable storage volume\n", path ? path : "");
    return -1;
}

int vfs_remove(const char *path) {
    if (!path) return -1;
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (type == 6 && rel && g_flash_mounted) {
        return fat32_remove_file(&g_fat32_flash, rel);
    } else if (type == 1 && rel && g_sd_mounted) {
        return fat32_remove_file(&g_fat32_sd, rel);
    } else if (type == 2 && rel && g_ram_mounted) {
        return fat32_remove_file(&g_fat32_ram, rel);
    }
    return -1;
}

int vfs_mkdir(const char *path) {
    if (!path) return -1;
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (type == 6 && rel && g_flash_mounted) {
        return fat32_mkdir(&g_fat32_flash, rel);
    } else if (type == 1 && rel && g_sd_mounted) {
        return fat32_mkdir(&g_fat32_sd, rel);
    } else if (type == 2 && rel && g_ram_mounted) {
        return fat32_mkdir(&g_fat32_ram, rel);
    }
    return -1;
}

int vfs_rmdir(const char *path) {
    if (!path) return -1;
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (type == 6 && rel && g_flash_mounted) {
        return fat32_rmdir(&g_fat32_flash, rel);
    } else if (type == 1 && rel && g_sd_mounted) {
        return fat32_rmdir(&g_fat32_sd, rel);
    } else if (type == 2 && rel && g_ram_mounted) {
        return fat32_rmdir(&g_fat32_ram, rel);
    }
    return -1;
}

/* Copies via the new handle API in fixed-size chunks rather than one
 * vfs_read()/vfs_write() shot into a static 4KB buffer -- the old approach
 * silently truncated any source file over 4095 bytes with no error. */
int vfs_cp(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return -1;

    int src_fd = vfs_open(src_path, VFS_O_READ);
    if (src_fd < 0) {
        printk("cp: cannot read source path '%s'\n", src_path);
        return -1;
    }
    int dst_fd = vfs_open(dst_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (dst_fd < 0) {
        vfs_close(src_fd);
        printk("cp: failed to write to destination path '%s'\n", dst_path);
        return -1;
    }

    static uint8_t copy_buf[512];
    uint64_t offset = 0;
    int result = 0;
    for (;;) {
        int n = vfs_pread(src_fd, copy_buf, sizeof(copy_buf), offset);
        if (n < 0) { result = -1; break; }
        if (n == 0) break;
        int w = vfs_pwrite(dst_fd, copy_buf, (uint32_t)n, offset);
        if (w != n) { result = -1; break; }
        offset += (uint64_t)n;
    }

    vfs_close(src_fd);
    vfs_close(dst_fd);
    if (result < 0) {
        printk("cp: copy failed ('%s' -> '%s')\n", src_path, dst_path);
    }
    return result;
}

void vfs_ls(const char *path) {
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);

    if (type == 0) { // Root "/"
        printk("\nDirectory Listing (/):\n");
        printk("Name        Type                        Status\n");
        printk("----------  --------------------------  ---------\n");
        printk("flash0      FAT32 Embedded Flash ROM    %s\n", g_flash_mounted ? "active" : "unmounted");
        printk("sd0         FAT32 VirtIO Persistent SD  %s\n", g_sd_mounted ? "mounted" : "unmounted");
        printk("ram0        FAT32 In-Memory RAMDisk     %s\n", g_ram_mounted ? "mounted" : "unmounted");
        printk("proc        Synthetic Metrics System    active\n");
        printk("dev         Hardware Device Nodes       active\n");
        printk("srv         IPC Service Registry        active\n\n");
    } else if (type == 6) { // /flash0/
        if (g_flash_mounted) {
            fat32_list_dir(&g_fat32_flash, rel);
        } else {
            printk("ls: /flash0/ is not mounted\n");
        }
    } else if (type == 1) { // /sd0/
        if (g_sd_mounted) {
            fat32_list_dir(&g_fat32_sd, rel);
        } else {
            printk("ls: /sd0/ is not mounted\n");
        }
    } else if (type == 2) { // /ram0/
        if (g_ram_mounted) {
            fat32_list_dir(&g_fat32_ram, rel);
        } else {
            printk("ls: /ram0/ is not mounted\n");
        }
    } else if (type == 3) { // /proc/ -- listed via the real handle/readdir API now (V5 fix)
        printk("\nDirectory Listing (/proc/):\n");
        printk("Name        Type\n----------  ----\n");
        int fd = vfs_open("/proc", VFS_O_READ);
        if (fd >= 0) {
            char name[32];
            vfs_stat_t st;
            for (uint32_t i = 0; vfs_readdir(fd, i, name, sizeof(name), &st) == 0; i++) {
                printk("%s      %s\n", name, st.is_dir ? "<DIR>" : "<FILE>");
            }
            vfs_close(fd);
        }
        printk("\n");
    } else if (type == 4) { // /dev/
        printk("\nDirectory Listing (/dev/):\n");
        printk("Name        Type\n----------  ----\nuart        char device\nnull        bit bucket\nzero        null generator\neeprom      i2c eeprom (4KB)\n\n");
    } else if (type == 5) { // /srv/
        printk("\nDirectory Listing (/srv/):\n");
        printk("Service Name  Target PID\n------------  ----------\n");
        for (int i = 0; i < g_num_services; i++) {
            printk("%s            %d\n", g_services[i].name, g_services[i].target_pid);
        }
        printk("\n");
    }
}
