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
#include "net/ip.h"
#include "net/tcp.h"
#include "kernel/identity.h"
#include "kernel/chan.h"
#include "kernel/palloc.h"
#include "kernel/meminfo.h"
#include "arch/elf.h"
#include "kernel/path.h"
#include "kernel/ipc.h"
#include "kernel/sched.h"
#include "kernel/mem_domain.h"
#include "kernel/version.h"
#include <string.h>
#include <stdbool.h>

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
/* Last, deliberately: the CONFIG_* guard arrives with the headers above. */
#include "drivers/dcf77_service.h"
#endif

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
    /* Not a mount kind: the kind a *handle* carries when it is the synthetic
     * root directory, which belongs to no volume. It lives in this enum
     * rather than being cast in from outside it (it used to be
     * `(mount_kind_t)-1`) so that a switch over a handle's kind is a switch
     * over a complete set of named values, and -Wswitch can say so. */
    MOUNT_ROOT_HANDLE,
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
#define VFS_KIND_ROOT MOUNT_ROOT_HANDLE

typedef struct {
    bool in_use;
    mount_kind_t kind;
    int flags;
    bool is_dir;
    fat32_fs_t *fs;              /* valid for MOUNT_FAT32 */
    fat32_dir_entry_t entry;     /* valid for MOUNT_FAT32, non-dir */
    uint32_t dir_cluster;        /* valid for MOUNT_FAT32, is_dir */
    char rel_path[128];          /* path within volume (FAT32) or device name (DEV) */
    /* Generated /proc file content (PROC, non-dir), shared by every file
     * under /proc that this VFS serves. 512 silently truncated /proc/config on RP2350
     * once every board feature's pin fields were in it -- caught by
     * tests/hw/test_rp2350.py's K3, not by anything that runs on QEMU,
     * since QEMU has no pin model to report. Measured, not assumed: the
     * worst case (every ENABLE_* flag on at once, including the clock
     * persona's fields alongside chess's) is 791 bytes including the NUL:
     *
     *     PALLOC_MAX_PAGES=128\nUART0_BASE=0x40070000\n...DCF77_WARMUP_MS=5000\n
     *
     * That was 690 before D2 (plan/phase17_clock_ui_and_dcf77.md) added
     * ENABLE_DCF77 plus four DCF77_* pin lines -- 95 bytes, which is what
     * took it past the old 768 and would have silently truncated the tail of
     * /proc/config all over again. 896 gives headroom without padding
     * arbitrarily. */
    char proc_buf[896];
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
    vfs_register_service("console", VFS_PID); /* B4: the console server endpoint */
    vfs_register_service("lisp", 2);
    loopback_net_init();
    vfs_register_service("p9_loopback", 9);
    uart_net_init();
    vfs_register_service("uart_9p", 10);

    printk("[VFS Server] Universal Namespace Resolver (Plan 9 Model) initialized (PID %d).\n", VFS_PID);
}

bool vfs_volume_writable(const char *name) {
    if (!name) return false;
    while (*name == '/') name++;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry_t *m = &g_mounts[i];
        if (!m->in_use || !m->name[0]) continue;

        /* Compare up to the volume name's end, tolerating a trailing slash so
         * "/sd0/" and "sd0" both work -- a boot script should not have to
         * know which spelling this function wants. */
        const char *a = name;
        const char *b = m->name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b != '\0') continue;
        if (*a != '\0' && *a != '/') continue;

        return mount_is_active(m) && !m->read_only;
    }
    return false;
}

int vfs_mount_ramdisk(int size_kb) {
    block_dev_t *ram_dev = ramdisk_get_device();
    if (ram_dev) {
        /* The storage is allocated here rather than reserved in .bss (C5), so
         * a mount can fail for lack of memory -- which is a better outcome
         * than an eighth of the machine being spent on a disk nobody asked
         * for. */
        if (size_kb <= 0) size_kb = 64;
        uint32_t requested_blocks = ((uint32_t)size_kb * 1024) / ram_dev->block_size;
        uint32_t max_blocks = ramdisk_max_blocks();
        /* Below this a volume is smaller than its own FAT32 metadata: it
         * formats, it mounts, and then every write fails. Clamping up and
         * saying so beats handing back a disk that reports itself healthy. */
        if (requested_blocks < FAT32_MIN_SECTORS) {
            uint32_t min_kb = FAT32_MIN_SECTORS * ram_dev->block_size / 1024;
            printk("[VFS Server] Requested /ram0/ size %d KB is below the %u KB "
                   "FAT32 minimum; using %u KB.\n", size_kb,
                   (unsigned int)min_kb, (unsigned int)min_kb);
            requested_blocks = FAT32_MIN_SECTORS;
            size_kb = (int)min_kb;
        }
        if (requested_blocks > max_blocks) {
            uint32_t max_kb = (max_blocks * ram_dev->block_size) / 1024;
            printk("[VFS Server] Warning: requested /ram0/ size %d KB exceeds the "
                   "RAMDisk cap (%u KB); clamping.\n", size_kb, (unsigned int)max_kb);
            requested_blocks = max_blocks;
            size_kb = (int)max_kb;
        }
        if (ramdisk_init(requested_blocks) != 0) {
            printk("[VFS Server] /ram0/ not mounted: no memory for a %d KB RAM disk\n",
                   size_kb);
            return -1;
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
         * bookkeeping only and nothing was ever scheduled.
         *
         * M6: "Isol" makes the vision statement at the top of
         * plan/phase12_microkernel_migration.md a thing you can point at
         * rather than describe -- whether each task actually runs under
         * hardware-enforced isolation (task_set_domain() was called on
         * it) rather than just "is alive". Every M5 driver task shows the
         * real backend name once it enters U-mode (PMP on the RP2350/
         * RV32_NOMMU builds, Sv39 on RV64_MMU); the kernel task itself
         * and anything not yet converted (M4.5's own p9srv) correctly
         * show "-". Same shape on both backends -- the one thing that
         * differs is which name appears, not whether the column exists. */
        /* "Isol" trails Exit rather than sitting between Name and Exit --
         * tests/hw and tests/runner.py both have existing patterns
         * (`uprog\s+42`, `uprog\s+killed`) asserting Name is immediately
         * followed by the exit outcome; keeping that adjacency means this
         * column addition costs nothing elsewhere instead of needing every
         * such pattern hunted down and loosened. Found the hard way (a
         * QEMU rv64 run that looked like a hang was actually 3 full
         * retries failing the same two now-broken regexes). */
        used += (uint32_t)ksnprintf(buf + used, cap - used, "PID  State    Name          Exit   Isol  Stack\n");
        used += (uint32_t)ksnprintf(buf + used, cap - used, "---  -------  ------------  ----   ----  ---------\n");
        int pid, state;
        const char *tname;
        long status;
        bool clean, has_domain;
        for (uint32_t i = 0; sched_task_info_ex(i, &pid, &state, &tname, &status, &clean, &has_domain); i++) {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "%3d  ", pid);
            used = append_col(buf, used, cap, sched_state_name(state), 9);
            used = append_col(buf, used, cap, tname, 14);
            /* Only a dead task has an outcome. "killed" rather than a number
             * for one the fault handler ended: a status of 0 would otherwise
             * read as a clean success, which is precisely backwards. */
            char exitbuf[8];
            if (state != TASK_DEAD) {
                ksnprintf(exitbuf, sizeof(exitbuf), "-");
            } else if (clean) {
                ksnprintf(exitbuf, sizeof(exitbuf), "%ld", status);
            } else {
                ksnprintf(exitbuf, sizeof(exitbuf), "killed");
            }
            /* Width 7, not 6: "killed" (6 chars) would otherwise exactly
             * fill a 6-wide column and glue onto Isol with no separating
             * space at all -- found live on real hardware (`usbisotest`
             * prints "killed" for the intruder task it deliberately kills). */
            used = append_col(buf, used, cap, exitbuf, 7);
            used = append_col(buf, used, cap,
                              has_domain ? mem_domain_backend_name() : "-", 6);
            /* Stack high-water against the size it was given (§6,
             * plan/phase15_memory_reclamation.md). "-" for the boot task,
             * whose stack is the linker's and is reported by the Boot Stack
             * line in /proc/meminfo instead. */
            {
                uint32_t su = sched_stack_used(pid);
                uint32_t ss = sched_stack_size(pid);
                if (ss == 0) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used, "-\n");
                } else if (sched_stack_full(pid)) {
                    /* Not a formatting nicety: an overflowing stack scribbles
                     * on whatever is below it, and the symptom appears
                     * anywhere but here. Say the word. */
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                                                "%u/%u B  ** STACK OVERFLOW **\n", su, ss);
                } else {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                                                "%u/%u B\n", su, ss);
                }
            }
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
         * couldn't free and had no upper bound to be a fraction of.
         *
         * Extended since with the figures that cannot be read off the ELF:
         * the heap's high-water mark, its worst-case contiguous run, and how
         * deep the boot stack has ever been. Static section sizes are exact
         * in the image and are not re-derived here; what the RAM map below
         * adds is the *leftover* -- the relationship between those sizes and
         * the board's actual RAM, which is the thing that decides whether
         * another static array fits.
         *
         * The whole report has to land inside the handle's 512-byte proc_buf
         * (see vfs_handle_t). ksnprintf() truncates safely rather than
         * overrunning, so the failure mode is a silently short file; the
         * Storage line is deliberately kept last so that a test asserting on
         * it is also asserting that nothing above it was cut off. */
        uint32_t total_pg = 0, free_pg = 0, peak_pg = 0, run_pg = 0;
        palloc_stats(&total_pg, &free_pg);
        palloc_extra_stats(&peak_pg, &run_pg);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "Heap & Storage Status:\n  Page Size: %u bytes\n", (unsigned int)PAGE_SIZE);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Pages Total: %u\n  Pages Free: %u\n  Pages Used: %u\n",
            total_pg, free_pg, total_pg - free_pg);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Pages Peak: %u\n  Largest Free Run: %u pages\n", peak_pg, run_pg);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Heap Free: %u KB of %u KB\n",
            (free_pg * (uint32_t)PAGE_SIZE) / 1024, (total_pg * (uint32_t)PAGE_SIZE) / 1024);

        mem_ram_map_t map;
        meminfo_ram_map(&map);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "RAM: %u KB total at 0x%lx\n",
            map.total_bytes / 1024, (unsigned long)map.ram_start);
        /* On RP2350 .text and .rodata are in flash and appear in the Flash
         * line below; on the QEMU targets they are in this same region and
         * are part of this figure. Labelled per target rather than averaged
         * into one vague word, because the two numbers are not comparable. */
#if defined(CONFIG_BOARD_RP2350)
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Image (data+bss): %u KB\n", map.image_bytes / 1024);
#else
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Image (text+data+bss): %u KB\n", map.image_bytes / 1024);
#endif
        /* The static side in the parts it is made of (§6,
         * plan/phase15_memory_reclamation.md). The one Image figure above
         * shows that it moved; these show what moved -- and on this board
         * every byte of them is a heap byte nothing else can have. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "    .data %u B, .bss %u KB", map.data_bytes, map.bss_bytes / 1024);
        if (map.ustacks_bytes) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                ", ustacks %u KB", map.ustacks_bytes / 1024);
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used, "\n");
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Boot Stack: %u KB, peak %u bytes\n",
            map.stack_bytes / 1024, stack_used_bytes());
        /* Two different sizes on purpose. The heap *region* is whatever the
         * linker left above _kernel_end; the *managed* part is what palloc
         * actually put in its bitmap, which PALLOC_MAX_PAGES can cap well
         * below the region (it does on QEMU: 16 MB of 128 MB). Reporting only
         * one of them would make the cap invisible. */
        uint32_t img_alloc = 0, img_used = 0;
        elf_image_stats(&img_alloc, &img_used);
        if (img_alloc) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "  User Images: %u pages, %u spanned (%u padding)\n",
                img_alloc, img_used, img_alloc - img_used);
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Heap: %u KB managed of %u KB\n",
            (total_pg * (uint32_t)PAGE_SIZE) / 1024, map.heap_bytes / 1024);

        uint32_t flash_used = 0, flash_total = 0;
        if (meminfo_flash(&flash_used, &flash_total)) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "Flash: %u KB of %u KB\n", flash_used / 1024, flash_total / 1024);
        }

        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "  Storage: /flash0/ (Flash ROM), /sd0/ (VirtIO SD), /ram0/ (RAMDisk)\n");
        return (int)used;
    } else if (strcmp(rel, "node") == 0) {
        /* Who this node is, and where each half of that came from.
         *
         * Its own file rather than a corner of /proc/net, because identity is
         * not a networking fact -- the name is a hostname, a 9P uname and a
         * log prefix before it is anything on a wire. Both `source` lines are
         * the point: "why do two boards answer to the same thing" is a
         * question with three possible answers, and this says which. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "name: %s\nname source: %s\n", node_name(), node_name_source());
        char mac[18];
        netif_mac_str(node_mac(), mac);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "mac: %s\nmac source: %s\n", mac, node_mac_source());
        /* I2 (plan/phase21_identity_and_authentication.md): the device-scope
         * UID, absent unless silicon or a provisioned record supplied one --
         * "unset" would claim more than is known, so this says "none". */
        uint8_t uid[NODE_UID_LEN];
        if (node_uid(uid)) {
            /* Hand-rolled, not "%02x": this custom printf's %x reads a
             * va_arg(unsigned long), and a uint8_t argument only gets
             * promoted to int -- correct on rv32 where int and long are
             * both 32 bits, wrong on rv64 where long is 64. netif_mac_str()
             * above hits the same trap and hand-rolls for the same reason. */
            static const char hex[] = "0123456789abcdef";
            char uidhex[NODE_UID_LEN * 2 + 1];
            for (unsigned i = 0; i < NODE_UID_LEN; i++) {
                uidhex[i * 2]     = hex[uid[i] >> 4];
                uidhex[i * 2 + 1] = hex[uid[i] & 0x0f];
            }
            uidhex[NODE_UID_LEN * 2] = '\0';
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "uid: %s\nuid source: %s\n", uidhex, node_uid_source());
        } else {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "uid: none\nuid source: %s\n", node_uid_source());
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "persona: %s\nbuild seed: %s\n", CONFIG_NODE_PERSONA, CONFIG_NODE_SEED);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "9P uname: %s\n", node_name());
        return (int)used;
    } else if (strcmp(rel, "net") == 0) {
        /* R2, plan/phase19_ip_stack_and_ethernet.md: the interface, its
         * address, its ARP cache and every counter the stack keeps.
         *
         * **A file under /proc rather than a `/net` mount**, which is where
         * the plan's §3 put it. The reasoning changed once it was written: a
         * whole new mount kind, with its own readdir and a two-level
         * ipifc/0/ tree, is real machinery to build for one status file --
         * and the eventual `/net` is a *writable socket* filesystem, so it
         * would be redesigned when sockets land anyway. /proc/ports,
         * /proc/devices and /proc/config are this project's existing idiom
         * for exactly this, and `cat /proc/net` debugs the stack today.
         * Promoting it is a later phase's job, with a user to justify it.
         *
         * Every drop is a separate line for the reason phase 18 learned
         * expensively: "the network does not work" is not a diagnosis, and a
         * single total cannot become one. */
        const net_state_t *ns = net_state();
        if (!ns->nif) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "interface: none\nThis board has no network interface.\n");
        } else {
            char mac[18];
            netif_mac_str(ns->nif->mac, mac);
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "interface: %s\nmac: %s\nlink: %s\n",
                ns->nif->name, mac, netif_link_up(ns->nif) ? "up" : "down");
            if (ns->configured) {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "address: %u.%u.%u.%u\nnetmask: %u.%u.%u.%u\ngateway: %u.%u.%u.%u\n",
                    ns->ip[0], ns->ip[1], ns->ip[2], ns->ip[3],
                    ns->mask[0], ns->mask[1], ns->mask[2], ns->mask[3],
                    ns->gw[0], ns->gw[1], ns->gw[2], ns->gw[3]);
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "address: unconfigured\n");
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "rx: %lu frames, %lu bytes\ntx: %lu frames, %lu bytes\n",
                (unsigned long)ns->nif->rx_frames, (unsigned long)ns->nif->rx_bytes,
                (unsigned long)ns->nif->tx_frames, (unsigned long)ns->nif->tx_bytes);
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "arp: %lu rx, %lu tx\nip: %lu rx, %lu tx\n"
                "icmp: %lu rx, %lu tx\nudp: %lu rx, %lu tx\ntcp: %lu rx\n",
                (unsigned long)ns->rx_arp, (unsigned long)ns->tx_arp,
                (unsigned long)ns->rx_ip, (unsigned long)ns->tx_ip,
                (unsigned long)ns->rx_icmp, (unsigned long)ns->tx_icmp,
                (unsigned long)ns->rx_udp, (unsigned long)ns->tx_udp,
                (unsigned long)ns->rx_tcp);
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "drop: %lu not-for-us, %lu short, %lu checksum, %lu fragment,\n"
                "      %lu proto, %lu no-route, %lu no-port\n",
                (unsigned long)ns->drop_not_for_us, (unsigned long)ns->drop_short,
                (unsigned long)ns->drop_checksum, (unsigned long)ns->drop_fragment,
                (unsigned long)ns->drop_proto, (unsigned long)ns->drop_no_route,
                (unsigned long)ns->drop_no_port);
            uint16_t lport = 0;
            if (tcp_listening(&lport)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "tcp: listening on %u, %lu open, %lu accepted, %lu reset\n",
                    lport, (unsigned long)tcp_conn_count(),
                    (unsigned long)tcp_accepted_total(),
                    (unsigned long)tcp_reset_total());
                for (uint32_t i = 0; i < 2; i++) {
                    char line[80];
                    tcp_conn_str(i, line, sizeof(line));
                    if (line[0]) used += (uint32_t)ksnprintf(buf + used, cap - used, "  %s", line);
                }
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "tcp: not listening\n");
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "udp bindings: %lu\narp cache: %lu entries\n",
                (unsigned long)udp_bindings(), (unsigned long)arp_entries());
            for (uint32_t i = 0; i < 8; i++) {
                char line[48];
                arp_entry_str(i, line, sizeof(line));
                if (line[0]) used += (uint32_t)ksnprintf(buf + used, cap - used, "  %s", line);
            }
        }
        return (int)used;
    } else if (strcmp(rel, "ports") == 0) {
        /* Which channels this board has, what each can be, and who holds it
         * (C8). Several names can be one wire -- the UART is a console, a
         * dedicated 9P link and a demultiplexed one -- so the wire column is
         * what makes a conflict legible. */
        used = append_col(buf, used, cap, "Port", 11);
        used = append_col(buf, used, cap, "Role", 9);
        used = append_col(buf, used, cap, "Wire", 8);
        used += (uint32_t)ksnprintf(buf + used, cap - used, "State\n");
        const char *pname, *pkind, *pwire;
        bool ppresent, pbound;
        for (uint32_t i = 0;
             dev_binding_info(i, &pname, &pkind, &pwire, &ppresent, &pbound); i++) {
            used = append_col(buf, used, cap, pname, 11);
            used = append_col(buf, used, cap, pkind, 9);
            used = append_col(buf, used, cap, pwire, 8);
            used += (uint32_t)ksnprintf(buf + used, cap - used, "%s\n",
                                        !ppresent ? "absent" : (pbound ? "bound" : "free"));
        }
        return (int)used;
    } else if (strcmp(rel, "path") == 0) {
        /* The search path as a file, so a remote node can read this board's
         * command-resolution policy over 9P like anything else in /proc. */
        used += (uint32_t)path_format(buf + used, cap - used);
        used += (uint32_t)ksnprintf(buf + used, cap - used, "\n");
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
    } else if (strcmp(rel, "config") == 0) {
        /* Board config (K3, plan/phase7_kernel_config.md): the generated
         * platform-default and pin-map values actually baked into this
         * image, so a wrong value in cmake/board-*.cmake is visible from a
         * running board -- including over 9P from a remote node -- rather
         * than only from reading the generator's output by hand. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "PALLOC_MAX_PAGES=%d\n", CONFIG_PALLOC_MAX_PAGES);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "UART0_BASE=0x%lx\n", (unsigned long)CONFIG_UART0_BASE);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "ENABLE_CC=%d\nENABLE_ED=%d\n", CONFIG_ENABLE_CC, CONFIG_ENABLE_ED);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "ENABLE_ST7735=%d\nENABLE_TM1638=%d\nENABLE_CHESS=%d\nENABLE_SPISD=%d\nENABLE_PICO_CLOCK_GREEN=%d\nENABLE_DCF77=%d\n",
            CONFIG_ENABLE_ST7735, CONFIG_ENABLE_TM1638, CONFIG_ENABLE_CHESS, CONFIG_ENABLE_SPISD,
            CONFIG_ENABLE_PICO_CLOCK_GREEN, CONFIG_ENABLE_DCF77);
#if defined(CONFIG_BOARD_RP2350)
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "UART0_TX_GPIO=%d\nUART0_RX_GPIO=%d\n",
            CONFIG_UART0_TX_GPIO, CONFIG_UART0_RX_GPIO);
#if CONFIG_ENABLE_SPISD
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "SPI1_BASE=0x%lx\n", (unsigned long)CONFIG_SPI1_BASE);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "SPI1_SCK_GPIO=%d\nSPI1_MOSI_GPIO=%d\nSPI1_MISO_GPIO=%d\nSPI1_CS_GPIO=%d\n",
            CONFIG_SPI1_SCK_GPIO, CONFIG_SPI1_MOSI_GPIO,
            CONFIG_SPI1_MISO_GPIO, CONFIG_SPI1_CS_GPIO);
#endif
#ifdef CONFIG_LED_ONBOARD_GPIO
        /* Optional: the clock persona has no onboard LED to name (its GP25 is
         * the wireless module's chip select, phase17 C1), so the key is simply
         * absent there rather than carrying a wrong number. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "LED_ONBOARD_GPIO=%d\n", CONFIG_LED_ONBOARD_GPIO);
#endif
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "LED_EXT_GPIO=%d\n", CONFIG_LED_EXT_GPIO);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "I2C_RTC_BASE=0x%lx\nI2C_RTC_SDA_GPIO=%d\nI2C_RTC_SCL_GPIO=%d\n",
            (unsigned long)CONFIG_I2C_RTC_BASE, CONFIG_I2C_RTC_SDA_GPIO, CONFIG_I2C_RTC_SCL_GPIO);
#if CONFIG_ENABLE_ST7735
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "SPI0_BASE=0x%lx\n", (unsigned long)CONFIG_SPI0_BASE);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "ST7735_SCK_GPIO=%d\nST7735_MOSI_GPIO=%d\nST7735_CS_GPIO=%d\nST7735_DC_GPIO=%d\nST7735_RST_GPIO=%d\n",
            CONFIG_ST7735_SCK_GPIO, CONFIG_ST7735_MOSI_GPIO,
            CONFIG_ST7735_CS_GPIO, CONFIG_ST7735_DC_GPIO, CONFIG_ST7735_RST_GPIO);
#endif
#if CONFIG_ENABLE_TM1638
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "TM1638_STB_GPIO=%d\nTM1638_CLK_GPIO=%d\nTM1638_DIO_GPIO=%d\n",
            CONFIG_TM1638_STB_GPIO, CONFIG_TM1638_CLK_GPIO, CONFIG_TM1638_DIO_GPIO);
#endif
        /* N5 (plan/phase18_networking_and_auth.md): the gateway persona's
         * UART1 downlink. Keyed on the pins being defined rather than on a
         * feature flag -- the pin map is a board fact and is worth reporting
         * from the moment it exists, which on this persona was one milestone
         * before the driver that uses it. Reported for exactly the reason
         * this file exists: a board should be able to say what it thinks it
         * is wired to, so that a wrong number is a line of output rather than
         * an afternoon. (An Ethernet pin block sat here too until phase 19's
         * R0; R4's ENC28J60 restores one.) */
#ifdef CONFIG_UART1_BASE
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "UART1_BASE=0x%lx\nUART1_TX_GPIO=%d\nUART1_RX_GPIO=%d\n",
            (unsigned long)CONFIG_UART1_BASE, CONFIG_UART1_TX_GPIO, CONFIG_UART1_RX_GPIO);
#endif
#if CONFIG_ENABLE_PICO_CLOCK_GREEN
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "CLOCK_OE_GPIO=%d\nCLOCK_SDI_GPIO=%d\nCLOCK_CLK_GPIO=%d\nCLOCK_LE_GPIO=%d\n",
            CONFIG_CLOCK_OE_GPIO, CONFIG_CLOCK_SDI_GPIO, CONFIG_CLOCK_CLK_GPIO, CONFIG_CLOCK_LE_GPIO);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "CLOCK_A0_GPIO=%d\nCLOCK_A1_GPIO=%d\nCLOCK_A2_GPIO=%d\nCLOCK_ADC_LIGHT_GPIO=%d\n",
            CONFIG_CLOCK_A0_GPIO, CONFIG_CLOCK_A1_GPIO, CONFIG_CLOCK_A2_GPIO, CONFIG_CLOCK_ADC_LIGHT_GPIO);
#ifdef CONFIG_CLOCK_BTN_SET_GPIO
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "CLOCK_BTN_SET_GPIO=%d\nCLOCK_BTN_UP_GPIO=%d\nCLOCK_BTN_DOWN_GPIO=%d\nCLOCK_BUZZER_GPIO=%d\n",
            CONFIG_CLOCK_BTN_SET_GPIO, CONFIG_CLOCK_BTN_UP_GPIO,
            CONFIG_CLOCK_BTN_DOWN_GPIO, CONFIG_CLOCK_BUZZER_GPIO);
#endif
#ifdef CONFIG_CLOCK_TEMP_OFFSET_C
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "CLOCK_TEMP_OFFSET_C=%d\nCLOCK_COLON_BLINK=%d\n",
            CONFIG_CLOCK_TEMP_OFFSET_C, CONFIG_CLOCK_COLON_BLINK);
#endif
#endif
#if CONFIG_ENABLE_DCF77
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "DCF77_OUT_GPIO=%d\nDCF77_PON_GPIO=%d\nDCF77_PON_ACTIVE_LOW=%d\n"
            "DCF77_OUT_ACTIVE_LOW=%d\nDCF77_WARMUP_MS=%d\n",
            CONFIG_DCF77_OUT_GPIO, CONFIG_DCF77_PON_GPIO,
            CONFIG_DCF77_PON_ACTIVE_LOW, CONFIG_DCF77_OUT_ACTIVE_LOW,
            CONFIG_DCF77_WARMUP_MS);
#endif
#endif
        return (int)used;
    }
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    else if (strcmp(rel, "dcf77") == 0) {
        /* The radio's health, without a serial console (D4,
         * plan/phase17_clock_ui_and_dcf77.md). The clock persona's only other
         * window on this is a 24-column LED panel, so a 9P-attached host
         * reading this file is how the receiver gets watched over days rather
         * than minutes -- which is the timescale reception problems actually
         * live on. */
        dcf_status_t st;
        dcf77_service_status(&st);
        uint32_t used = 0;
        char iso[32];

        static const char *const STATE[] = { "idle", "syncing", "done", "failed" };
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "state=%s\nauto=%s\nauto_at=%02d:%02d\n",
            STATE[(unsigned)st.state <= 3 ? (unsigned)st.state : 0],
            dcf77_service_auto() ? "on" : "off",
            CONFIG_DCF77_AUTO_HOUR, CONFIG_DCF77_AUTO_MIN);
        if (st.state == DCF_SYNCING) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "timeout_left_s=%u\n", (unsigned)st.timeout_left_s);
        }

        uint32_t age = dcf77_service_age_s();
        if (st.ever_synced) {
            time_format_iso(&st.last_sync_utc, iso, sizeof(iso));
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "last_sync=%s UTC\nlast_sync_age_s=%u\n", iso, (unsigned)age);
        } else {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "last_sync=never\nlast_sync_age_s=-1\n");
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "sync_attempts=%u\nsync_ok=%u\n",
            (unsigned)st.attempts, (unsigned)st.successes);

        /* Distinct from last_sync: the radio can be decoding perfectly while
         * the clock has deliberately not been touched. */
        if (st.have_radio_time) {
            time_format_iso(&st.radio_utc, iso, sizeof(iso));
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "radio_time=%s UTC\n", iso);
        } else {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "radio_time=none\n");
        }

        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "pulses=%u\npulses_bad=%u\nglitches=%u\nsync_losses=%u\n"
            "frames_seen=%u\nframes_accepted=%u\n"
            "parity_errors=%u\nframing_errors=%u\nrange_errors=%u\nweekday_errors=%u\n"
            "spacing_err_max_ms=%u\nbit_index=%d\nduty_permille=%u\n"
            "quality_mean_tenths=%u\nquality_samples=%u\nlongest_clean_run_s=%u\n",
            (unsigned)st.decoder.pulses_seen, (unsigned)st.decoder.pulses_bad,
            (unsigned)st.decoder.glitches, (unsigned)st.decoder.sync_losses,
            (unsigned)st.decoder.frames_seen, (unsigned)st.decoder.frames_accepted,
            (unsigned)st.decoder.parity_errors, (unsigned)st.decoder.framing_errors,
            (unsigned)st.decoder.range_errors, (unsigned)st.decoder.weekday_errors,
            (unsigned)st.decoder.spacing_err_max_ms, st.decoder.bit_index,
            (unsigned)st.decoder.high_permille,
            (unsigned)(st.decoder.quality_total
                ? (st.decoder.quality_sum * 10u) / st.decoder.quality_total : 0),
            (unsigned)st.decoder.quality_total,
            (unsigned)st.decoder.clean_run_max);

        used += (uint32_t)ksnprintf(buf + used, cap - used, "quality=");
        for (unsigned i = 0; i < st.decoder.quality_count && used + 2 < cap; i++) {
            buf[used++] = (char)('0' + (st.decoder.quality[i] > 7 ? 7 : st.decoder.quality[i]));
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used, "\n");
        return (int)used;
    }
#endif
    return -1;
}

/* Unsized on purpose: /proc/dcf77 exists only where a receiver does, and the
 * one caller that walks this list already derives the count with sizeof. */
static const char *g_proc_names[] = { "ps", "meminfo", "version", "df", "kmsg", "devices", "buildid", "path", "ports", "config", "net", "node",
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    "dcf77",
#endif
};
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
            /* Unlike the MOUNT_PROC branch just above (which rejects
             * anything vfs_generate_proc_content() doesn't recognize),
             * this used to accept *any* name unconditionally, opening a
             * phantom zero-length handle for something like
             * /dev/totally-bogus-name instead of failing -- found via a
             * host tool's 9P client walking to a nonexistent path (a file
             * manager's own routine "does a .Trash-NNNN already exist
             * here?" probe) and getting back a false "yes, and it's a
             * 0-byte file" instead of the expected walk failure. */
            bool known = false;
            for (int i = 0; i < 4; i++) {
                if (strcmp(rel, g_dev_names[i]) == 0) { known = true; break; }
            }
            if (!known) return -1;
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
                ((char *)buf)[0] = console_getc();
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
                uart_flush(); /* M4: this write is its own message; nothing else here would */
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
        case MOUNT_ROOT_HANDLE:
            /* Unreachable: a *mount* never carries the synthetic root
             * handle's kind. Named rather than swept under a `default:` so
             * that adding a real mount kind later is still a compile error
             * here, which is the entire value of switching over an enum. */
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
