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

static void populate_initial_files(void) {
    fat32_dir_entry_t entry;
    if (fat32_find_file(&g_fat32, "lugal.h", &entry) < 0) {
        static const char *lugal_h_src =
            "#ifndef _LUGAL_H\n"
            "#define _LUGAL_H\n"
            "#define SYS_IPC_CALL   1\n"
            "#define SYS_IPC_REPLY  2\n"
            "#define SYS_IPC_SEND   3\n"
            "#define SYS_IPC_RECV   4\n"
            "#define SYS_PRINT      10\n"
            "#define SYS_PUTNUM     11\n"
            "#define SYS_PUTCHAR    12\n"
            "#define SYS_READ_FILE  13\n"
            "#define SYS_WRITE_FILE 14\n"
            "#define IPC_ANY       -1\n"
            "struct ipc_msg {\n"
            "    long tag;\n"
            "    long d0;\n"
            "    long d1;\n"
            "    long d2;\n"
            "    long d3;\n"
            "    long d4;\n"
            "};\n"
            "long lugal_syscall(long sys_nr, long a1, long a2, long a3);\n"
            "int print(char *s);\n"
            "int puts(char *s);\n"
            "int printf(char *s);\n"
            "int putnum(long n);\n"
            "int putchar(char c);\n"
            "int read_file(char *path, void *buf, int max_len);\n"
            "int write_file(char *path, void *buf, int len);\n"
            "#endif\n";
        fat32_write_file(&g_fat32, "lugal.h", lugal_h_src, strlen(lugal_h_src));
    }

    if (fat32_find_file(&g_fat32, "hello.c", &entry) < 0) {
        static const char *hello_c_src =
            "#include <lugal.h>\n"
            "main() {\n"
            "    printf(\"Hello from LugalOS FAT32 Storage!\\n\");\n"
            "}\n";
        fat32_write_file(&g_fat32, "hello.c", hello_c_src, strlen(hello_c_src));
    }

    if (fat32_find_file(&g_fat32, "prime.c", &entry) < 0) {
        static const char *prime_c_src =
            "#include <lugal.h>\n"
            "main() {\n"
            "    print(\"Prime numbers up to 30:\\n\");\n"
            "    int i = 2;\n"
            "    while (i <= 30) {\n"
            "        int is_p = 1;\n"
            "        int j = 2;\n"
            "        while (j * j <= i) {\n"
            "            if (i % j == 0) { is_p = 0; break; }\n"
            "            j++;\n"
            "        }\n"
            "        if (is_p) {\n"
            "            putnum(i);\n"
            "            putchar(' ');\n"
            "        }\n"
            "        i++;\n"
            "    }\n"
            "    putchar('\\n');\n"
            "}\n";
        fat32_write_file(&g_fat32, "prime.c", prime_c_src, strlen(prime_c_src));
    }

    if (fat32_find_file(&g_fat32, "fib.c", &entry) < 0) {
        static const char *fib_c_src =
            "#include <lugal.h>\n"
            "fib(n) {\n"
            "    if (n <= 1) return n;\n"
            "    return fib(n - 1) + fib(n - 2);\n"
            "}\n"
            "main() {\n"
            "    print(\"Fibonacci sequence:\\n\");\n"
            "    int i = 0;\n"
            "    while (i <= 10) {\n"
            "        putnum(fib(i));\n"
            "        putchar(' ');\n"
            "        i++;\n"
            "    }\n"
            "    putchar('\\n');\n"
            "}\n";
        fat32_write_file(&g_fat32, "fib.c", fib_c_src, strlen(fib_c_src));
    }

    if (fat32_find_file(&g_fat32, "cat.c", &entry) < 0) {
        static const char *cat_c_src =
            "#include <lugal.h>\n"
            "main() {\n"
            "    char buf[256];\n"
            "    int bytes = read_file(\"/proc/ps\", buf, 255);\n"
            "    if (bytes > 0) {\n"
            "        buf[bytes] = 0;\n"
            "        print(buf);\n"
            "    }\n"
            "}\n";
        fat32_write_file(&g_fat32, "cat.c", cat_c_src, strlen(cat_c_src));
    }
}

void vfs_server_init(void) {
    block_dev_t *ramdisk = ramdisk_get_device();
    if (ramdisk) {
        fat32_init(&g_fat32, ramdisk);
        populate_initial_files();
    }

    g_num_services = 0;
    vfs_register_service("lisp", 2);

    printk("[VFS Server] Universal Namespace Resolver (Plan 9 Model) initialized (PID %d).\n", VFS_PID);
    printk("[VFS Server] Mounted: /ram0/ & /sd0/ (FAT32), /proc/ (Metrics), /dev/ (Devices), /srv/ (IPC)\n");
}

int vfs_register_service(const char *service_name, int target_pid) {
    if (!service_name || g_num_services >= MAX_SERVICES) return -1;
    strncpy(g_services[g_num_services].name, service_name, 31);
    g_services[g_num_services].name[31] = '\0';
    g_services[g_num_services].target_pid = target_pid;
    g_num_services++;
    return 0;
}

/* Parse prefix: returns prefix type (1: ram0, 2: proc, 3: dev, 4: srv) */
static int parse_prefix(const char *path, const char **rel_path) {
    static const char *empty_str = "";
    if (!rel_path) rel_path = &empty_str;

    if (!path) {
        *rel_path = empty_str;
        return 1;
    }

    if (strncmp(path, "/ram0/", 6) == 0) {
        *rel_path = path + 6;
        return 1;
    } else if (strncmp(path, "/sd0/", 5) == 0) {
        *rel_path = path + 5;
        return 1;
    } else if (strcmp(path, "/ram0") == 0 || strcmp(path, "/sd0") == 0) {
        *rel_path = "";
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
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 1) { // FAT32 /ram0/
        fat32_dir_entry_t entry;
        if (fat32_find_file(&g_fat32, rel, &entry) < 0) {
            return -1;
        }
        return fat32_read_file(&g_fat32, &entry, buf, max_len);
    } else if (type == 2) { // /proc/ synthetic metrics
        char *sbuf = (char *)buf;
        if (strcmp(rel, "ps") == 0) {
            int len = printk("PID  State    Name\n---  -------  ------------\n 0   RUNNING  kernel_idle\n 1   READY    lsh_console\n 2   READY    lisp_engine\n 3   READY    vfs_server (FAT32)\n");
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return len;
        } else if (strcmp(rel, "meminfo") == 0) {
            printk("Heap & Storage Status:\n  Page Size: 4096 bytes\n  VMM Status: Active\n  Storage: /ram0/ FAT32 Volume (512 KB)\n");
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return 0;
        } else if (strcmp(rel, "version") == 0) {
            printk("LugalOS v0.3.0 (Plan 9 Universal Namespace Core)\n");
            if (sbuf && max_len > 0) sbuf[0] = '\0';
            return 0;
        }
        return -1;
    } else if (type == 3) { // /dev/ hardware devices
        if (strcmp(rel, "uart") == 0) {
            if (buf && max_len > 0) {
                char *sbuf = (char *)buf;
                sbuf[0] = uart_getc();
                sbuf[1] = '\0';
                return 1;
            }
            return 0;
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
    if (!path) return -1;

    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (!rel) rel = "";

    if (type == 1) { // FAT32 /ram0/
        return fat32_write_file(&g_fat32, rel, buf, len);
    } else if (type == 3) { // /dev/ hardware devices
        if (strcmp(rel, "uart") == 0) {
            if (buf) {
                const char *str = (const char *)buf;
                for (uint32_t i = 0; i < len; i++) {
                    uart_putc(str[i]);
                }
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
    if (!path) return -1;
    const char *rel = NULL;
    int type = parse_prefix(path, &rel);
    if (type == 1 && rel) {
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
