#include "fs/vfs.h"
#include "fs/fat32.h"
#include "fs/9p.h"
#include "fs/p9_link.h"
#include "fs/p9_chan.h"
#include "drivers/block.h"
#include "drivers/flashdisk.h"
#include "drivers/uart.h"
#include "drivers/at24c32.h"
#include "drivers/loopback_net.h"
#include "drivers/uart_net.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/klog.h"
#include "kernel/device.h"
#include "kernel/chan.h"
#include "kernel/palloc.h"
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

/* --- Mount table (A5, plan/phase5_distributed_design.md) ---
 * Replaces the old parse_prefix()'s hardcoded if-chain, which defaulted
 * *any* unrecognized path to /flash0/ -- silent, surprising fall-through
 * that broke Plan 9's "namespaces are explicit" semantics (review finding
 * A2, plan/completed/2026-08-07_review_and_remediation.md). vfs_resolve()
 * below returns NULL for a path with no matching mount, full stop; nothing
 * falls back to anything else.
 *
 * Flat, single-component matching only (a mount name is exactly the first
 * path segment, e.g. "sd0", "proc", or a user-chosen remote mount name) --
 * this system doesn't need Plan 9's general bind/union-directory mount
 * table, and a flat table is enough to "attach a remote namespace into the
 * local one" (A5's stated goal) via vfs_mount_remote().
 *
 * FAT32 mounts still carry a `mounted_ptr` indirection into the existing
 * g_flash_mounted/g_sd_mounted/g_ram_mounted globals rather than owning
 * their mounted-state directly: those globals are still the single
 * source of truth vfs_mount_ramdisk()/vfs_format() flip, unchanged from
 * before this refactor -- minimizes risk to already-tested mount/format
 * logic while still replacing the *routing* mechanism around it. */
typedef enum {
    MOUNT_FAT32,
    MOUNT_PROC,
    MOUNT_DEV,
    MOUNT_SRV,
    MOUNT_REMOTE9P,
} mount_kind_t;

typedef struct {
    bool in_use;
    char name[16];
    mount_kind_t kind;
    char label[40];              /* human-readable description for `ls /` */
    bool read_only;
    fat32_fs_t *fs;               /* MOUNT_FAT32 */
    bool *mounted_ptr;            /* MOUNT_FAT32; NULL = always considered mounted */
    p9_remote_mount_t *remote;    /* MOUNT_REMOTE9P */
} mount_entry_t;

#define MAX_MOUNTS 10
static mount_entry_t g_mounts[MAX_MOUNTS];

/* --- Handle table (A1, plan/phase5_distributed_design.md) ---
 * A flat array, not a union: at 8 handles this is a few KB (dominated by
 * the 512-byte proc_buf per slot), affordable even on RP2350's SRAM
 * budget. `kind` mirrors the owning mount's mount_kind_t, plus a
 * root-pseudo-directory case (kind == (mount_kind_t)-1, see vfs_open())
 * for "/" itself, which isn't a real mount_entry_t. MOUNT_SRV never
 * appears here -- /srv/ stays on its own direct-dispatch path in
 * vfs_read()/vfs_write() (see fs/include/fs/vfs.h), it's message-oriented
 * IPC, not a byte-addressable file. */
#define VFS_KIND_ROOT ((mount_kind_t)-1)

typedef struct {
    bool in_use;
    mount_kind_t kind;
    int flags;
    bool is_dir;
    fat32_fs_t *fs;              /* valid for MOUNT_FAT32 */
    fat32_dir_entry_t entry;     /* valid for MOUNT_FAT32, non-dir */
    uint32_t dir_cluster;        /* valid for MOUNT_FAT32, is_dir */
    char rel_path[128];          /* path within volume (FAT32) or device name (DEV) */
    char proc_buf[512];          /* generated /proc file content (PROC, non-dir) */
    uint32_t proc_len;
    /* /proc/kmsg only: served straight from the klog ring rather than
     * generated into proc_buf, which is far too small to hold it. The window
     * [kmsg_start, kmsg_start + proc_len) is snapshotted at open() time, so
     * the log output produced *by reading the log* (cat's own printk() calls
     * land in the same ring) can't feed the read and make it never finish. */
    bool is_kmsg;
    uint64_t kmsg_start;
    p9_remote_mount_t *remote_mount; /* valid for MOUNT_REMOTE9P */
    uint32_t remote_fid;              /* valid for MOUNT_REMOTE9P */
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

static mount_entry_t *mount_alloc(const char *name) {
    if (!name || !name[0] || strlen(name) >= sizeof(g_mounts[0].name)) return NULL;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].name, name) == 0) return NULL; // name collision
    }
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) {
            memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            strncpy(g_mounts[i].name, name, sizeof(g_mounts[i].name) - 1);
            g_mounts[i].in_use = true;
            return &g_mounts[i];
        }
    }
    return NULL; // table full
}

static bool mount_is_active(const mount_entry_t *m) {
    if (!m->in_use) return false;
    if (m->kind == MOUNT_FAT32) return m->mounted_ptr ? *m->mounted_ptr : true;
    return true; // proc/dev/srv/remote9p are always "active" once registered
}

static void mount_table_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));

    mount_entry_t *e;
    e = mount_alloc("flash0");
    e->kind = MOUNT_FAT32; e->fs = &g_fat32_flash; e->mounted_ptr = &g_flash_mounted; e->read_only = true;
    strncpy(e->label, "FAT32 Embedded Flash ROM", sizeof(e->label) - 1);

    e = mount_alloc("sd0");
    e->kind = MOUNT_FAT32; e->fs = &g_fat32_sd; e->mounted_ptr = &g_sd_mounted;
    strncpy(e->label, "FAT32 Persistent SD", sizeof(e->label) - 1);

    e = mount_alloc("ram0");
    e->kind = MOUNT_FAT32; e->fs = &g_fat32_ram; e->mounted_ptr = &g_ram_mounted;
    strncpy(e->label, "FAT32 In-Memory RAMDisk", sizeof(e->label) - 1);

    e = mount_alloc("proc");
    e->kind = MOUNT_PROC;
    strncpy(e->label, "Synthetic Metrics System", sizeof(e->label) - 1);

    e = mount_alloc("dev");
    e->kind = MOUNT_DEV;
    strncpy(e->label, "Hardware Device Nodes", sizeof(e->label) - 1);

    e = mount_alloc("srv");
    e->kind = MOUNT_SRV;
    strncpy(e->label, "IPC Service Registry", sizeof(e->label) - 1);
}

/* Attaches a remote 9P namespace at /<name>/ (A5). `link` must already be
 * usable (e.g. virtio_console_get_link()); this does the Tversion +
 * Tattach handshake once via p9_remote_mount_open() and keeps the
 * connection alive for the mount's lifetime. Fails if `name` collides with
 * an existing mount, the table is full, or the handshake itself fails. */
int vfs_mount_remote(const char *name, p9_link_t *link) {
    if (!link) return -1;
    mount_entry_t *e = mount_alloc(name);
    if (!e) return -1;

    p9_remote_mount_t *remote = p9_remote_mount_open(link);
    if (!remote) {
        memset(e, 0, sizeof(*e));
        return -1;
    }

    e->kind = MOUNT_REMOTE9P;
    e->remote = remote;
    ksnprintf(e->label, sizeof(e->label), "Remote 9P Namespace (%s)", link->name ? link->name : "?");
    printk("[VFS Server] Mounted remote 9P namespace on /%s/ (link '%s')\n", name, link->name ? link->name : "?");
    return 0;
}

/* Attaches this node's OWN 9P server at /<name>/, over a local channel
 * (B1). Note what this function does not do: there is no MOUNT_LOCAL9P kind,
 * no local-specific handle code, no second dispatch path. A local mount is
 * vfs_mount_remote() handed a channel-backed link instead of a wire-backed
 * one, and everything downstream -- p9_remote_open/pread/pwrite/readdir,
 * vfs_open()'s MOUNT_REMOTE9P branch -- is reused unchanged.
 *
 * That reuse *is* the deliverable. Because the copy-always discipline an
 * address-space boundary would impose is already being obeyed with no
 * boundary present, "local" and "remote" stopped being different problems.
 * It is also why walking into /<name>/<name>/... is possible: bounded by
 * chan_call()'s re-entrancy check, which fails the inner call rather than
 * corrupting the outer one. */
int vfs_mount_local(const char *name) {
    p9_link_t *link = p9_chan_get_link();
    if (!link) return -1;
    return vfs_mount_remote(name, link);
}

/* Detaches a remote mount added via vfs_mount_remote(). Only ever removes
 * MOUNT_REMOTE9P entries -- the built-in mounts (flash0/sd0/ram0/proc/dev/
 * srv) aren't unmountable through this path, since too much of the rest of
 * the system assumes they exist. */
int vfs_unmount(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && g_mounts[i].kind == MOUNT_REMOTE9P && strcmp(g_mounts[i].name, name) == 0) {
            p9_remote_mount_close(g_mounts[i].remote);
            memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            printk("[VFS Server] Unmounted /%s/\n", name);
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

    mount_table_init();

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

/* Resolves `path` against the mount table. `*is_root` is set when `path`
 * is "/" (or empty); in that case the return value is always NULL and
 * callers handle the root pseudo-directory themselves. Otherwise, NULL
 * means no mount matches the path's first component -- explicit failure,
 * not a fallback to any particular volume (closes review finding A2). */
static mount_entry_t *vfs_resolve(const char *path, const char **rel_path, bool *is_root) {
    static const char *empty_str = "";
    if (!rel_path) rel_path = &empty_str;
    if (is_root) *is_root = false;

    if (!path || path[0] == '\0') path = "/";
    while (*path == '/') path++;

    if (*path == '\0') {
        *rel_path = empty_str;
        if (is_root) *is_root = true;
        return NULL;
    }

    const char *slash = strchr(path, '/');
    size_t complen = slash ? (size_t)(slash - path) : strlen(path);
    if (complen >= sizeof(g_mounts[0].name)) return NULL; // no mount name is ever this long

    char comp[sizeof(g_mounts[0].name)];
    memcpy(comp, path, complen);
    comp[complen] = '\0';

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].name, comp) == 0) {
            *rel_path = slash ? slash + 1 : empty_str;
            return &g_mounts[i];
        }
    }
    return NULL; // no such mount
}

/* Generates the current content of a /proc/<name> virtual file directly into
 * a caller-owned buffer using ksnprintf() instead of printk() -- so /proc
 * files are real, readable byte streams a caller (eventually a remote 9P
 * client) can vfs_pread() in pieces, not a printk() side effect (see V5 in
 * plan/completed/2026-08-07_review_and_remediation.md). Returns the number of
 * bytes generated, or -1 if `rel` doesn't name a known /proc file. */
/* Appends `s` padded with spaces to `width`, for fixed-width /proc columns.
 * Needed because this kernel's printk()/ksnprintf() format engine accepts
 * only '0', width, '.prec' and 'l' -- there is no '-' (left-justify) flag,
 * so "%-11s" would be emitted literally rather than padding. Truncates
 * rather than overflowing if `s` is longer than `width`. */
static uint32_t append_col(char *buf, uint32_t used, uint32_t cap,
                           const char *s, uint32_t width) {
    uint32_t n = 0;
    while (s && s[n] && n < width && used < cap - 1) buf[used++] = s[n++];
    while (n < width && used < cap - 1) { buf[used++] = ' '; n++; }
    if (used < cap) buf[used] = '\0';
    return used;
}

static int vfs_generate_proc_content(const char *rel, char *buf, uint32_t cap) {
    if (!rel || !buf || cap == 0) return -1;
    uint32_t used = 0;

    if (strcmp(rel, "ps") == 0) {
        /* B2: the real task table. This used to be a hardcoded string
         * listing four tasks that did not exist -- kernel/sched.c was
         * bookkeeping only and nothing was ever scheduled. */
        used += (uint32_t)ksnprintf(buf + used, cap - used, "PID  State    Name\n");
        used += (uint32_t)ksnprintf(buf + used, cap - used, "---  -------  ------------\n");
        int pid, state;
        const char *tname;
        for (uint32_t i = 0; sched_task_info(i, &pid, &state, &tname); i++) {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "%3d  ", pid);
            used = append_col(buf, used, cap, sched_state_name(state), 9);
            used += (uint32_t)ksnprintf(buf + used, cap - used, "%s\n", tname);
        }
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
        /* B2: real allocator numbers instead of a fixed string. The bump
         * allocator this replaced had nothing meaningful to report -- it
         * couldn't free and had no upper bound to be a fraction of. */
        uint32_t total_pg = 0, free_pg = 0;
        palloc_stats(&total_pg, &free_pg);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "Heap & Storage Status:\n  Page Size: %u bytes\n", (unsigned int)PAGE_SIZE);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Pages Total: %u\n  Pages Free: %u\n  Pages Used: %u\n",
            total_pg, free_pg, total_pg - free_pg);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Heap Free: %u KB of %u KB\n",
            (free_pg * (uint32_t)PAGE_SIZE) / 1024, (total_pg * (uint32_t)PAGE_SIZE) / 1024);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Storage: /flash0/ (Flash ROM), /sd0/ (VirtIO SD), /ram0/ (RAMDisk)\n");
        return (int)used;
    } else if (strcmp(rel, "buildid") == 0) {
        /* Deliberately its own file rather than extra lines on /proc/version:
         * tests/hw/ needs to ask "is this board running the firmware I just
         * built?", and a small dedicated file answers that without changing
         * the size of a file several tests already read. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "%s %s\n", LUGALOS_VERSION, LUGALOS_BUILD_ID);
        return (int)used;
    } else if (strcmp(rel, "devices") == 0) {
        /* B0 device registry (kernel/device.h). Compact deliberately: this
         * is generated into the handle's fixed 512-byte proc_buf. */
        used = append_col(buf, used, cap, "Name", 11);
        used = append_col(buf, used, cap, "Kind", 9);
        used += (uint32_t)ksnprintf(buf + used, cap - used, "State\n");
        const char *dname, *dkind;
        bool dpresent;
        for (uint32_t i = 0; dev_info(i, &dname, &dkind, &dpresent); i++) {
            used = append_col(buf, used, cap, dname, 11);
            used = append_col(buf, used, cap, dkind, 9);
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                                        "%s\n", dpresent ? "present" : "absent");
        }
        return (int)used;
    } else if (strcmp(rel, "version") == 0) {
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "LugalOS v%s (Bare-Metal RISC-V Lisp Machine)\n", LUGALOS_VERSION);
        return (int)used;
    }
    return -1;
}

static const char *g_proc_names[7] = { "ps", "meminfo", "version", "df", "kmsg", "devices", "buildid" };
static const char *g_dev_names[4]  = { "uart", "null", "zero", "eeprom" };

/* Opens `path` into a fresh handle, returning a small non-negative fd (index
 * into g_handles[]) on success or -1 on failure. /srv/ (message-oriented
 * IPC channels) is deliberately not handle-addressable -- see the comment
 * on this API in fs/include/fs/vfs.h. */
/* Fills an already-reserved handle. Split out of vfs_open() so that the
 * slot can be marked in_use *before* any of this runs -- see vfs_open(). */
static int vfs_open_into(vfs_handle_t *h, const char *path, int flags) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);

    if (is_root) {
        h->kind = VFS_KIND_ROOT;
        h->is_dir = true;
    } else if (!m) {
        return -1; // no such mount -- explicit failure, no fallback (A2)
    } else if (m->kind == MOUNT_FAT32) {
        if (!mount_is_active(m)) return -1;
        if (m->read_only && (flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC))) return -1;

        fat32_dir_entry_t entry;
        if (fat32_find_file(m->fs, rel, &entry) < 0) {
            if (!(flags & VFS_O_CREATE)) return -1;
            if (fat32_write_file(m->fs, rel, NULL, 0) != 0) return -1;
            if (fat32_find_file(m->fs, rel, &entry) < 0) return -1;
        } else if ((flags & VFS_O_TRUNC) && !(entry.attr & FAT32_ATTR_DIRECTORY)) {
            if (fat32_truncate(m->fs, rel) != 0) return -1;
            if (fat32_find_file(m->fs, rel, &entry) < 0) return -1;
        }

        h->fs = m->fs;
        h->entry = entry;
        h->is_dir = (entry.attr & FAT32_ATTR_DIRECTORY) != 0;
        if (h->is_dir) {
            h->dir_cluster = ((uint32_t)entry.fst_clus_hi << 16) | entry.fst_clus_lo;
        }
        strncpy(h->rel_path, rel, sizeof(h->rel_path) - 1);
        h->rel_path[sizeof(h->rel_path) - 1] = '\0';
        h->kind = MOUNT_FAT32;
    } else if (m->kind == MOUNT_PROC) {
        if (rel[0] == '\0') {
            h->is_dir = true;
        } else if (strcmp(rel, "kmsg") == 0) {
            h->is_kmsg = true;
            h->kmsg_start = klog_oldest();
            h->proc_len = (uint32_t)(klog_total() - h->kmsg_start);
        } else {
            int len = vfs_generate_proc_content(rel, h->proc_buf, sizeof(h->proc_buf));
            if (len < 0) return -1;
            h->proc_len = (uint32_t)len;
        }
        h->kind = MOUNT_PROC;
    } else if (m->kind == MOUNT_DEV) {
        if (rel[0] == '\0') {
            h->is_dir = true;
        } else {
            strncpy(h->rel_path, rel, sizeof(h->rel_path) - 1);
            h->rel_path[sizeof(h->rel_path) - 1] = '\0';
        }
        h->kind = MOUNT_DEV;
    } else if (m->kind == MOUNT_REMOTE9P) {
        uint8_t p9_mode = P9_OREAD;
        if ((flags & VFS_O_WRITE) && (flags & VFS_O_READ)) p9_mode = P9_ORDWR;
        else if (flags & VFS_O_WRITE) p9_mode = P9_OWRITE;
        if (flags & VFS_O_TRUNC) p9_mode |= P9_OTRUNC;
        bool create = (flags & VFS_O_CREATE) != 0;

        uint32_t fid;
        bool remote_is_dir;
        if (p9_remote_open(m->remote, rel, p9_mode, create, &fid, &remote_is_dir) < 0) return -1;

        h->remote_mount = m->remote;
        h->remote_fid = fid;
        h->is_dir = remote_is_dir;
        h->kind = MOUNT_REMOTE9P;
    } else {
        return -1; // MOUNT_SRV: not handle-addressable
    }

    return 0;
}

int vfs_open(const char *path, int flags) {
    if (!path) return -1;

    int slot = -1;
    for (int i = 0; i < VFS_MAX_HANDLES; i++) {
        if (!g_handles[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    vfs_handle_t *h = &g_handles[slot];
    memset(h, 0, sizeof(*h));

    /* Reserve the slot BEFORE doing any work that might re-enter vfs_open().
     *
     * This used to be set only on the success path at the very end, which was
     * fine while every backend was a straight-line local call. It stopped
     * being fine the moment a mount could be served by this same node over a
     * channel (B1's vfs_mount_local()): opening /<local>/proc/version makes
     * the 9P server re-enter vfs_open() for /proc/version *while this call is
     * still in flight*, the inner call finds this slot still marked free,
     * takes it, and the outer call then overwrites the inner handle with its
     * own remote state. The server's fid then pointed at a MOUNT_REMOTE9P
     * handle, so its next vfs_pread() bounced straight back into the channel
     * and was refused as re-entrant -- a genuinely confusing failure two
     * layers away from the cause.
     *
     * A truly remote peer could never expose this (its vfs_open() runs on
     * another machine), which is exactly why exercising the local case is
     * worth doing: it puts the client and server in one address space, where
     * shared-state bugs like this one become reachable. */
    h->in_use = true;

    if (vfs_open_into(h, path, flags) < 0) {
        memset(h, 0, sizeof(*h)); /* releases the reservation */
        return -1;
    }

    h->flags = flags;
    return slot;
}

int vfs_pread(int fd, void *buf, uint32_t count, uint64_t offset) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use) return -1;
    vfs_handle_t *h = &g_handles[fd];
    if (h->is_dir) return -1;
    if (count == 0) return 0;
    if (!buf) return -1;

    switch (h->kind) {
        case MOUNT_FAT32:
            return fat32_read_at(h->fs, &h->entry, buf, count, offset);
        case MOUNT_PROC: {
            if (offset >= h->proc_len) return 0;
            uint32_t avail = h->proc_len - (uint32_t)offset;
            uint32_t n = (avail < count) ? avail : count;
            if (h->is_kmsg) {
                return (int)klog_read(h->kmsg_start + offset, (char *)buf, n);
            }
            memcpy(buf, h->proc_buf + offset, n);
            return (int)n;
        }
        case MOUNT_DEV:
            if (strcmp(h->rel_path, "uart") == 0) {
                ((char *)buf)[0] = uart_getc();
                return 1;
            } else if (strcmp(h->rel_path, "eeprom") == 0) {
                return at24c32_read((uint16_t)offset, (uint8_t *)buf, count);
            } else if (strcmp(h->rel_path, "null") == 0 || strcmp(h->rel_path, "zero") == 0) {
                return 0;
            }
            return -1;
        case MOUNT_REMOTE9P:
            return p9_remote_pread(h->remote_mount, h->remote_fid, buf, count, offset);
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

    switch (h->kind) {
        case MOUNT_FAT32: {
            int n = fat32_write_at(h->fs, h->rel_path, buf, count, offset);
            if (n > 0) fat32_find_file(h->fs, h->rel_path, &h->entry); // refresh cached size
            return n;
        }
        case MOUNT_DEV:
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
        case MOUNT_REMOTE9P:
            return p9_remote_pwrite(h->remote_mount, h->remote_fid, buf, count, offset);
        default:
            return -1; // /flash0/ (read-only) and anything else not writable
    }
}

int vfs_readdir(int fd, uint32_t index, char *name_out, uint32_t name_max, vfs_stat_t *stat_out) {
    if (fd < 0 || fd >= VFS_MAX_HANDLES || !g_handles[fd].in_use) return -1;
    vfs_handle_t *h = &g_handles[fd];
    if (!h->is_dir) return -1;

    switch (h->kind) {
        case VFS_KIND_ROOT: {
            uint32_t seen = 0;
            for (int i = 0; i < MAX_MOUNTS; i++) {
                if (!mount_is_active(&g_mounts[i])) continue;
                if (seen == index) {
                    if (name_out && name_max > 0) {
                        strncpy(name_out, g_mounts[i].name, name_max - 1);
                        name_out[name_max - 1] = '\0';
                    }
                    if (stat_out) { stat_out->size = 0; stat_out->is_dir = 1; }
                    return 0;
                }
                seen++;
            }
            return -1;
        }
        case MOUNT_FAT32: {
            fat32_dir_entry_t entry;
            if (fat32_readdir(h->fs, h->dir_cluster, index, name_out, name_max, &entry) != 0) return -1;
            if (stat_out) {
                stat_out->size = entry.file_size;
                stat_out->is_dir = (entry.attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
            }
            return 0;
        }
        case MOUNT_PROC: {
            /* Derived from the table rather than hardcoded: adding /proc/kmsg
             * (B0) meant updating a literal 4 in a second place, which is
             * exactly the drift this sizeof() prevents next time. */
            if (index >= sizeof(g_proc_names) / sizeof(g_proc_names[0])) return -1;
            if (name_out && name_max > 0) {
                strncpy(name_out, g_proc_names[index], name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
            return 0;
        }
        case MOUNT_DEV: {
            if (index >= 4) return -1;
            if (name_out && name_max > 0) {
                strncpy(name_out, g_dev_names[index], name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
            return 0;
        }
        case MOUNT_REMOTE9P: {
            p9_remote_stat_t rst;
            if (p9_remote_readdir(h->remote_mount, h->remote_fid, index, name_out, name_max, &rst) != 0) return -1;
            if (stat_out) {
                stat_out->size = (uint32_t)rst.size;
                stat_out->is_dir = rst.is_dir ? 1 : 0;
            }
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
    switch (h->kind) {
        case MOUNT_FAT32:
            out->size = h->entry.file_size;
            break;
        case MOUNT_PROC:
            out->size = h->proc_len;
            break;
        case MOUNT_REMOTE9P: {
            p9_remote_stat_t rst;
            out->size = (p9_remote_fstat(h->remote_mount, h->remote_fid, &rst) == 0) ? (uint32_t)rst.size : 0;
            break;
        }
        default:
            out->size = 0;
            break;
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
    vfs_handle_t *h = &g_handles[fd];
    if (h->kind == MOUNT_REMOTE9P) {
        p9_remote_close(h->remote_mount, h->remote_fid);
    }
    memset(h, 0, sizeof(*h));
    return 0;
}

/* --- Legacy whole-file API: thin compat wrappers over the handle API above,
 * except the /srv/ branch of vfs_read()/vfs_write(), which bypasses it
 * entirely (message-oriented IPC channels aren't handle-addressable).
 * Note this means vfs_read()/vfs_write()/vfs_cp()/vfs_append() all pick up
 * MOUNT_REMOTE9P support "for free" -- they were already generic over
 * whatever vfs_open()/vfs_pread()/vfs_pwrite() resolve to. --- */

int vfs_read(const char *path, void *buf, uint32_t max_len) {
    if (!path) return -1;

    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);

    if (m && m->kind == MOUNT_SRV) {
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

    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);

    if (m && m->kind == MOUNT_FAT32 && m->read_only) {
        printk("[VFS Error] '/%s/' is read-only\n", m->name);
        return -1;
    }

    if (m && m->kind == MOUNT_SRV) {
        if (strcmp(rel, "p9_loopback") == 0 || strcmp(rel, "loopback_9p") == 0 || strcmp(rel, "p9") == 0 || strcmp(rel, "net") == 0) {
            return loopback_9p_rpc((const char *)buf, NULL, 0);
        } else if (strcmp(rel, "uart_9p") == 0 || strcmp(rel, "net0") == 0) {
            return uart_net_rpc((const char *)buf, NULL, 0);
        }
        /* B1 Rule 1: a /srv/ write is a *message*, delivered by copy through
         * a channel. This used to build an ipc_msg_t carrying
         * `.data = { (uintptr_t)buf, len, ... }` -- the caller's raw pointer
         * handed to another "task" -- which is exactly the shortcut that
         * cannot survive an address-space boundary and would have forced the
         * MMU and NOMMU builds apart. chan_call() copies in and out; nothing
         * a handler sees belongs to the caller. */
        chan_endpoint_t *ep = chan_lookup(rel);
        if (ep) {
            uint8_t reply[64];
            int n = chan_call(ep, (const uint8_t *)buf, len, reply, sizeof(reply));
            return (n < 0) ? -1 : 0;
        }
        for (int i = 0; i < g_num_services; i++) {
            if (strcmp(rel, g_services[i].name) == 0) {
                /* A declared service with no channel behind it yet. Reports
                 * failure rather than the old unconditional 0: claiming a
                 * write succeeded when nothing consumed it is worse than an
                 * honest error, and nothing in the tree depends on the lie. */
                printk("[VFS Router] '/srv/%s' has no channel endpoint bound\n",
                       g_services[i].name);
                return -1;
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

    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);
    if (!m) return -1;
    if (m->kind == MOUNT_FAT32 && m->read_only) {
        printk("[VFS Error] '/%s/' is read-only\n", m->name);
        return -1;
    }
    if (m->kind != MOUNT_FAT32 && m->kind != MOUNT_REMOTE9P) return -1;

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
 * formatted now. Remote mounts can't be formatted (nonsensical -- it isn't
 * this node's storage to reinitialize). */
int vfs_format(const char *path) {
    if (!path) return -1;
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);

    if (m && strcmp(m->name, "sd0") == 0) {
#if defined(CONFIG_BOARD_RP2350)
        block_dev_t *dev = spisd_get_device();
#else
        block_dev_t *dev = virtio_blk_get_device();
#endif
        if (!dev || fat32_format(dev) != 0) return -1;
        g_sd_mounted = (fat32_init(&g_fat32_sd, dev) == 0);
        return g_sd_mounted ? 0 : -1;
    } else if (m && strcmp(m->name, "ram0") == 0) {
        block_dev_t *dev = ramdisk_get_device();
        if (!dev || fat32_format(dev) != 0) return -1;
        g_ram_mounted = (fat32_init(&g_fat32_ram, dev) == 0);
        return g_ram_mounted ? 0 : -1;
    } else if (m && strcmp(m->name, "flash0") == 0) {
        printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) cannot be formatted (read-only)\n");
        return -1;
    }
    printk("[VFS Error] format: '%s' is not a formattable storage volume\n", path ? path : "");
    return -1;
}

int vfs_remove(const char *path) {
    if (!path) return -1;
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);
    if (!m || !rel || rel[0] == '\0') return -1;

    if (m->kind == MOUNT_FAT32) {
        if (m->read_only || !mount_is_active(m)) return -1;
        return fat32_remove_file(m->fs, rel);
    } else if (m->kind == MOUNT_REMOTE9P) {
        return p9_remote_remove(m->remote, rel);
    }
    return -1;
}

int vfs_mkdir(const char *path) {
    if (!path) return -1;
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);
    if (!m || !rel || rel[0] == '\0') return -1;

    if (m->kind == MOUNT_FAT32) {
        if (m->read_only || !mount_is_active(m)) return -1;
        return fat32_mkdir(m->fs, rel);
    } else if (m->kind == MOUNT_REMOTE9P) {
        return p9_remote_mkdir(m->remote, rel);
    }
    return -1;
}

int vfs_rmdir(const char *path) {
    if (!path) return -1;
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);
    if (!m || !rel || rel[0] == '\0') return -1;

    if (m->kind == MOUNT_FAT32) {
        if (m->read_only || !mount_is_active(m)) return -1;
        return fat32_rmdir(m->fs, rel);
    } else if (m->kind == MOUNT_REMOTE9P) {
        return p9_remote_remove(m->remote, rel); // server dispatches file vs. dir removal itself
    }
    return -1;
}

/* Copies via the new handle API in fixed-size chunks rather than one
 * vfs_read()/vfs_write() shot into a static 4KB buffer -- the old approach
 * silently truncated any source file over 4095 bytes with no error. Works
 * across mounts of any kind (including remote ones) since it's built
 * entirely on vfs_open()/vfs_pread()/vfs_pwrite(). */
int vfs_cp(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return -1;

    int src_fd = vfs_open(src_path, VFS_O_READ);
    if (src_fd < 0) {
        cprintf("cp: cannot read source path '%s'\n", src_path);
        return -1;
    }
    int dst_fd = vfs_open(dst_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (dst_fd < 0) {
        vfs_close(src_fd);
        cprintf("cp: failed to write to destination path '%s'\n", dst_path);
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
        cprintf("cp: copy failed ('%s' -> '%s')\n", src_path, dst_path);
    }
    return result;
}

void vfs_ls(const char *path) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = vfs_resolve(path, &rel, &is_root);

    if (is_root) {
        cprintf("\nDirectory Listing (/):\n");
        cprintf("Name        Type                        Status\n");
        cprintf("----------  --------------------------  ---------\n");
        for (int i = 0; i < MAX_MOUNTS; i++) {
            if (!g_mounts[i].in_use) continue;
            cprintf("%s      %s  %s\n", g_mounts[i].name, g_mounts[i].label,
                   mount_is_active(&g_mounts[i]) ? "active" : "unmounted");
        }
        cprintf("\n");
        return;
    }

    if (!m) {
        cprintf("ls: '%s': no such mount\n", path ? path : "");
        return;
    }

    switch (m->kind) {
        case MOUNT_FAT32:
            if (mount_is_active(m)) {
                fat32_list_dir(m->fs, rel);
            } else {
                cprintf("ls: /%s/ is not mounted\n", m->name);
            }
            break;
        case MOUNT_PROC: { // listed via the real handle/readdir API (V5 fix)
            cprintf("\nDirectory Listing (/proc/):\n");
            cprintf("Name        Type\n----------  ----\n");
            int fd = vfs_open("/proc", VFS_O_READ);
            if (fd >= 0) {
                char name[32];
                vfs_stat_t st;
                for (uint32_t i = 0; vfs_readdir(fd, i, name, sizeof(name), &st) == 0; i++) {
                    cprintf("%s      %s\n", name, st.is_dir ? "<DIR>" : "<FILE>");
                }
                vfs_close(fd);
            }
            cprintf("\n");
            break;
        }
        case MOUNT_DEV:
            cprintf("\nDirectory Listing (/dev/):\n");
            cprintf("Name        Type\n----------  ----\nuart        char device\nnull        bit bucket\nzero        null generator\neeprom      i2c eeprom (4KB)\n\n");
            break;
        case MOUNT_SRV:
            cprintf("\nDirectory Listing (/srv/):\n");
            cprintf("Service Name  Target PID\n------------  ----------\n");
            for (int i = 0; i < g_num_services; i++) {
                cprintf("%s            %d\n", g_services[i].name, g_services[i].target_pid);
            }
            cprintf("\n");
            break;
        case MOUNT_REMOTE9P: {
            cprintf("\nDirectory Listing (/%s/%s):\n", m->name, rel);
            cprintf("Name        Type      Size\n----------  --------  ----------\n");
            int fd = vfs_open(path, VFS_O_READ);
            if (fd >= 0) {
                char name[32];
                vfs_stat_t st;
                for (uint32_t i = 0; vfs_readdir(fd, i, name, sizeof(name), &st) == 0; i++) {
                    cprintf("%s      %s  %u\n", name, st.is_dir ? "<DIR>" : "<FILE>", st.size);
                }
                vfs_close(fd);
            } else {
                cprintf("ls: cannot open '%s'\n", path);
            }
            break;
        }
    }
}
