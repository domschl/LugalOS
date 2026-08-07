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

int vfs_read(const char *path, void *buf, uint32_t max_len) {
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    fat32_dir_entry_t entry;

    if (type == 6) { // /flash0/
        if (g_flash_mounted && fat32_find_file(&g_fat32_flash, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_flash, &entry, buf, max_len);
        }
        if (g_sd_mounted && fat32_find_file(&g_fat32_sd, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_sd, &entry, buf, max_len);
        }
        if (g_ram_mounted && fat32_find_file(&g_fat32_ram, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_ram, &entry, buf, max_len);
        }
        return -1;
    } else if (type == 1) { // /sd0/
        if (g_sd_mounted && fat32_find_file(&g_fat32_sd, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_sd, &entry, buf, max_len);
        }
        if (g_flash_mounted && fat32_find_file(&g_fat32_flash, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_flash, &entry, buf, max_len);
        }
        if (g_ram_mounted && fat32_find_file(&g_fat32_ram, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_ram, &entry, buf, max_len);
        }
        return -1;
    } else if (type == 2) { // /ram0/
        if (g_ram_mounted && fat32_find_file(&g_fat32_ram, rel, &entry) >= 0) {
            return fat32_read_file(&g_fat32_ram, &entry, buf, max_len);
        }
        return -1;
    } else if (type == 3) { // /proc/ synthetic metrics
        char *sbuf = (char *)buf;
        if (strcmp(rel, "ps") == 0) {
            int len = printk("PID  State    Name\n---  -------  ------------\n 0   RUNNING  kernel_idle\n 1   READY    lsh_console\n 2   READY    lisp_engine\n 3   READY    vfs_server (FAT32)\n");
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return len;
        } else if (strcmp(rel, "df") == 0) {
            printk("Filesystem     512-blocks       Used  Available Capacity Mounted on\n");
            if (g_flash_mounted) {
                uint32_t total = 0, free = 0;
                fat32_statfs(&g_fat32_flash, &total, &free);
                uint32_t total_b = total / 512;
                uint32_t free_b = free / 512;
                uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
                uint32_t pct = total_b ? (used_b * 100 / total_b) : 100;
                printk("/flash0/         %9u  %9u  %9u     %3u%% /flash0/\n", total_b, used_b, free_b, pct);
            }
            if (g_ram_mounted) {
                uint32_t total = 0, free = 0;
                fat32_statfs(&g_fat32_ram, &total, &free);
                uint32_t total_b = total / 512;
                uint32_t free_b = free / 512;
                uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
                uint32_t pct = total_b ? (used_b * 100 / total_b) : 0;
                printk("/ram0/           %9u  %9u  %9u     %3u%% /ram0/\n", total_b, used_b, free_b, pct);
            }
            if (g_sd_mounted) {
                uint32_t total = 0, free = 0;
                fat32_statfs(&g_fat32_sd, &total, &free);
                uint32_t total_b = total / 512;
                uint32_t free_b = free / 512;
                uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
                uint32_t pct = total_b ? (used_b * 100 / total_b) : 0;
                printk("/sd0/            %9u  %9u  %9u     %3u%% /sd0/\n", total_b, used_b, free_b, pct);
            }
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return 0;
        } else if (strcmp(rel, "meminfo") == 0) {
            printk("Heap & Storage Status:\n  Page Size: 4096 bytes\n  VMM Status: Active\n  Storage: /flash0/ (Flash ROM), /sd0/ (VirtIO SD), /ram0/ (RAMDisk)\n");
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return 0;
        } else if (strcmp(rel, "version") == 0) {
            printk("LugalOS v0.5.0 (Bare-Metal RISC-V Lisp Machine)\n");
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return 0;
        }
        return -1;
    } else if (type == 4) { // /dev/ hardware devices
        if (strcmp(rel, "uart") == 0) {
            if (buf && max_len > 0) {
                char *sbuf = (char *)buf;
                sbuf[0] = uart_getc();
                sbuf[1] = '\0';
                return 1;
            }
            return 0;
        } else if (strcmp(rel, "eeprom") == 0) {
            if (buf && max_len > 0) {
                return at24c32_read(0, (uint8_t *)buf, max_len);
            }
            return 0;
        } else if (strcmp(rel, "null") == 0 || strcmp(rel, "zero") == 0) {
            return 0;
        }
    } else if (type == 5) { // /srv/ IPC channels
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
    }
    return -1;
}

int vfs_write(const char *path, const void *buf, uint32_t len) {
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 6) { // /flash0/
        if (g_flash_mounted) {
            printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) is read-only\n");
            return -1;
        }
        return -1;
    } else if (type == 1) { // /sd0/
        if (g_sd_mounted) {
            return fat32_write_file(&g_fat32_sd, rel, buf, len);
        }
        return -1;
    } else if (type == 2) { // /ram0/
        if (g_ram_mounted) {
            return fat32_write_file(&g_fat32_ram, rel, buf, len);
        }
        return -1;
    } else if (type == 4) { // /dev/ hardware devices
        if (strcmp(rel, "uart") == 0) {
            if (buf) {
                const char *str = (const char *)buf;
                for (uint32_t i = 0; i < len; i++) {
                    uart_putc(str[i]);
                }
            }
            return 0;
        } else if (strcmp(rel, "eeprom") == 0) {
            if (buf && len > 0) {
                return at24c32_write(0, (const uint8_t *)buf, len);
            }
            return 0;
        } else if (strcmp(rel, "null") == 0) {
            return 0;
        }
    } else if (type == 5) { // /srv/ IPC channels
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
    }
    return -1;
}

int vfs_append(const char *path, const void *buf, uint32_t len) {
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 1) { // /sd0/
        if (g_sd_mounted) return fat32_append_file(&g_fat32_sd, rel, buf, len);
        return -1;
    } else if (type == 2) { // /ram0/
        if (g_ram_mounted) return fat32_append_file(&g_fat32_ram, rel, buf, len);
        return -1;
    } else if (type == 6) { // /flash0/
        printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) is read-only\n");
        return -1;
    }
    return -1;
}

/* Explicit volume initialization, replacing the auto-format-on-invalid-boot-
 * sector behavior fat32_init() used to have (see B10 in
 * plan/2026-08-07_review_and_remediation.md) -- a corrupt or blank card
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

int vfs_cp(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return -1;

    static uint8_t copy_buf[4096];
    int bytes_read = vfs_read(src_path, copy_buf, sizeof(copy_buf) - 1);
    if (bytes_read < 0) {
        printk("cp: cannot read source path '%s'\n", src_path);
        return -1;
    }

    int write_res = vfs_write(dst_path, copy_buf, (uint32_t)bytes_read);
    if (write_res < 0) {
        printk("cp: failed to write to destination path '%s'\n", dst_path);
        return -1;
    }

    return 0;
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
    } else if (type == 3) { // /proc/
        printk("\nDirectory Listing (/proc/):\n");
        printk("Name        Type\n----------  ----\nps          synthetic\nmeminfo     synthetic\nversion     synthetic\ndf          synthetic\n\n");
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
