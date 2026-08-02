#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/block.h"
#include "drivers/uart.h"
#include "kernel/printk.h"
#include "kernel/ipc.h"
#include "kernel/sched.h"
#include <string.h>

static fat32_fs_t g_fat32;

typedef struct {
    char name[32];
    int target_pid;
} service_entry_t;

#define MAX_SERVICES 8
static service_entry_t g_services[MAX_SERVICES];
static int g_num_services = 0;

void vfs_server_init(void) {
    block_dev_t *ramdisk = ramdisk_get_device();
    fat32_init(&g_fat32, ramdisk);

    g_num_services = 0;
    vfs_register_service("lisp", 2);

    printk("[VFS Server] Universal Namespace Resolver (Plan 9 Model) initialized (PID %d).\n", VFS_PID);
    printk("[VFS Server] Mounted: /ram0/ (FAT32), /proc/ (Metrics), /dev/ (Devices), /srv/ (IPC)\n");
}

int vfs_register_service(const char *service_name, int target_pid) {
    if (g_num_services >= MAX_SERVICES) return -1;
    strncpy(g_services[g_num_services].name, service_name, 31);
    g_services[g_num_services].name[31] = '\0';
    g_services[g_num_services].target_pid = target_pid;
    g_num_services++;
    return 0;
}

/* Parse prefix: returns prefix type (1: ram0, 2: proc, 3: dev, 4: srv) */
static int parse_prefix(const char *path, const char **rel_path) {
    if (!path) return 1;

    if (strncmp(path, "/ram0/", 6) == 0) {
        *rel_path = path + 6;
        return 1;
    } else if (strncmp(path, "/proc/", 6) == 0) {
        *rel_path = path + 6;
        return 2;
    } else if (strncmp(path, "/dev/", 5) == 0) {
        *rel_path = path + 5;
        return 3;
    } else if (strncmp(path, "/srv/", 5) == 0) {
        *rel_path = path + 5;
        return 4;
    }

    if (path[0] == '/') {
        *rel_path = path + 1;
    } else {
        *rel_path = path;
    }
    return 1; // Default to /ram0/
}

int vfs_read(const char *path, void *buf, uint32_t max_len) {
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);

    if (type == 1) { // FAT32 /ram0/
        fat32_dir_entry_t entry;
        if (fat32_find_file(&g_fat32, rel, &entry) < 0) {
            return -1;
        }
        return fat32_read_file(&g_fat32, &entry, buf, max_len);
    } else if (type == 2) { // /proc/ synthetic metrics
        char *sbuf = (char *)buf;
        if (strcmp(rel, "ps") == 0) {
            int len = 0;
            len += printk("PID  State    Name\n---  -------  ------------\n 0   RUNNING  kernel_idle\n 1   READY    lsh_console\n 2   READY    lisp_engine\n 3   READY    vfs_server (FAT32)\n");
            sbuf[0] = '\0';
            return len;
        } else if (strcmp(rel, "meminfo") == 0) {
            printk("Heap & Storage Status:\n  Page Size: 4096 bytes\n  VMM Status: Active\n  Storage: /ram0/ FAT32 Volume (512 KB)\n");
            sbuf[0] = '\0';
            return 0;
        } else if (strcmp(rel, "version") == 0) {
            printk("LugalOS v0.3.0 (Plan 9 Universal Namespace Core)\n");
            sbuf[0] = '\0';
            return 0;
        }
        return -1;
    } else if (type == 3) { // /dev/ hardware devices
        if (strcmp(rel, "uart") == 0) {
            char *sbuf = (char *)buf;
            sbuf[0] = uart_getc();
            sbuf[1] = '\0';
            return 1;
        } else if (strcmp(rel, "null") == 0 || strcmp(rel, "zero") == 0) {
            return 0;
        }
    } else if (type == 4) { // /srv/ IPC channels
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
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);

    if (type == 1) { // FAT32 /ram0/
        return fat32_write_file(&g_fat32, rel, buf, len);
    } else if (type == 3) { // /dev/ hardware devices
        if (strcmp(rel, "uart") == 0) {
            const char *str = (const char *)buf;
            for (uint32_t i = 0; i < len; i++) {
                uart_putc(str[i]);
            }
            return 0;
        } else if (strcmp(rel, "null") == 0) {
            return 0; // Bit bucket
        }
    } else if (type == 4) { // /srv/ IPC channels
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

int vfs_remove(const char *path) {
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (type == 1) {
        return fat32_remove_file(&g_fat32, rel);
    }
    return -1;
}

void vfs_ls(const char *path) {
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);

    if (type == 2) { // /proc/
        printk("\nDirectory Listing (/proc/):\n");
        printk("Name        Type\n----------  ----\nps          synthetic\nmeminfo     synthetic\nversion     synthetic\n\n");
    } else if (type == 3) { // /dev/
        printk("\nDirectory Listing (/dev/):\n");
        printk("Name        Type\n----------  ----\nuart        char device\nnull        bit bucket\nzero        null generator\n\n");
    } else if (type == 4) { // /srv/
        printk("\nDirectory Listing (/srv/):\n");
        printk("Service Name  Target PID\n------------  ----------\n");
        for (int i = 0; i < g_num_services; i++) {
            printk("%s            %d\n", g_services[i].name, g_services[i].target_pid);
        }
        printk("\n");
    } else { // /ram0/ default
        fat32_list_dir(&g_fat32);
    }
}
