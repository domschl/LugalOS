#include "kernel/privilege.h"
#include "kernel/lock.h"
#include "kernel/power.h"
#include "fs/vfs.h"
#include "fs/fat32.h"
#include "fs/9p.h"
#include "fs/p9_link.h"
#include "fs/p9_chan.h"
#include "drivers/block.h"
#include "drivers/psram.h"
#include "drivers/flashdisk.h"
#include "drivers/uart.h"
#include "drivers/at24c32.h"
#include "drivers/loopback_net.h"
#include "drivers/uart_net.h"
#include "kernel/discipline.h"
#include "drivers/cyw43.h"
#include "drivers/i2c_rtc.h"
#include "drivers/bme280.h"
#include "drivers/bme680.h"
#include "drivers/ccs811.h"
#include "drivers/sgp30.h"
#include "drivers/tsl2561.h"
#include "drivers/tsl2591.h"
#include "drivers/mics6814.h"
#include "drivers/mhz19b.h"
#include "drivers/hdc1080.h"
#include "drivers/sensor_hub.h"
#include "drivers/piousb.h"
#include "kernel/time.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/devirq.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/clipboard.h"
#if defined(CONFIG_BOARD_ESP32C6)
#include "kernel/radio_c6.h"
#include "drivers/watchdog.h"
#endif
#include "kernel/llp_node.h"
#include "kernel/ota.h"
#include "kernel/klog.h"
#include "kernel/device.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "kernel/identity.h"
#include "kernel/taint.h"
#include "kernel/sha256.h"
#include "kernel/chan.h"
#include "kernel/hart.h"
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
#include "drivers/dcf77.h"
#include "drivers/dcf77_service.h"
#include "drivers/dcf77_p0log.h"
#include "drivers/gps_pps.h"
#endif

/* guarded by: each volume's own fs->lock (fs/fat32.c) for every operation;
 * re-initialised only by `format` and a new RAM disk, which first withdraw
 * the volume from lookups and refuse one with users (mount_quiesce()). */
static fat32_fs_t g_fat32_sd;
static fat32_fs_t g_fat32_ram;
static fat32_fs_t g_fat32_flash;

/* 38.0: /sd0's FAT-sector cache (fat32_set_fat_cache()). Only where there is
 * a card to read: on a card every FAT lookup is a sector transfer, while
 * /ram0 and /flash0 are memcpy and would gain nothing for the 512 bytes. */
#if (defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_SPISD) || \
    (defined(CONFIG_BOARD_ESP32P4) && CONFIG_ENABLE_SDMMC) || \
    (!defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_BOARD_ESP32P4) && !defined(CONFIG_BOARD_ESP32C6) && !defined(CONFIG_BOARD_K210))
#define VFS_SD_FAT_CACHE 1
static uint32_t g_sd_fat_cache[128];
#endif
/* guarded by: mount_quiesce() for `format` and a new RAM disk; set at boot otherwise. */
static bool g_sd_mounted = false;
static bool g_ram_mounted = false;
/* single task: the boot, before any reader runs; constant after. */
static bool g_flash_mounted = false;

typedef struct {
    char name[32];
    int target_pid;
} service_entry_t;

#define MAX_SERVICES 8
/* single task: the boot, before any reader runs; constant after. */
static service_entry_t g_services[MAX_SERVICES];
static int g_num_services = 0;

/* --- Mount table ---
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
    uint8_t probe_misses;         /* vfs_volume_probe()'s failures in a row */
    /* Lifetime: users counts the operations and open handles on the entry;
     * an unmount marks it dying (no new lookups find it) and the last user's
     * mount_put() clears it. ready: filled in (a remote mount's handshake
     * runs after its slot is taken). */
    uint16_t users;
    bool dying;
    bool ready;
} mount_entry_t;

#define MAX_MOUNTS 10
/* guarded by: g_mount_lock for every change of an entry's state (taking,
 * readying, users, dying, clearing) and for lookups; a found entry stays
 * valid while its caller holds a reference (mount_get/mount_put). The
 * listings (`ls /`, /proc/mounts) read names unlocked; they can show one
 * mid-change. */
static mount_entry_t g_mounts[MAX_MOUNTS];
static spinlock_t    g_mount_lock;

/* --- Handle table ---
 * A flat array, not a union: about 250 bytes a handle, the /proc text living
 * in a page of its own while the file is open. `kind` mirrors the owning mount's mount_kind_t, plus a
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
    fat32_cursor_t cursor;       /* MOUNT_FAT32: where in the chain this handle last was (38.0) */
    bool wrote;                  /* MOUNT_FAT32: sync FSInfo on close */
    uint32_t dir_cluster;        /* valid for MOUNT_FAT32, is_dir */
    char rel_path[128];          /* path within volume (FAT32) or device name (DEV) */
    /* Generated /proc file content (PROC, non-dir): one page, taken at open
     * (from the bulk zone where there is one) and given back at close. It
     * used to be 896 bytes inside every handle, which cut /proc/ps off at
     * thirteen tasks and cost 896 bytes per handle for a /proc file few
     * handles ever hold. */
    char *proc_buf;
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
    mount_entry_t *mnt;               /* the mount reference the open handle holds */
} vfs_handle_t;

/* guarded by: g_handles_lock for taking a slot (in_use); an open handle is
 * its opener's, and freeing it is one clear of in_use. */
static vfs_handle_t g_handles[VFS_MAX_HANDLES];
static spinlock_t   g_handles_lock = SPINLOCK_INIT;
/* guarded by: g_handles_lock. How full the table has been, how often an open
 * found it full and waited, and how often the wait ran out. */
static uint32_t g_handles_peak, g_handles_waits, g_handles_none;
#define VFS_OPEN_WAIT_MS 500u

void vfs_handle_stats(vfs_handle_stats_t *out) {
    uintptr_t f = spin_lock_irqsave(&g_handles_lock);
    out->peak = g_handles_peak;
    out->waits = g_handles_waits;
    out->none = g_handles_none;
    spin_unlock_irqrestore(&g_handles_lock, f);
}

#include "drivers/spisd.h"
#include "drivers/sdmmc.h"

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

/* `ready` false: the caller fills the entry in and readies it with
 * mount_ready(); lookups pass it by until then. A dying entry's name is free
 * for a new mount (a radio that restarted is mounted again while handles to
 * the old session close). */
static mount_entry_t *mount_alloc(const char *name, bool ready) {
    if (!name || !name[0] || strlen(name) >= sizeof(g_mounts[0].name)) return NULL;
    mount_entry_t *e = NULL;
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    bool taken = false;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && !g_mounts[i].dying && strcmp(g_mounts[i].name, name) == 0) taken = true;
    }
    for (int i = 0; !taken && i < MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) {
            e = &g_mounts[i];
            memset(e, 0, sizeof(*e));
            strncpy(e->name, name, sizeof(e->name) - 1);
            e->in_use = true;
            e->ready = ready;
            break;
        }
    }
    spin_unlock_irqrestore(&g_mount_lock, f);
    return e;   /* NULL: the name is taken, or the table is full */
}

static void mount_ready(mount_entry_t *e) {
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    e->ready = true;
    spin_unlock_irqrestore(&g_mount_lock, f);
}

/* Gives up a slot that never became ready. */
static void mount_abandon(mount_entry_t *e) {
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    memset(e, 0, sizeof(*e));
    spin_unlock_irqrestore(&g_mount_lock, f);
}

/* Drops a reference from mount_get() (NULL is fine). The last one on a dying
 * entry clears it and closes its remote session, outside the lock. */
uint32_t vfs_mount_users(const char *name) {
    uint32_t n = 0;
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++)
        if (g_mounts[i].in_use && strcmp(g_mounts[i].name, name) == 0) n = g_mounts[i].users;
    spin_unlock_irqrestore(&g_mount_lock, f);
    return n;
}

static void mount_put(mount_entry_t *m) {
    if (!m) return;
    p9_remote_mount_t *close = NULL;
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    if (m->users) m->users--;
    if (m->dying && m->users == 0) {
        close = m->remote;
        memset(m, 0, sizeof(*m));
    }
    spin_unlock_irqrestore(&g_mount_lock, f);
    if (close) p9_remote_mount_close(close);
}

/* Takes the named volume out of lookups for a re-initialisation (`format`,
 * a new RAM disk), if no one but `own` references holds it: the test and the
 * withdrawal are one step, so no open can slip in between. True: withdrawn
 * (or no such mount); give it back with mount_ready(). */
static bool mount_quiesce(const char *name, uint16_t own, mount_entry_t **out) {
    bool ok = true;
    *out = NULL;
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry_t *e = &g_mounts[i];
        if (!e->in_use || e->dying || strcmp(e->name, name) != 0) continue;
        if (e->users > own) ok = false;
        else { e->ready = false; *out = e; }
        break;
    }
    spin_unlock_irqrestore(&g_mount_lock, f);
    return ok;
}

static bool mount_is_active(const mount_entry_t *m) {
    if (!m->in_use) return false;
    if (m->kind == MOUNT_FAT32) {
        if (m->fs && m->fs->dev && m->fs->dev->lost) return false;   /* card pulled */
        return m->mounted_ptr ? *m->mounted_ptr : true;
    }
    return true; // proc/dev/srv/remote9p are always "active" once registered
}

static void mount_table_init(void) {
    memset(g_mounts, 0, sizeof(g_mounts));

    mount_entry_t *e;
    e = mount_alloc("flash0", true);
    e->kind = MOUNT_FAT32; e->fs = &g_fat32_flash; e->mounted_ptr = &g_flash_mounted;
    /* Read-only everywhere the backing store cannot be written, which until
     * E6 was everywhere. The QEMU targets back /flash0 with a C array in the
     * kernel image; RP2350 reads it out of the XIP window and
     * drivers/flashdisk.c's write path there prints "read-only" and returns
     * -1. Marking the mount read-only is what turns that into a clean refusal
     * at the VFS instead of a failed write three layers down.
     *
     * The ESP32-P4 is the first target where it is genuinely writable: E6
     * puts the segment in real flash and
     * gives flashdisk a read-modify-erase-write path through the boot ROM. So
     * the flag follows the hardware rather than the filename. */
#if defined(CONFIG_BOARD_ESP32P4)
    e->read_only = false;
    strncpy(e->label, "FAT32 SPI Flash", sizeof(e->label) - 1);
#else
    e->read_only = true;
    strncpy(e->label, "FAT32 Embedded Flash ROM", sizeof(e->label) - 1);
#endif

    e = mount_alloc("sd0", true);
    e->kind = MOUNT_FAT32; e->fs = &g_fat32_sd; e->mounted_ptr = &g_sd_mounted;
    strncpy(e->label, "FAT32 Persistent SD", sizeof(e->label) - 1);

    e = mount_alloc("ram0", true);
    e->kind = MOUNT_FAT32; e->fs = &g_fat32_ram; e->mounted_ptr = &g_ram_mounted;
    strncpy(e->label, "FAT32 In-Memory RAMDisk", sizeof(e->label) - 1);

    e = mount_alloc("proc", true);
    e->kind = MOUNT_PROC;
    strncpy(e->label, "Synthetic Metrics System", sizeof(e->label) - 1);

    e = mount_alloc("dev", true);
    e->kind = MOUNT_DEV;
    strncpy(e->label, "Hardware Device Nodes", sizeof(e->label) - 1);

    e = mount_alloc("srv", true);
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
    mount_entry_t *e = mount_alloc(name, false);
    if (!e) return -1;

    p9_remote_mount_t *remote = p9_remote_mount_open(link);
    if (!remote) {
        mount_abandon(e);
        return -1;
    }

    e->kind = MOUNT_REMOTE9P;
    e->remote = remote;
    ksnprintf(e->label, sizeof(e->label), "Remote 9P Namespace (%s)", link->name ? link->name : "?");
    mount_ready(e);
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
    mount_entry_t *m = NULL;
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry_t *e = &g_mounts[i];
        if (e->in_use && e->ready && !e->dying && e->kind == MOUNT_REMOTE9P && strcmp(e->name, name) == 0) {
            e->dying = true;
            e->users++;               /* this call's own, dropped below */
            m = e;
            break;
        }
    }
    spin_unlock_irqrestore(&g_mount_lock, f);
    if (!m) return -1;
    printk("[VFS Server] Unmounted /%s/\n", name);
    mount_put(m);                     /* clears it now, or when its last user leaves */
    return 0;
}

void vfs_server_init(void) {
    g_sd_mounted = false;
    g_ram_mounted = false;
    g_flash_mounted = false;

    /* Mount embedded Flash ROM filesystem on /flash0/ */
    vfs_mount_flashdisk();

#ifdef VFS_SD_FAT_CACHE
    /* Before the mount: fat32_init() keeps the buffer and empties it, so the
     * cache also survives (format "/sd0") remounting the same struct. */
    fat32_set_fat_cache(&g_fat32_sd, g_sd_fat_cache);
#endif

#if defined(CONFIG_BOARD_RP2350)
    /* Try mounting physical MicroSD card via SPI1 (GP10-GP13) on RP2350 */
    block_dev_t *sd_dev = spisd_get_device();
    if (sd_dev) {
        if (fat32_init(&g_fat32_sd, sd_dev) == 0) {
            g_sd_mounted = true;
            printk("[VFS Server] Mounted FAT32 Filesystem on /sd0/ (Device: SPI1 MicroSD Card Reader)\n");
        }
    }
#elif defined(CONFIG_BOARD_ESP32C6)
    /* no block device on this board (no card slot;
     * /flash0 arrives with 45.4.3), so /sd0 stays unmounted and `df` says so. */
#elif defined(CONFIG_BOARD_K210)
    /* no block device on this board (65.3): the SPI NOR is a 65.5 owner,
     * so /sd0 stays unmounted and `df` says so. */
#elif defined(CONFIG_BOARD_ESP32P4)
    /* the microSD card in the NANO's
     * own slot, on the SD/MMC host controller rather than on a SPI bus.
     * Where E2 had nothing to mount here, an empty slot now reports itself
     * the same way an unformatted card does -- /sd0 stays unmounted and `df`
     * says so. */
    block_dev_t *sd_dev = sdmmc_get_device();
    if (sd_dev) {
        if (fat32_init(&g_fat32_sd, sd_dev) == 0) {
            g_sd_mounted = true;
            printk("[VFS Server] Mounted FAT32 Filesystem on /sd0/ (Device: SD/MMC MicroSD Card Reader)\n");
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

    printk("[VFS Server] Namespace resolver online (PID %d).\n", VFS_PID);
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

bool vfs_volume_mounted(const char *name) {
    if (!name) return false;
    while (*name == '/') name++;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry_t *m = &g_mounts[i];
        if (!m->in_use || !m->name[0]) continue;

        const char *a = name;
        const char *b = m->name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b != '\0') continue;
        if (*a != '\0' && *a != '/') continue;

        return mount_is_active(m);
    }
    return false;
}

void vfs_volume_probe(const char *name) {
    if (!name) return;
    while (*name == '/') name++;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry_t *m = &g_mounts[i];
        if (!m->in_use || m->kind != MOUNT_FAT32 || strcmp(m->name, name) != 0) continue;
        if (!mount_is_active(m) || !m->fs->dev->probe) return;
        /* Two misses in a row: one could be a card mid-write. */
        if (m->fs->dev->probe(m->fs->dev) == 0) {
            m->probe_misses = 0;
        } else if (++m->probe_misses >= 2) {
            m->fs->dev->lost = true;
            printk("[VFS Server] /%s/: the card is gone; unmounted until a reboot\n", m->name);
        }
        return;
    }
}

static int vfs_mount_ramdisk_quiet(int size_kb);

/* Not under open files: a new RAM disk replaces the storage under them. */
int vfs_mount_ramdisk(int size_kb) {
    mount_entry_t *e;
    if (!mount_quiesce("ram0", 0, &e)) {
        printk("[VFS Server] /ram0/ is in use; not replaced\n");
        return -1;
    }
    int rc = vfs_mount_ramdisk_quiet(size_kb);
    if (e) mount_ready(e);
    return rc;
}

static int vfs_mount_ramdisk_quiet(int size_kb) {
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
        /* Quiet, because a blank RAM disk is the expected state on every
         * boot, not a fault.
         * This used to narrate one expected event in four lines; the
         * "Mounted ... on /ram0/" line below is the one that is news.
         *
         * The failure path is now checked, which it was not before: a format
         * that does not take used to leave g_ram_mounted true and a
         * filesystem that answers every write with an error. */
        if (fat32_init_quiet(&g_fat32_ram, ram_dev, true) != 0 ||
            g_fat32_ram.bpb.tot_sec32 != ram_dev->num_blocks) {
            fat32_format_quiet(ram_dev, true);
            if (fat32_init_quiet(&g_fat32_ram, ram_dev, true) != 0) {
                printk("[VFS Server] /ram0/ not mounted: the RAM disk would not format.\n");
                return -1;
            }
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
/* Finds the mount a path is on and takes a reference to it: the caller ends
 * with mount_put(), and until then the entry cannot be cleared under it. */
static mount_entry_t *mount_get(const char *path, const char **rel_path, bool *is_root) {
    static const char *empty_str = "";   /* accepted: only ever assigned itself */
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

    mount_entry_t *m = NULL;
    uintptr_t f = spin_lock_irqsave(&g_mount_lock);
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_entry_t *e = &g_mounts[i];
        if (e->in_use && e->ready && !e->dying && strcmp(e->name, comp) == 0) {
            e->users++;
            m = e;
            break;
        }
    }
    spin_unlock_irqrestore(&g_mount_lock, f);
    if (m) *rel_path = slash ? slash + 1 : empty_str;
    return m;   /* NULL: no such mount */
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

static int vfs_generate_proc_content_raw(const char *rel, char *buf, uint32_t cap) {
    if (!rel || !buf || cap == 0) return -1;
    uint32_t used = 0;

    if (strcmp(rel, "ps") == 0) {
        /* Task table formatted for /proc/ps.
         *
         * The "Isol" column reports whether each task actually runs under
         * hardware-enforced isolation (task_set_domain() was called on
         * it) rather than in kernel mode. Driver and user tasks show the
         * real backend name once they enter U-mode (PMP on RV32/RP2350,
         * Sv39 on RV64_MMU); kernel tasks show "-". */
        /* "Isol" trails Exit rather than sitting between Name and Exit --
         * tests/hw and tests/runner.py both have existing patterns
         * (`uprog\s+42`, `uprog\s+killed`) asserting Name is immediately
         * followed by the exit outcome; keeping that adjacency means this
         * column addition costs nothing elsewhere instead of needing every
         * such pattern hunted down and loosened. Found the hard way (a
         * QEMU rv64 run that looked like a hang was actually 3 full
         * retries failing the same two now-broken regexes). */
        used += (uint32_t)ksnprintf(buf + used, cap - used, "PID  State    Name          Exit   Isol  Hart  Stack\n");
        used += (uint32_t)ksnprintf(buf + used, cap - used, "---  -------  ------------  ----   ----  ----  ---------\n");
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
            /* X2: which hart this task may run on. "any" is the default and
             * the common case; a number means it is pinned, which for a
             * driver task is load-bearing rather than decorative -- several
             * of S5 dispositions rest on it. */
            {
                char hbuf[8];
                int aff = task_affinity(pid);
                if (aff < 0) ksnprintf(hbuf, sizeof(hbuf), "any");
                else         ksnprintf(hbuf, sizeof(hbuf), "%d", aff);
                used = append_col(buf, used, cap, hbuf, 6);
            }
            /* Stack high-water against the size it was given (§6,
             * ). "-" for the boot task,
             * whose stack is the linker's and is reported by the Boot Stack
             * line in /proc/meminfo instead. */
            {
                /* The deeper of the poison scan and the stack pointer seen
                 * at switches and traps: a large frame can jump the poison. */
                uint32_t su = sched_stack_used(pid);
                uint32_t sp_depth = sched_stack_sp_depth(pid);
                if (sp_depth > su) su = sp_depth;
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
            uint32_t total_b = 0, free_b = 0;
            fat32_statfs(&g_fat32_flash, &total_b, &free_b);
            uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
            uint32_t pct = total_b ? (uint32_t)((uint64_t)used_b * 100u / total_b) : 100;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "/flash0/         %9u  %9u  %9u     %3u%% /flash0/\n", total_b, used_b, free_b, pct);
        }
        if (g_ram_mounted) {
            uint32_t total_b = 0, free_b = 0;
            fat32_statfs(&g_fat32_ram, &total_b, &free_b);
            uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
            uint32_t pct = total_b ? (uint32_t)((uint64_t)used_b * 100u / total_b) : 0;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "/ram0/           %9u  %9u  %9u     %3u%% /ram0/\n", total_b, used_b, free_b, pct);
        }
        if (g_sd_mounted) {
            uint32_t total_b = 0, free_b = 0;
            fat32_statfs(&g_fat32_sd, &total_b, &free_b);
            uint32_t used_b = total_b >= free_b ? total_b - free_b : 0;
            uint32_t pct = total_b ? (uint32_t)((uint64_t)used_b * 100u / total_b) : 0;
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
         * The whole report has to land inside the handle's one-page proc_buf
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
         * ). The one Image figure above
         * shows that it moved; these show what moved -- and on this board
         * every byte of them is a heap byte nothing else can have. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "    .data %u B, .bss %u KB", map.data_bytes, map.bss_bytes / 1024);
        /* Bytes below a kilobyte, not a rounded-down "0 KB". The RP2350
         * personas carry 22 KB of U-mode stacks and read fine either way; the
         * ESP32-P4 has one 512-byte probe stack (E5), which "0 KB" reports as
         * absent when it is present. A field that exists to make this memory
         * visible should not round it out of existence. */
        if (map.ustacks_bytes) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                ", ustacks %u %s",
                map.ustacks_bytes >= 1024 ? map.ustacks_bytes / 1024
                                          : map.ustacks_bytes,
                map.ustacks_bytes >= 1024 ? "KB" : "B");
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
#if defined(CONFIG_PSRAM_BYTES)
        used += (uint32_t)psram_meminfo(buf + used, cap - used);
#endif
        /* 38.4: the bulk page zone, where there is one. SRAM fallbacks should
         * stay 0; each `zonetest` run adds exactly one on purpose (its step 5
         * exhausts the zone to prove the fallback works). */
        palloc_zone_stats_t bz;
        if (palloc_bulk_stats(&bz)) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "  Bulk Pages: %u total, %u free, peak %u, largest run %u, SRAM fallbacks %u\n",
                bz.total_pages, bz.free_pages, bz.peak_used_pages, bz.largest_free_run, bz.fallbacks);
        }

        /* Read off the mount table rather than spelled out as a constant.
         * The fixed string this replaces named "/sd0/ (VirtIO SD)" on every
         * target -- including RP2350, which has no VirtIO at all, and
         * including personas where /sd0 cannot mount because those pins
         * drive something else entirely (the clock baseboard's GP10-13 are
         * the LED matrix's shift registers, see cmake/board-rp2350-clock.cmake).
         * So this line claimed storage that was not there, in terms that were
         * wrong for the board reading it, while `ls /` two functions away
         * had the truth the whole time. */
        used += (uint32_t)ksnprintf(buf + used, cap - used, "  Storage:");
        bool first_mount = true;
        for (int i = 0; i < MAX_MOUNTS; i++) {
            if (!g_mounts[i].in_use || g_mounts[i].kind != MOUNT_FAT32) continue;
            used += (uint32_t)ksnprintf(buf + used, cap - used, "%s /%s/ (%s)",
                first_mount ? "" : ",", g_mounts[i].name,
                mount_is_active(&g_mounts[i]) ? g_mounts[i].label : "not mounted");
            first_mount = false;
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used, "\n");
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
        /* the device-scope
         * UID, absent unless silicon or a provisioned record supplied one --
         * "unset" would claim more than is known, so this says "none". */
        uint8_t uid[NODE_UID_LEN];
        if (node_uid(uid)) {
            /* Hand-rolled because printk's %x used to read every argument
             * as a long, which printed an int wrongly on rv64. That is fixed
             * (`fmtselftest`); this stays because it is correct anyway. */
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
        /* I3: the device key, never itself -- only its fingerprint, the one
         * thing §6's toolset is allowed to print. Not yet consulted by the
         * 9P auth path (I4), so this can be provisioned and inspected on a
         * node that still authenticates the phase-18 way. */
        {
            uint8_t key[NODE_DEVKEY_MAX];
            uint32_t key_len = 0;
            if (node_devkey(key, sizeof(key), &key_len)) {
                char fp[KEY_FINGERPRINT_HEX_LEN + 1];
                key_fingerprint_hex(key, key_len, fp);
                used += (uint32_t)ksnprintf(buf + used, cap - used, "key fingerprint: %s\n", fp);
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "key fingerprint: none\n");
            }
            memset(key, 0, sizeof(key));
        }
        /* I6: same discipline -- the derived PSK never appears, only its
         * fingerprint. The SSID is not a secret (it is broadcast in every
         * beacon frame the AP sends), so it prints in full. */
        {
            char ssid[NODE_WLAN_SSID_MAX + 1];
            uint8_t psk[NODE_WLAN_PSK_LEN];
            bool have_ssid = node_wlan_ssid(ssid, sizeof(ssid));
            bool have_psk = node_wlan_psk(psk);
            if (have_ssid) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "wlan ssid: %s\n", ssid);
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "wlan ssid: none\n");
            }
            if (have_psk) {
                char fp[KEY_FINGERPRINT_HEX_LEN + 1];
                key_fingerprint_hex(psk, sizeof(psk), fp);
                used += (uint32_t)ksnprintf(buf + used, cap - used, "wlan psk fingerprint: %s\n", fp);
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "wlan psk fingerprint: none\n");
            }
            memset(psk, 0, sizeof(psk));

            /* The address this board comes up on, if one is stored. Reported
             * here rather than only by `netcfg` because /proc/node is what a
             * host reads over 9P -- and "which address will this board have
             * after the next reboot" is exactly the question someone asks
             * remotely, before rebooting it. */
            uint8_t nip[NODE_IPV4_LEN], nmask[NODE_IPV4_LEN], ngw[NODE_IPV4_LEN];
            if (node_ipv4(nip, nmask, ngw)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "ipv4: %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n",
                    nip[0], nip[1], nip[2], nip[3],
                    nmask[0], nmask[1], nmask[2], nmask[3],
                    ngw[0], ngw[1], ngw[2], ngw[3]);
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "ipv4: none\n");
            }

            /* Q6: where this board publishes, so it can be asked over 9P
             * before rebooting it -- the same reason the address above is
             * here. The password is named, never shown, like the device key
             * fingerprint further up. */
            node_mqtt_t mq;
            if (node_mqtt(&mq)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "mqtt: %u.%u.%u.%u:%u user %s pass %s prefix %s\n",
                    mq.broker[0], mq.broker[1], mq.broker[2], mq.broker[3],
                    mq.port ? mq.port : 1883u,
                    mq.username[0] ? mq.username : "(none)",
                    mq.password[0] ? "set" : "(none)",
                    mq.prefix[0] ? mq.prefix : "lugalos");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "mqtt: none\n");
            }
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "persona: %s\nbuild seed: %s\ntaint: %s\n", CONFIG_NODE_PERSONA, CONFIG_NODE_SEED, LUGALOS_TAINT);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "9P uname: %s\n", node_name());
        /* The isolation verdict, askable over 9P like everything else:
         * a host deciding whether to trust this node with an unattended
         * multi-program workload reads this, not the boot log. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "domains: %s\n", mem_domain_status());
        return (int)used;
    } else if (strcmp(rel, "net") == 0) {
        /* the interface, its
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
         * Every drop is a separate line for the reason learned
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
                for (uint32_t i = 0; i < tcp_max_conns(); i++) {
                    char line[80];
                    tcp_conn_str(i, line, sizeof(line));
                    if (line[0]) used += (uint32_t)ksnprintf(buf + used, cap - used, "  %s", line);
                }
                used += (uint32_t)ksnprintf(buf + used, cap - used, "9p: %lu of %u fids in use\n",
                    (unsigned long)p9_server_fids_in_use(), (unsigned)P9_MAX_FIDS);
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
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_WL_CS_GPIO)
            /* The radio's own account of itself, and the reason it is here
             * rather than in the log: a drop reason printed to /proc/kmsg dies
             * when the board is unplugged to go and read it. These survive the
             * reconnection, so they can be read afterwards, over the very link
             * whose loss they describe. */
            {
                cyw43_link_status_t w;
                cyw43_link_status(&w);
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "wlan link: %s\nwlan rssi_dbm: %s%ld\n"
                    "wlan joins: %lu\nwlan drops: %lu\n",
                    w.link_up ? "up" : "down",
                    w.rssi_valid ? "" : "stale ", (long)w.rssi_dbm,
                    (unsigned long)w.joins, (unsigned long)w.drops);
                uint32_t stalls = 0, stall_timeouts = 0;
                cyw43_tx_stats(&stalls, &stall_timeouts);
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "wlan tx credit waits: %lu (%lu gave up)\n",
                    (unsigned long)stalls, (unsigned long)stall_timeouts);
                if (w.drops) {
                    /* A drop the AP announced and one we had to go and
                     * discover are different animals, and the distinction is
                     * the first thing worth knowing: an announced deauth is
                     * the network behaving normally, a polled one means
                     * something went away silently. */
                    if (w.last_drop_ev == 0xFFFFFFFFu) {
                        used += (uint32_t)ksnprintf(buf + used, cap - used,
                            "wlan last drop: found by poll (no event) %lus ago\n",
                            (unsigned long)w.s_since_drop);
                    } else {
                        used += (uint32_t)ksnprintf(buf + used, cap - used,
                            "wlan last drop: ev=%lu status=%lu reason=%lu %lus ago\n",
                            (unsigned long)w.last_drop_ev,
                            (unsigned long)w.last_drop_status,
                            (unsigned long)w.last_drop_reason,
                            (unsigned long)w.s_since_drop);
                    }
                }
                if (w.link_up) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "wlan up for: %lus\n", (unsigned long)w.s_since_up);
                }
            }
#endif
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
        /* B0 device registry (kernel/device.h), into the handle's proc_buf. */
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
            "LugalOS v%s (Bare-Metal RISC-V Lisp Machine)%s%s\n", LUGALOS_VERSION,
            LUGALOS_TAINTED ? " tainted: " : "", LUGALOS_TAINTED ? LUGALOS_TAINT : "");
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "machine: %s, %s, %s\n",
#if defined(CONFIG_TARGET_RV32)
            "rv32imac",
#elif defined(CONFIG_BOARD_K210)
            "rv64imac",
#elif defined(CONFIG_TARGET_RV64)
            "rv64gc",
#else
            "riscv",
#endif
#if defined(CONFIG_NOMMU)
            "nommu",
#elif defined(CONFIG_MMU)
            "sv39",
#else
            "?mmu",
#endif
            mem_domain_status());
        return (int)used;
    } else if (strcmp(rel, "cpuinfo") == 0) {
        /* S0, : which hart is
         * executing, read back from the record `tp` points at.
         *
         * This exists because the claim it reports is not one the rest of
         * the suite can make. Every other test here passes just as well with
         * a kernel that cannot identify its own hart at all -- that was true
         * of every build before S0. The specific thing worth showing is that
         * the answer is available *in S-mode*, where the obvious
         * implementation (`csrr mhartid`) traps as an illegal instruction
         * because arch/riscv/common/entry.S performs the M->S transition
         * itself and leaves no machine-mode CSR reachable. If this file
         * renders on the RV64 build, that problem is solved; if S0 were
         * wrong in that particular way, the build would not get this far
         * without a fault.
         *
         * `consistent` is the assertion rather than the decoration. It walks
         * back from the id in the record to the array slot that id names and
         * checks the pointer `tp` actually holds is that slot -- so a `tp`
         * pointing at the right kind of thing but the wrong entry, which is
         * exactly the failure mode a second hart would introduce, shows as
         * `no` rather than as a plausible-looking id. */
        const hart_t *self = hart_self();
        unsigned id = hart_id();
        bool consistent = (id < MAX_HARTS) && (&g_harts[id] == self);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "hart:        %u\n", id);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "harts_max:   %d\n", MAX_HARTS);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "harts_online: %u\n", smp_harts_online());
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "record:      0x%lx\n", (unsigned long)(uintptr_t)self);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "consistent:  %s\n", consistent ? "yes" : "no");
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "priv:        %s\n",
#if defined(CONFIG_MODE_S)
            "S"
#else
            "M"
#endif
            );
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "xlen:        %d\n", (int)(__riscv_xlen));
        /* X2: restricted-domain activations per hart. Zero for hart 1 on a
         * single-hart build, and the point of reporting it is that a test
         * can tell "isolation was never exercised elsewhere" apart from
         * "isolation held elsewhere". */
        for (unsigned h = 0; h < MAX_HARTS; h++) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "domains_hart%u: %u\n", h, mem_domain_activations(h));
        }
#if defined(CONFIG_BOARD_RP2350)
        /* X3 probe: did a secondary core execute this image? See
         * linker/rp2350.ld's .smpmark. 0x51C0DE01 means core 1 reached
         * _reset_handler; anything else means it never ran our code -- it is
         * still parked in the bootrom, and waking it needs the SIO-FIFO
         * launch sequence rather than a stack. */
        {
            extern uint32_t _smp_mark;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "core1_probe: 0x%lx\n", (unsigned long)_smp_mark);
        {
            extern uint32_t _smp_mark0;
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "core0_probe: 0x%lx\n", (unsigned long)_smp_mark0);
        }
        }
#endif
        return (int)used;
    } else if (strcmp(rel, "config") == 0) {
        /* Board config: the generated
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
#ifdef CONFIG_LED_EXT_GPIO
        /* Optional too since 36.0: the RP2350-LCD-7 has no user LED at all. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "LED_EXT_GPIO=%d\n", CONFIG_LED_EXT_GPIO);
#endif
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
        /* the gateway persona's
         * UART1 downlink. Keyed on the pins being defined rather than on a
         * feature flag -- the pin map is a board fact and is worth reporting
         * from the moment it exists, which on this persona was one milestone
         * before the driver that uses it. Reported for exactly the reason
         * this file exists: a board should be able to say what it thinks it
         * is wired to, so that a wrong number is a line of output rather than
         * an afternoon. (An Ethernet pin block sat here too until          * R0; R4's ENC28J60 restores one.) */
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
#ifdef CONFIG_LCD_PCLK_GPIO
        /* the RP2350-LCD-7's panel
         * and its PIO-USB host port. Keyed on the pins, like UART1 above. Only
         * this persona has them, and it has none of the ST7735/TM1638/clock/
         * DCF77 blocks, so proc_buf's all-flags worst case does not grow. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "LCD_DE_GPIO=%d\nLCD_PCLK_GPIO=%d\nLCD_DATA0_GPIO=%d\n"
            "LCD_RST_GPIO=%d\nLCD_BL_GPIO=%d\nLCD_EN_GPIO=%d\n",
            CONFIG_LCD_DE_GPIO, CONFIG_LCD_PCLK_GPIO, CONFIG_LCD_DATA0_GPIO,
            CONFIG_LCD_RST_GPIO, CONFIG_LCD_BL_GPIO, CONFIG_LCD_EN_GPIO);
#endif
#ifdef CONFIG_PIOUSB_DP_GPIO
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "PIOUSB_DP_GPIO=%d\nPIOUSB_DM_GPIO=%d\n",
            CONFIG_PIOUSB_DP_GPIO, CONFIG_PIOUSB_DM_GPIO);
#endif
#ifdef CONFIG_PSRAM_BYTES
        /* 38.2: the PSRAM's chip select and its configured size. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "PSRAM_CS_GPIO=%d\nPSRAM_BYTES=%lu\n",
            CONFIG_PSRAM_CS_GPIO, (unsigned long)CONFIG_PSRAM_BYTES);
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
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)
    else if (strcmp(rel, "usbhost") == 0) {
        /* 36.7: the PIO-USB engine's counters, key=value (drivers/piousb.h). */
        return piousb_proc_render(buf, cap);
    }
#endif
#if defined(CONFIG_LLP)
    else if (strcmp(rel, "llp") == 0) {
        /* The link to this node's LLP peer, both roles (kernel/llp_node.c). */
        return (int)llp_node_proc_render(buf, cap);
    }
#endif
#if defined(CONFIG_BOARD_ESP32C6)
    else if (strcmp(rel, "radio") == 0) {
        return (int)radio_c6_proc_render(buf, cap);
    }
    else if (strcmp(rel, "wlanscan") == 0) {
        return (int)radio_c6_scan_render(buf, cap);
    }
    else if (strcmp(rel, "wdt") == 0) {
        return (int)watchdog_proc_render(buf, cap);
    }
#endif
    else if (strcmp(rel, "sensors") == 0) {
        /* the environment sensor, for a
         * reader that is not on this board.
         *
         * Served entirely from drivers/bme280.c's cache. This function runs on
         * the 9P task, and that task must not take the I2C bus -- the rule and
         * its reason are in drivers/i2c_rtc.h. What keeps the cache worth
         * reading is bme280_sampler_start()'s task, not this read.
         *
         * `age_s` is therefore part of the answer, not a diagnostic: a
         * consumer deciding whether to publish a value needs to tell a fresh
         * reading from a frozen one, and over a 9P link it has no other way
         * to know. reads/failures give it the same judgement about the bus.
         *
         * key=value, one per line, integers in the units the driver already
         * uses, because the parser on the other end may be a shell script.
         * Scaling here would put a decimal point in two places. */
        uint32_t used = 0;
        if (bme680_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                bme680_part_name(), bme680_address(),
                (unsigned long)sensor_hub_sample_period_s());

            bme680_reading_t r;
            uint32_t age_s = 0;
            if (!bme680_cached(&r, &age_s)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\ntemperature_c100=%ld\npressure_pa=%lu\nhumidity_rh1000=%lu\ngas_resistance_ohm=%lu\n",
                    (unsigned long)age_s, (long)r.temperature_c100,
                    (unsigned long)r.pressure_pa,
                    (unsigned long)((uint32_t)r.humidity_cpercent * 10u),
                    (unsigned long)r.gas_res_ohm);

                int32_t alt;
                if (node_altitude(&alt)) {
                    int32_t msl = sensor_calc_sea_level_pa((int32_t)r.pressure_pa, r.temperature_c100, alt);
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "altitude_m=%ld\npressure_msl_pa=%ld\n", (long)alt, (long)msl);
                }
                int32_t ah = sensor_calc_abs_humidity_c100(r.temperature_c100, (int32_t)r.humidity_cpercent);
                int32_t dp = sensor_calc_dew_point_c100(r.temperature_c100, (int32_t)r.humidity_cpercent);
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "abs_humidity_c100=%ld\ndew_point_c100=%ld\n", (long)ah, (long)dp);
                int32_t iaq = 0;
                if (sensor_hub_get(SENSOR_CHAN_IAQ, &iaq, NULL)) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "iaq=%ld\n", (long)iaq);
                }
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)bme680_read_count(),
                (unsigned long)bme680_fail_count());
            const char *lf = bme680_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (bme280_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                bme280_part_name(), bme280_address(),
                (unsigned long)bme280_sample_period_s());

            bme280_reading_t r;
            uint32_t age_s = 0;
            if (!bme280_cached(&r, &age_s)) {
                /* Detected but never read: distinguishable from a stale reading,
                 * and from no part at all, which are three different situations
                 * with three different fixes. */
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\ntemperature_c100=%ld\npressure_pa=%lu\n",
                    (unsigned long)age_s, (long)r.temperature_c100,
                    (unsigned long)(r.pressure_pa256 >> 8));
                /* reduced to sea level, where the node knows
                 * its altitude -- so a gateway republishing this file
                 * (`mqttd file`) can publish what weather services quote. */
                int32_t alt;
                if (bme280_altitude(&alt))
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "altitude_m=%ld\npressure_msl_pa=%ld\n", (long)alt,
                        (long)bme280_sea_level_pa((int32_t)(r.pressure_pa256 >> 8),
                                                  r.temperature_c100, alt));
                if (r.have_humidity)
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "humidity_rh1000=%lu\n",
                        (unsigned long)((r.humidity_rh1024 * 1000u) >> 10));
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)bme280_read_count(),
                (unsigned long)bme280_fail_count());
            const char *lf = bme280_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (tsl2591_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                tsl2591_part_name(), tsl2591_address(),
                (unsigned long)sensor_hub_sample_period_s());

            tsl2591_reading_t tr;
            uint32_t t_age = 0;
            if (!tsl2591_cached(&tr, &t_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\nlux_c100=%ld\n",
                    (unsigned long)t_age, (long)tr.lux_c100);
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)tsl2591_read_count(),
                (unsigned long)tsl2591_fail_count());
            const char *lf = tsl2591_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (tsl2561_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                tsl2561_part_name(), tsl2561_address(),
                (unsigned long)sensor_hub_sample_period_s());

            tsl2561_reading_t tr;
            uint32_t t_age = 0;
            if (!tsl2561_cached(&tr, &t_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\nlux_c100=%ld\n",
                    (unsigned long)t_age, (long)tr.lux_c100);
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)tsl2561_read_count(),
                (unsigned long)tsl2561_fail_count());
            const char *lf = tsl2561_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (ccs811_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                ccs811_part_name(), ccs811_address(),
                (unsigned long)sensor_hub_sample_period_s());

            ccs811_reading_t cr;
            uint32_t c_age = 0;
            if (!ccs811_cached(&cr, &c_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\neco2_ppm=%u\ntvoc_ppb=%u\n",
                    (unsigned long)c_age, (unsigned int)cr.eco2_ppm, (unsigned int)cr.tvoc_ppb);
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)ccs811_read_count(),
                (unsigned long)ccs811_fail_count());
            const char *lf = ccs811_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (sgp30_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                sgp30_part_name(), sgp30_address(),
                (unsigned long)sensor_hub_sample_period_s());

            sgp30_reading_t sr;
            uint32_t s_age = 0;
            if (!sgp30_cached(&sr, &s_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\neco2_ppm=%u\ntvoc_ppb=%u\n",
                    (unsigned long)s_age, (unsigned int)sr.eco2_ppm, (unsigned int)sr.tvoc_ppb);
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)sgp30_read_count(),
                (unsigned long)sgp30_fail_count());
            const char *lf = sgp30_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (mics6814_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                mics6814_part_name(), mics6814_address(),
                (unsigned long)sensor_hub_sample_period_s());

            mics6814_reading_t mr;
            uint32_t m_age = 0;
            if (!mics6814_cached(&mr, &m_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                uint32_t rem = 0;
                bool warming = mics6814_is_warming_up(&rem);
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\nco_c_ppm=%ld\nno2_c_ppm=%ld\nnh3_c_ppm=%ld\nwarming_up=%s\n",
                    (unsigned long)m_age, (long)mr.co_c_ppm, (long)mr.no2_c_ppm, (long)mr.nh3_c_ppm,
                    warming ? "yes" : "no");
                if (warming) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "warmup_remaining_s=%lu\n", (unsigned long)rem);
                }
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)mics6814_read_count(),
                (unsigned long)mics6814_fail_count());
            const char *lf = mics6814_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (mhz19b_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\nsample_period_s=%lu\n",
                mhz19b_part_name(),
                (unsigned long)sensor_hub_sample_period_s());

            mhz19b_reading_t mr;
            uint32_t m_age = 0;
            if (!mhz19b_cached(&mr, &m_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                uint32_t rem = 0;
                bool warming = mhz19b_is_warming_up(&rem);
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\nco2_ppm=%ld\nchamber_temp_c100=%ld\nwarming_up=%s\n",
                    (unsigned long)m_age, (long)mr.co2_ppm, (long)mr.temp_c100,
                    warming ? "yes" : "no");
                if (warming) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "warmup_remaining_s=%lu\n", (unsigned long)rem);
                }
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)mhz19b_read_count(),
                (unsigned long)mhz19b_fail_count());
            const char *lf = mhz19b_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else if (hdc1080_is_detected()) {
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "part=%s\naddr=0x%02x\nsample_period_s=%lu\n",
                hdc1080_part_name(), hdc1080_address(),
                (unsigned long)sensor_hub_sample_period_s());

            hdc1080_reading_t hr;
            uint32_t h_age = 0;
            if (!hdc1080_cached(&hr, &h_age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "valid=no\n");
            } else {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "valid=yes\nage_s=%lu\ntemperature_c100=%ld\nhumidity_rh1000=%lu\n",
                    (unsigned long)h_age, (long)hr.temp_c100,
                    (unsigned long)hr.humidity_rh1000);
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "reads=%lu\nfailures=%lu\n",
                (unsigned long)hdc1080_read_count(),
                (unsigned long)hdc1080_fail_count());
            const char *lf = hdc1080_last_failure();
            if (lf)
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "last_failure=%s\n", lf);
        } else {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "part=none\n");
            return (int)used;
        }

        if (bme680_is_detected() || bme280_is_detected() || hdc1080_is_detected()) {
            if (hdc1080_is_detected() && (bme680_is_detected() || bme280_is_detected())) {
                hdc1080_reading_t hr;
                uint32_t h_age = 0;
                if (hdc1080_cached(&hr, &h_age)) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "hdc1080_temperature_c100=%ld\nhdc1080_humidity_rh1000=%lu\nhdc1080_age_s=%lu\n",
                        (long)hr.temp_c100, (unsigned long)hr.humidity_rh1000,
                        (unsigned long)h_age);
                }
            }
            if (tsl2591_is_detected()) {
                tsl2591_reading_t tr;
                uint32_t t_age = 0;
                if (tsl2591_cached(&tr, &t_age)) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "lux_c100=%ld\nlux_age_s=%lu\n",
                        (long)tr.lux_c100, (unsigned long)t_age);
                }
            } else if (tsl2561_is_detected()) {
                tsl2561_reading_t tr;
                uint32_t t_age = 0;
                if (tsl2561_cached(&tr, &t_age)) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "lux_c100=%ld\nlux_age_s=%lu\n",
                        (long)tr.lux_c100, (unsigned long)t_age);
                }
            }

            if (ccs811_is_detected()) {
                ccs811_reading_t cr;
                uint32_t c_age = 0;
                if (ccs811_cached(&cr, &c_age)) {
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "eco2_ppm=%u\ntvoc_ppb=%u\ngas_age_s=%lu\n",
                        (unsigned int)cr.eco2_ppm, (unsigned int)cr.tvoc_ppb, (unsigned long)c_age);
                }
            }
            if (sgp30_is_detected()) {
                sgp30_reading_t sr;
                uint32_t s_age = 0;
                if (sgp30_cached(&sr, &s_age)) {
                    if (!ccs811_is_detected()) {
                        used += (uint32_t)ksnprintf(buf + used, cap - used,
                            "eco2_ppm=%u\ntvoc_ppb=%u\ngas_age_s=%lu\n",
                            (unsigned int)sr.eco2_ppm, (unsigned int)sr.tvoc_ppb, (unsigned long)s_age);
                    } else {
                        used += (uint32_t)ksnprintf(buf + used, cap - used,
                            "sgp30_eco2_ppm=%u\nsgp30_tvoc_ppb=%u\nsgp30_age_s=%lu\n",
                            (unsigned int)sr.eco2_ppm, (unsigned int)sr.tvoc_ppb, (unsigned long)s_age);
                    }
                }
            }

            if (mics6814_is_detected()) {
                mics6814_reading_t mr;
                uint32_t m_age = 0;
                if (mics6814_cached(&mr, &m_age)) {
                    uint32_t rem = 0;
                    bool warming = mics6814_is_warming_up(&rem);
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "co_c_ppm=%ld\nno2_c_ppm=%ld\nnh3_c_ppm=%ld\nmics6814_age_s=%lu\nmics6814_warming_up=%s\n",
                        (long)mr.co_c_ppm, (long)mr.no2_c_ppm, (long)mr.nh3_c_ppm,
                        (unsigned long)m_age, warming ? "yes" : "no");
                    if (warming) {
                        used += (uint32_t)ksnprintf(buf + used, cap - used,
                            "mics6814_warmup_remaining_s=%lu\n", (unsigned long)rem);
                    }
                }
            }

            if (mhz19b_is_detected()) {
                mhz19b_reading_t mr;
                uint32_t m_age = 0;
                if (mhz19b_cached(&mr, &m_age)) {
                    uint32_t rem = 0;
                    bool warming = mhz19b_is_warming_up(&rem);
                    used += (uint32_t)ksnprintf(buf + used, cap - used,
                        "co2_ppm=%ld\nmhz19b_chamber_temp_c100=%ld\nmhz19b_age_s=%lu\nmhz19b_warming_up=%s\n",
                        (long)mr.co2_ppm, (long)mr.temp_c100,
                        (unsigned long)m_age, warming ? "yes" : "no");
                    if (warming) {
                        used += (uint32_t)ksnprintf(buf + used, cap - used,
                            "mhz19b_warmup_remaining_s=%lu\n", (unsigned long)rem);
                    }
                }
            }

            if (mhz19b_is_detected() && (ccs811_is_detected() || sgp30_is_detected())) {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "mox_contaminated=%s\nco2_ratio_pct=%ld\nfresh_air_verified=%s\n",
                    sensor_hub_is_mox_contaminated() ? "yes" : "no",
                    (long)sensor_hub_co2_ratio_pct(),
                    sensor_hub_is_fresh_air_verified() ? "yes" : "no");
            }
        }
        return (int)used;
    }
    else if (strncmp(rel, "sensor/", 7) == 0) {
        const char *sub = rel + 7;
        uint32_t used = 0;
        if (strcmp(sub, "fused") == 0) {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "tier=fused\n");
            int32_t val = 0;
            uint32_t age = 0;
            if (sensor_hub_get(SENSOR_CHAN_CO2, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "co2_ppm=%ld\nco2_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_TEMP, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "temperature_c100=%ld\ntemperature_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_PRESSURE, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "pressure_pa=%ld\npressure_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_HUMIDITY, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "humidity_rh1000=%ld\nhumidity_age_s=%lu\n", (long)(val * 10), (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_LUX, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "lux_c100=%ld\nlux_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_ECO2, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "eco2_ppm=%ld\neco2_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_TVOC, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "tvoc_ppb=%ld\ntvoc_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_GAS_RES, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "gas_resistance_ohm=%ld\ngas_resistance_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_CO, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "co_c_ppm=%ld\nco_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_NO2, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "no2_c_ppm=%ld\nno2_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_NH3, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "nh3_c_ppm=%ld\nnh3_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            return (int)used;
        } else if (strcmp(sub, "inferred") == 0) {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "tier=inferred\n");
            int32_t val = 0;
            uint32_t age = 0;
            int32_t alt = 0;
            if (node_altitude(&alt)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "altitude_m=%ld\n", (long)alt);
            }
            if (sensor_hub_get(SENSOR_CHAN_PRESSURE_MSL, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "pressure_msl_pa=%ld\npressure_msl_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_DEW_POINT, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "dew_point_c100=%ld\ndew_point_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_AH, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "abs_humidity_c100=%ld\nabs_humidity_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (sensor_hub_get(SENSOR_CHAN_IAQ, &val, &age)) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "iaq=%ld\niaq_age_s=%lu\n", (long)val, (unsigned long)age);
            }
            if (mhz19b_is_detected() && (ccs811_is_detected() || sgp30_is_detected())) {
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "mox_contaminated=%s\nco2_ratio_pct=%ld\nfresh_air_verified=%s\n",
                    sensor_hub_is_mox_contaminated() ? "yes" : "no",
                    (long)sensor_hub_co2_ratio_pct(),
                    sensor_hub_is_fresh_air_verified() ? "yes" : "no");
            }
            return (int)used;
        } else {
            /* Look for matching sensor device */
            sensor_dev_t *dev = NULL;
            for (uint32_t i = 0; i < sensor_hub_device_count(); i++) {
                sensor_dev_t *d = sensor_hub_device_get(i);
                if (d && d->name && strcmp(d->name, sub) == 0) {
                    dev = d;
                    break;
                }
            }
            if (!dev) return -1;

            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "tier=hw\npart=%s\nbus=%u\naddr=0x%02x\nsample_period_s=%lu\n",
                dev->name, (unsigned)dev->bus, dev->addr, (unsigned long)sensor_hub_sample_period_s());
            if (dev->period_ms) {
                used += (uint32_t)ksnprintf(buf + used, cap - used, "sample_period_ms=%lu\n",
                                            (unsigned long)dev->period_ms);
            }

            bool any_val = false;
            uint32_t min_age = 0;
            for (uint32_t ch = 0; ch < SENSOR_CHAN_MAX; ch++) {
                if (!(dev->chan_mask & (1u << ch))) continue;
                int32_t val = 0;
                uint32_t age = 0;
                if (sensor_hub_get_dev(dev, (sensor_chan_t)ch, &val, &age)) {
                    if (!any_val || age < min_age) min_age = age;
                    any_val = true;
                    switch ((sensor_chan_t)ch) {
                        case SENSOR_CHAN_TEMP:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "temperature_c100=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_PRESSURE:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "pressure_pa=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_HUMIDITY:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "humidity_rh1000=%ld\n", (long)(val * 10));
                            break;
                        case SENSOR_CHAN_LUX:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "lux_c100=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_ECO2:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "eco2_ppm=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_TVOC:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "tvoc_ppb=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_GAS_RES:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "gas_resistance_ohm=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_CO:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "co_c_ppm=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_NO2:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "no2_c_ppm=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_NH3:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "nh3_c_ppm=%ld\n", (long)val);
                            break;
                        case SENSOR_CHAN_CO2:
                            used += (uint32_t)ksnprintf(buf + used, cap - used, "co2_ppm=%ld\n", (long)val);
                            break;
                        default: {
                            const sensor_chan_desc_t *d = sensor_chan_desc((sensor_chan_t)ch);
                            if (d && d->raw_key) {
                                used += (uint32_t)ksnprintf(buf + used, cap - used, "%s=%ld\n", d->raw_key, (long)val);
                            }
                            break;
                        }
                    }
                }
            }
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "valid=%s\nage_s=%lu\n", any_val ? "yes" : "no", (unsigned long)min_age);
            return (int)used;
        }
    }
    else if (strcmp(rel, "clock") == 0) {
        /* The discipline loop's own account of itself (P5). Everywhere else
         * this tree reports what a device did; this reports what the clock
         * believes about itself, which is the only way to tell a clock that is
         * right from one that is merely confident. */
        disc_status_t d;
        discipline_status(&d);
        static const char *const NAMES[] = { "unset", "step", "track", "holdover" };
        uint32_t used = 0;
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "state=%s\nsamples=%lu\naccepted=%lu\nrejected=%lu\n",
            NAMES[(unsigned)d.state <= 3u ? (unsigned)d.state : 0u],
            (unsigned long)d.samples, (unsigned long)d.accepted,
            (unsigned long)d.rejected);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "freq_ppb=%ld\nslewing=%s\nlast_offset_us=%ld\n",
            (long)d.freq_ppb, time_slewing() ? "yes" : "no",
            (long)d.last_offset_us);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "mean_offset_us=%ld\nsd_offset_us=%lu\nage_s=%lu\n"
            "dispersion_us=%lu\ndelay_const_us=%d\n",
            (long)d.mean_offset_us, (unsigned long)d.sd_offset_us,
            (unsigned long)d.age_s, (unsigned long)d.dispersion_us,
            (int)CONFIG_DCF77_DELAY_US);
        /* freq_updates against the loop's time constant, so "still walking in"
         * can be told from "settled on the wrong value" -- the two need
         * opposite fixes and looked identical during the first holdover. */
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "freq_updates=%lu\n", (unsigned long)d.freq_updates);
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
        /* The DS3231's thermometer, cached: a crystal's rate is a function of
         * temperature, and the first holdover drifted 672 -> 485 ppb across a
         * warming morning without anything recording how warm. The age is
         * reported beside it because correlating a rate against a stale
         * reading is how a spurious tempco gets published. */
        {
            int tc = 0; uint32_t tage = 0;
            if (i2c_rtc_cached_temperature_c(&tc, &tage))
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "temp_c=%d\ntemp_age_s=%lu\n", tc, (unsigned long)tage);
            else
                used += (uint32_t)ksnprintf(buf + used, cap - used,
                    "temp_c=none\n");
        }
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
        /* The independent check: this clock against the satellite pulse,
         * once a second. Reported here beside what the loop believes about
         * itself, because the interesting number is the difference between
         * the two -- a clock whose dispersion says 5 ms while its true error
         * is 40 ms is not disciplined, it is confident. */
        gps_clock_err_t ce;
        gps_clock_error(&ce);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "gps_err_n=%lu\ngps_err_last_us=%ld\ngps_err_mean_us=%ld\n"
            "gps_err_sd_us=%lu\ngps_err_min_us=%ld\ngps_err_max_us=%ld\n",
            (unsigned long)ce.n, (long)ce.last_us, (long)ce.mean_us,
            (unsigned long)ce.sd_us, (long)ce.min_us, (long)ce.max_us);
#endif
        return (int)used;
    }
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
    else if (strcmp(rel, "gps") == 0) {
        /* P3b's window on the transfer standard, and the reason it is a file
         * rather than a shell command: the board doing this measurement lives
         * wherever its reception is, and its console does not. */
        gps_status_t gs;
        gps_status(&gs);
        uint32_t used = 0;

        /* A one-line verdict, first, because the counters below need a page of
         * context to interpret and this file is read on a windowsill. Each
         * state names the *next thing to do*, which is the only question a
         * person standing there actually has.
         *
         * "tracking" is the one worth spelling out: correct UTC alongside a
         * void RMC looks like a contradiction and is not. A module cannot
         * invent the time -- to emit it at all it must have decoded the
         * navigation message off a satellite it is tracking, so time arriving
         * before a fix is proof the antenna works. GGA's satellite count is
         * satellites *used in the fix*, so it reads 0 throughout. What is
         * missing is ephemeris, and that is only ever cured by waiting.
         *
         * "fix-no-pulse" is the one that matters most: gps_pps.h calls a fix
         * without a pulse a wiring fault, and without this line it would show
         * up as nothing more than a counter that stayed at zero. */
        const char *state, *hint;
        if (!gs.enabled)               { state = "disabled";     hint = "not built or not initialised"; }
        else if (!gs.rx_muxed)         { state = "mux-refused";  hint = "the RX pin never took its UART function"; }
        else if (gs.bytes == 0)        { state = "silent";       hint = "no bytes at all -- power or TX->RX wiring"; }
        else if (gs.sentences == 0)    { state = "garbage";      hint = "bytes but no valid sentence -- check baud"; }
        else if (!gs.have_utc)         { state = "searching";    hint = "sentences parse, no satellite decoded yet"; }
        else if (!gs.rmc_valid || gs.fix_quality == 0)
                                       { state = "tracking";     hint = "time decoded, no fix yet -- antenna works, wait for ephemeris"; }
        else if (gs.pps_stormed)       { state = "pps-noisy";    hint = "PPS shut off for oscillating; retrying -- ~2 kHz means the module is not locked yet, tens of kHz means a floating pin"; }
        else if (gs.pps_count == 0)    { state = "fix-no-pulse"; hint = "fix but no PPS edge -- check the PPS wire and CONFIG_GPS_PPS_ACTIVE_LOW"; }
        else if (!gps_pps_trustworthy()) { state = "unsteady";   hint = "fix and pulses, but the interval is not ~1 s"; }
        else                           { state = "locked";       hint = "fix and a 1 s pulse -- calibration can run"; }
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "state=%s\nstate_hint=%s\n", state, hint);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "enabled=%s\nrx_muxed=%s\nrx_ctrl=0x%08lx\n"
            "rx_bytes=%lu\nsentences=%lu\nbad_checksum=%lu\n",
            gs.enabled ? "yes" : "no", gs.rx_muxed ? "yes" : "no",
            (unsigned long)gs.rx_pad, (unsigned long)gs.bytes,
            (unsigned long)gs.sentences, (unsigned long)gs.bad_checksum);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "rx_idle_ms=%lu\nrx_fr=0x%08lx\nrx_cr=0x%08lx\n"
            "rx_err_overrun=%lu\nrx_err_break=%lu\n"
            "rx_err_parity=%lu\nrx_err_frame=%lu\n",
            (unsigned long)gs.rx_idle_ms, (unsigned long)gs.rx_fr,
            (unsigned long)gs.rx_cr, (unsigned long)gs.rx_err_overrun,
            (unsigned long)gs.rx_err_break, (unsigned long)gs.rx_err_parity,
            (unsigned long)gs.rx_err_frame);
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "fix_quality=%u\nsatellites=%u\nrmc=%s\n",
            (unsigned)gs.fix_quality, (unsigned)gs.satellites,
            gs.rmc_valid ? "valid" : "void");
        if (gs.have_utc) {
            char iso[32];
            time_format_iso(&gs.utc, iso, sizeof(iso));
            used += (uint32_t)ksnprintf(buf + used, cap - used, "utc=%s%s\n", iso,
                gs.rmc_valid ? "" : " (unvalidated: RMC says void)");
        } else {
            used += (uint32_t)ksnprintf(buf + used, cap - used, "utc=none\n");
        }
        used += (uint32_t)ksnprintf(buf + used, cap - used,
            "pps_count=%lu\npps_interval_us=%lu\npps_dropped=%lu\npps_stormed=%s\npps_storms=%lu\npps_storm_rate=%lu\n"
            "trustworthy=%s\n",
            (unsigned long)gs.pps_count, (unsigned long)gs.pps_interval_us,
            (unsigned long)gs.pps_dropped, gs.pps_stormed ? "yes" : "no",
            (unsigned long)gs.pps_storms, (unsigned long)gs.pps_storm_rate,
            gps_pps_trustworthy() ? "yes" : "no");
        /* What the module says about itself. The sentence set identifies its
         * family, which is what says whether a timepulse reconfiguration
         * should be spoken as UBX or as PMTK. */
        {
            char types[80];
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "ubx_tp5_sent=%lu\nubx_saves=%lu\n"
                "ubx_frames=%lu\nubx_acks=%lu\nubx_naks=%lu\n"
                "ubx_ack_for=%02x:%02x\n"
                "tp5_reads=%lu\ntp5_idx=%u\ntp5_freq=%lu\n"
                "tp5_freq_lock=%lu\ntp5_flags=0x%08lx\n",
                (unsigned long)gs.ubx_tp5_sent, (unsigned long)gs.ubx_saves,
                (unsigned long)gs.ubx_frames, (unsigned long)gs.ubx_acks,
                (unsigned long)gs.ubx_naks,
                (unsigned)gs.ubx_ack_cls, (unsigned)gs.ubx_ack_id,
                (unsigned long)gs.tp5_reads, (unsigned)gs.tp5_idx,
                (unsigned long)gs.tp5_freq, (unsigned long)gs.tp5_freq_lock,
                (unsigned long)gs.tp5_flags);
            gps_nmea_types(types, sizeof(types));
            used += (uint32_t)ksnprintf(buf + used, cap - used,
                "nmea_types=%s\nnmea_last=%s\n", types, gps_nmea_last());
        }
        return (int)used;
    }
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
#if CONFIG_DCF77_P0_LOG
    else if (strcmp(rel, "dcf77log") == 0) {
        /* P0's raw output.
         * The two accumulated results first, so they are correct however
         * little of the series a collector actually saw, then the recent
         * samples -- which is the split that makes an appliance at the far
         * end of a radio measurable at all. */
        return (int)dcf77_p0log_render(buf, cap);
    }
#endif
    else if (strcmp(rel, "dcf77") == 0) {
        /* The radio's health, without a serial console (D4,
         * ). The clock persona's only other
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
            "quality_mean_tenths=%u\nquality_samples=%u\nlongest_clean_run_s=%u\n"
            /* P3: whether the completed frame's instant came from an
             * interrupt-timestamped edge or from the millisecond sample that
             * noticed it, and how many edges the ring could not hold. A
             * non-zero drop count on a quiet line means the ring is too small;
             * on a noisy one it is the noise itself, and the quality figures
             * above say which. */
            "out_level=%u\npon_asserted=%s\n"
            "mark_from_edge=%s\nedges=%lu\nedges_dropped=%lu\n"
            "pps_n=%lu\npps_last_us=%ld\npps_mean_us=%ld\n"
            "pps_sd_us=%lu\npps_min_us=%ld\npps_max_us=%ld\n"
            "gerr_n=%lu\ngerr_secbad=%lu\ngerr_last_us=%ld\n"
            "gerr_mean_us=%ld\ngerr_sd_us=%lu\n",
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
            (unsigned)st.decoder.clean_run_max,
            (unsigned)(dcf77_raw_level() ? 1u : 0u),
            dcf77_pon_asserted() ? "yes" : "no",
            st.radio_at_us_ok ? "yes" : "no",
            (unsigned long)st.edges_total, (unsigned long)st.edges_dropped,
            (unsigned long)st.pps_n, (long)st.pps_last_us,
            (long)st.pps_mean_us, (unsigned long)st.pps_sd_us,
            (long)st.pps_min_us, (long)st.pps_max_us,
            (unsigned long)st.gerr_n, (unsigned long)st.gerr_secbad,
            (long)st.gerr_last_us, (long)st.gerr_mean_us,
            (unsigned long)st.gerr_sd_us);

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

/* Says so when the answer did not fit, instead of handing back a plausible
 * prefix.
 *
 * proc_buf is one page and several of these files are generated from tables
 * that grow. Every producer above stops cleanly at `cap`, which is safe and
 * completely silent -- the reader gets a table that simply ends, with no way
 * to tell a short system from a clipped one.
 *
 * Stamped over the tail rather than appended, because by definition there is
 * no room left to append into. */
static int vfs_generate_proc_content(const char *rel, char *buf, uint32_t cap) {
    int n = vfs_generate_proc_content_raw(rel, buf, cap);
    if (n < 0) return n;

    static const char mark[] = "\n-- truncated --\n";
    const uint32_t mlen = (uint32_t)sizeof(mark) - 1;
    if ((uint32_t)n >= cap - 1 && cap > mlen + 1) {
        uint32_t at = cap - 1 - mlen;
        for (uint32_t i = 0; i < mlen; i++) buf[at + i] = mark[i];
        buf[cap - 1] = '\0';
        n = (int)(cap - 1);
    }
    return n;
}

/* Unsized on purpose: /proc/dcf77 exists only where a receiver does, and the
 * one caller that walks this list already derives the count with sizeof. */
static const char *g_proc_names[] = { "ps", "meminfo", "version", "cpuinfo", "df", "kmsg", "devices", "buildid", "path", "ports", "config", "net", "node", "clock", "sensors", "sensor",
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)
    "usbhost",
#endif
#if defined(CONFIG_LLP)
    "llp",
#endif
#if defined(CONFIG_BOARD_ESP32C6)
    "radio", "wlanscan", "wdt",
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_GPS
    "gps",
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    "dcf77",
#if CONFIG_DCF77_P0_LOG
    "dcf77log",
#endif
#endif
};
/* 37.5a: `clipboard` is the machine's clipboard (kernel/clipboard.h), as a
 * file -- Plan 9's /dev/snarf -- so cat, Lisp's file functions and any 9P
 * client read and write it with nothing new to learn. */
static const char *g_dev_names[]  = { "uart", "null", "zero", "eeprom", "clipboard",
#if defined(CONFIG_BOARD_ESP32C6)
    /* the radio's ctl file (kernel/radio_c6.c): how an LLP host hands this
     * node the network to join, as /c6/dev/wlan */
    "wlan",
#endif
#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32C6)
    /* `reboot` or `download` written restarts the board (board_reset_later()) */
    "reboot",
#endif
#if defined(CONFIG_OTA)
    /* updating this node's image (kernel/ota.h): the ctl file and the image */
    "ota", "otaimage",
#endif
#if defined(CONFIG_BATT_CELLS)
    /* the board battery's state (kernel/power.h) */
    "power",
#endif
};
#define DEV_COUNT ((int)(sizeof(g_dev_names) / sizeof(g_dev_names[0])))

/* Opens `path` into a fresh handle, returning a small non-negative fd (index
 * into g_handles[]) on success or -1 on failure. /srv/ (message-oriented
 * IPC channels) is deliberately not handle-addressable -- see the comment
 * on this API in fs/include/fs/vfs.h. */
/* Fills an already-reserved handle. Split out of vfs_open() so that the
 * slot can be marked in_use *before* any of this runs -- see vfs_open(). */
static int vfs_open_into_on(mount_entry_t *m, const char *rel, bool is_root, vfs_handle_t *h, const char *path, int flags) {
    (void)path;

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
        strncpy(h->rel_path, rel, sizeof(h->rel_path) - 1);
        h->rel_path[sizeof(h->rel_path) - 1] = '\0';
        size_t rlen = strlen(h->rel_path);
        if (rlen > 0 && h->rel_path[rlen - 1] == '/') {
            h->rel_path[rlen - 1] = '\0';
        }

        if (h->rel_path[0] == '\0' || strcmp(h->rel_path, "sensor") == 0) {
            h->is_dir = true;
        } else if (strcmp(h->rel_path, "kmsg") == 0) {
            h->is_kmsg = true;
            h->kmsg_start = klog_oldest();
            h->proc_len = (uint32_t)(klog_total() - h->kmsg_start);
        } else {
            h->proc_buf = palloc_pages_bulk(1);
            if (!h->proc_buf) h->proc_buf = palloc_pages(1);
            if (!h->proc_buf) return -1;
            int len = vfs_generate_proc_content(h->rel_path, h->proc_buf, PAGE_SIZE);
            if (len < 0) {
                palloc_free(h->proc_buf, 1);
                h->proc_buf = NULL;
                return -1;
            }
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
            for (int i = 0; i < DEV_COUNT; i++) {
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

static int vfs_open_into(vfs_handle_t *h, const char *path, int flags) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_open_into_on(m, rel, is_root, h, path, flags);
    if (r >= 0) h->mnt = m;           /* the open handle keeps it */
    else mount_put(m);
    return r;
}

int vfs_open(const char *path, int flags) {
    if (!path) return -1;
    if ((flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC)) && privileged_path_refused(path)) return -1;

    /* Find and take the slot as one step: two tasks opening at once would
     * otherwise both find the same free one. */
    int slot = -1;
    bool waited = false;
    uint64_t deadline = 0;
    for (;;) {
        bool late = waited && time_get_ms() >= deadline;
        unsigned used = 0;
        uintptr_t f = spin_lock_irqsave(&g_handles_lock);
        for (int i = 0; i < VFS_MAX_HANDLES; i++) {
            if (!g_handles[i].in_use) { if (slot < 0) slot = i; }
            else used++;
        }
        if (slot >= 0) {
            memset(&g_handles[slot], 0, sizeof(g_handles[slot]));
            g_handles[slot].in_use = true;
            if (used + 1u > g_handles_peak) g_handles_peak = used + 1u;
        } else if (!waited) {
            g_handles_waits++;
        } else if (late) {
            g_handles_none++;
        }
        spin_unlock_irqrestore(&g_handles_lock, f);
        if (slot >= 0) break;
        /* Every handle is open: wait for one to be closed, as a busy endpoint
         * is waited for, where waiting is possible. */
        if (late || !sched_active() || devirq_in_handler()) return -1;
        if (!waited) { waited = true; deadline = time_get_ms() + VFS_OPEN_WAIT_MS; }
        task_sleep_ms(1u);
    }

    vfs_handle_t *h = &g_handles[slot];

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
     * shared-state bugs like this one become reachable. (Taken above, under
     * g_handles_lock.) */

    if (vfs_open_into(h, path, flags) < 0) {
        uintptr_t r = spin_lock_irqsave(&g_handles_lock);
        memset(h, 0, sizeof(*h)); /* releases the reservation */
        spin_unlock_irqrestore(&g_handles_lock, r);
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
            return fat32_read_at(h->fs, &h->entry, buf, count, offset, &h->cursor);
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
            } else if (strcmp(h->rel_path, "null") == 0 || strcmp(h->rel_path, "zero") == 0 ||
                       strcmp(h->rel_path, "reboot") == 0) {
                return 0;
#if defined(CONFIG_OTA)
            } else if (strcmp(h->rel_path, "otaimage") == 0) {
                return ota_image_read(buf, count, offset);
            } else if (strcmp(h->rel_path, "ota") == 0) {
                char st[512];
                uint32_t len = ota_ctl_read(st, sizeof st);
                if (offset >= len) return 0;
                uint32_t k = len - (uint32_t)offset;
                if (k > count) k = count;
                memcpy(buf, st + offset, k);
                return (int)k;
#endif
#if defined(CONFIG_BATT_CELLS)
            } else if (strcmp(h->rel_path, "power") == 0) {
                char st[256];
                uint32_t len = power_ctl_read(st, sizeof st);
                if (offset >= len) return 0;
                uint32_t k = len - (uint32_t)offset;
                if (k > count) k = count;
                memcpy(buf, st + offset, k);
                return (int)k;
#endif
            } else if (strcmp(h->rel_path, "clipboard") == 0) {
                uint32_t len;
                const char *d = clipboard_data(&len);
                if (!d || offset >= len) return 0;
                uint32_t k = len - (uint32_t)offset;
                if (k > count) k = count;
                memcpy(buf, d + offset, k);
                return (int)k;
#if defined(CONFIG_BOARD_ESP32C6)
            } else if (strcmp(h->rel_path, "wlan") == 0) {
                char line[256];
                uint32_t len = radio_c6_ctl_read(line, sizeof line);
                if (offset >= len) return 0;
                uint32_t k = len - (uint32_t)offset;
                if (k > count) k = count;
                memcpy(buf, line + offset, k);
                return (int)k;
#endif
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
            int n = fat32_write_at(h->fs, h->rel_path, buf, count, offset, &h->cursor);
            if (n > 0) fat32_find_file(h->fs, h->rel_path, &h->entry); // refresh cached size
            h->wrote = true;
            return n;
        }
        case MOUNT_DEV:
            if (strcmp(h->rel_path, "uart") == 0) {
                const char *str = (const char *)buf;
                for (uint32_t i = 0; i < count; i++) uart_putc(str[i]);
                uart_flush(); /* this write is its own message; nothing else here would */
                return (int)count;
            } else if (strcmp(h->rel_path, "eeprom") == 0) {
                return at24c32_write((uint16_t)offset, (const uint8_t *)buf, count);
            } else if (strcmp(h->rel_path, "null") == 0) {
                return (int)count;
            } else if (strcmp(h->rel_path, "clipboard") == 0) {
                return clipboard_write_at((const char *)buf, count, (uint32_t)offset);
#if defined(CONFIG_BOARD_ESP32C6)
            } else if (strcmp(h->rel_path, "wlan") == 0) {
                return radio_c6_ctl_write((const char *)buf, count);
#endif
#if defined(CONFIG_BATT_CELLS)
            } else if (strcmp(h->rel_path, "power") == 0) {
                return power_ctl_write((const char *)buf, count);
#endif
#if defined(CONFIG_OTA)
            } else if (strcmp(h->rel_path, "ota") == 0) {
                return ota_ctl_write((const char *)buf, count);
            } else if (strcmp(h->rel_path, "otaimage") == 0) {
                return ota_image_write(buf, count, offset);
#endif
#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32C6)
            } else if (strcmp(h->rel_path, "reboot") == 0) {
                /* 300 ms: time for this write's reply to leave, over any link. */
                const char *b = (const char *)buf;
                uint32_t k = count;
                while (k && (b[k - 1] == '\n' || b[k - 1] == '\r' || b[k - 1] == ' ')) k--;
                bool download = k == 8 && memcmp(b, "download", 8) == 0;
                if (!download && !(k == 6 && memcmp(b, "reboot", 6) == 0)) return -1;
                return board_reset_later(download, 300) ? (int)count : -1;
#endif
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
            if (h->rel_path[0] == '\0') {
                if (index >= sizeof(g_proc_names) / sizeof(g_proc_names[0])) return -1;
                if (name_out && name_max > 0) {
                    strncpy(name_out, g_proc_names[index], name_max - 1);
                    name_out[name_max - 1] = '\0';
                }
                if (stat_out) {
                    stat_out->size = 0;
                    stat_out->is_dir = (strcmp(g_proc_names[index], "sensor") == 0) ? 1 : 0;
                }
                return 0;
            } else if (strcmp(h->rel_path, "sensor") == 0) {
                if (index == 0) {
                    if (name_out && name_max > 0) {
                        strncpy(name_out, "fused", name_max - 1);
                        name_out[name_max - 1] = '\0';
                    }
                    if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
                    return 0;
                }
                if (index == 1) {
                    if (name_out && name_max > 0) {
                        strncpy(name_out, "inferred", name_max - 1);
                        name_out[name_max - 1] = '\0';
                    }
                    if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
                    return 0;
                }
                uint32_t dev_idx = index - 2;
                if (dev_idx < sensor_hub_device_count()) {
                    sensor_dev_t *dev = sensor_hub_device_get(dev_idx);
                    if (dev && dev->name) {
                        if (name_out && name_max > 0) {
                            strncpy(name_out, dev->name, name_max - 1);
                            name_out[name_max - 1] = '\0';
                        }
                        if (stat_out) { stat_out->size = 0; stat_out->is_dir = 0; }
                        return 0;
                    }
                }
                return -1;
            }
            return -1;
        }
        case MOUNT_DEV: {
            if (index >= (uint32_t)DEV_COUNT) return -1;
            if (name_out && name_max > 0) {
                strncpy(name_out, g_dev_names[index], name_max - 1);
                name_out[name_max - 1] = '\0';
            }
            if (stat_out) {
                uint32_t clen = 0;
                if (strcmp(g_dev_names[index], "clipboard") == 0) (void)clipboard_data(&clen);
                stat_out->size = clen;
                stat_out->is_dir = 0;
            }
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
    /* A written file's close is where FSInfo's free count goes back to disk
     * (fat32_sync()): one sector, so `df` after a reboot need not rescan. */
    if (h->kind == MOUNT_FAT32 && h->wrote) fat32_sync(h->fs);
    if (h->proc_buf) palloc_free(h->proc_buf, 1);
    mount_entry_t *mnt = h->mnt;
    uintptr_t f = spin_lock_irqsave(&g_handles_lock);   /* not taken mid-clear */
    memset(h, 0, sizeof(*h));
    spin_unlock_irqrestore(&g_handles_lock, f);
    mount_put(mnt);
    return 0;
}

/* --- Legacy whole-file API: thin compat wrappers over the handle API above,
 * except the /srv/ branch of vfs_read()/vfs_write(), which bypasses it
 * entirely (message-oriented IPC channels aren't handle-addressable).
 * Note this means vfs_read()/vfs_write()/vfs_cp()/vfs_append() all pick up
 * MOUNT_REMOTE9P support "for free" -- they were already generic over
 * whatever vfs_open()/vfs_pread()/vfs_pwrite() resolve to. --- */

static int vfs_read_on(mount_entry_t *m, const char *rel, bool is_root, const char *path, void *buf, uint32_t max_len) {
    (void)is_root;
    if (!path) return -1;


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

int vfs_read(const char *path, void *buf, uint32_t max_len) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_read_on(m, rel, is_root, path, buf, max_len);
    mount_put(m);
    return r;
}

static int vfs_write_on(mount_entry_t *m, const char *rel, bool is_root, const char *path, const void *buf, uint32_t len) {
    (void)is_root;
    if (!path) return -1;


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
            int n = chan_call_wait(ep, (const uint8_t *)buf, len, reply, sizeof(reply), CHAN_WAIT_MS);
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

int vfs_write(const char *path, const void *buf, uint32_t len) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_write_on(m, rel, is_root, path, buf, len);
    mount_put(m);
    return r;
}

static int vfs_append_on(mount_entry_t *m, const char *rel, bool is_root, const char *path, const void *buf, uint32_t len) {
    (void)rel; (void)is_root;
    if (!path) return -1;
    if (!buf || len == 0) return 0; // matches fat32_append_file()'s own no-op guard

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

int vfs_append(const char *path, const void *buf, uint32_t len) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_append_on(m, rel, is_root, path, buf, len);
    mount_put(m);
    return r;
}

/* Explicit volume initialization, replacing the auto-format-on-invalid-boot-
 * sector behavior fat32_init() used to have (see B10 in
 * plan/completed/2026-08-07_review_and_remediation.md) -- a corrupt or blank card
 * inserted at /sd0/ now just fails to mount instead of being silently
 * wiped; this is the only way (aside from vfs_mount_ramdisk()'s own
 * deliberate fallback for the always-starts-blank RAM disk) a volume gets
 * formatted now. Remote mounts can't be formatted (nonsensical -- it isn't
 * this node's storage to reinitialize). */
/* `df -r` (38.0): recount the writable FAT volumes' free space from the FAT
 * itself and store it in FSInfo, saying what the stored count had been. */
void vfs_fat_recount(void) {
    struct { const char *name; fat32_fs_t *fs; bool mounted; } v[] = {
        { "/sd0/",  &g_fat32_sd,  g_sd_mounted },
        { "/ram0/", &g_fat32_ram, g_ram_mounted },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        if (!v[i].mounted) continue;
        uint64_t t0 = time_get_ms();
        uint32_t before = fat32_recount(v[i].fs);
        uint32_t after = v[i].fs->free_count;
        if (before == FAT32_FREE_UNKNOWN) {
            cprintf("%-7s %lu free clusters (none stored), %lu ms\n", v[i].name,
                    (unsigned long)after, (unsigned long)(time_get_ms() - t0));
        } else {
            cprintf("%-7s %lu free clusters, stored count was %lu (%s), %lu ms\n", v[i].name,
                    (unsigned long)after, (unsigned long)before, before == after ? "right" : "STALE",
                    (unsigned long)(time_get_ms() - t0));
        }
    }
}

/* `fsck`: fat32_check() on /sd0 or /ram0, its working memory borrowed from
 * the page allocator for the run (a 32 GB card with 32 KB clusters needs a
 * 116 KB bitmap). */
#define FSCK_DIRQ 1024u
void vfs_fsck(const char *path) {
    fat32_fs_t *fs = NULL;
    const char *name = "/sd0/";
    if (!path || !*path || strncmp(path, "/sd0", 4) == 0) {
        if (g_sd_mounted) fs = &g_fat32_sd;
    } else if (strncmp(path, "/ram0", 5) == 0) {
        name = "/ram0/";
        if (g_ram_mounted) fs = &g_fat32_ram;
    } else {
        cprintf("usage: fsck [/sd0|/ram0]\n");
        return;
    }
    if (!fs) { cprintf("fsck: %s is not mounted\n", name); return; }
    uint32_t bitmap_bytes = (fs->total_clusters + 2u + 7u) / 8u;
    uint32_t bytes = ((bitmap_bytes + 3u) & ~3u) + FSCK_DIRQ * 4u;
    uint32_t pages = (bytes + 4095u) / 4096u;
    uint8_t *mem = palloc_pages_bulk(pages);
    if (!mem) mem = palloc_pages(pages);
    if (!mem) { cprintf("fsck: no %lu KB for the cluster map\n", (unsigned long)(bytes / 1024u)); return; }
    uint32_t *dirq = (uint32_t *)(mem + ((bitmap_bytes + 3u) & ~3u));
    fat32_check_t r;
    uint64_t t0 = time_get_ms();
    int rc = fat32_check(fs, mem, bitmap_bytes, dirq, FSCK_DIRQ, &r);
    palloc_free(mem, pages);
    uint32_t problems = r.cross + r.bad_chain + r.size + r.lost;
    cprintf("fsck %s %lu files, %lu directories, %lu clusters in use; "
            "%lu cross-linked, %lu broken chains, %lu wrong sizes, %lu lost clusters%s; %lu ms\n",
            name, (unsigned long)r.files, (unsigned long)r.dirs, (unsigned long)r.clusters,
            (unsigned long)r.cross, (unsigned long)r.bad_chain, (unsigned long)r.size,
            (unsigned long)r.lost, r.skipped ? " (some directories not walked)" : "",
            (unsigned long)(time_get_ms() - t0));
    cprintf("%s\n", rc != 0 ? "fsck: INCOMPLETE (read error)" : problems ? "fsck: DAMAGED" : "fsck: clean");
}

static int vfs_format_volume(mount_entry_t *m, const char *path, const char *label);

static int vfs_format_on(mount_entry_t *m, const char *rel, bool is_root, const char *path,
                         const char *label) {
    (void)rel; (void)is_root;
    if (!path) return -1;
    /* Not under open files: fat32_init() starts the volume over, its lock
     * included. This call's own reference is the one allowed, and no new
     * one is handed out until it is done. */
    mount_entry_t *q = NULL;
    if (m && !mount_quiesce(m->name, 1, &q)) {
        printk("[VFS Error] format: '/%s/' is in use\n", m->name);
        return -1;
    }
    int rc = vfs_format_volume(m, path, label);
    if (q) mount_ready(q);
    return rc;
}

static void format_progress(uint32_t pct) { cprintf("format: %lu%%\r", (unsigned long)pct); }

static int vfs_format_volume(mount_entry_t *m, const char *path, const char *label) {
    if (m && strcmp(m->name, "sd0") == 0) {
#if defined(CONFIG_BOARD_RP2350)
        block_dev_t *dev = spisd_get_device();
#elif defined(CONFIG_BOARD_ESP32C6)
        block_dev_t *dev = NULL;   /* no block device (45.4) */
#elif defined(CONFIG_BOARD_K210)
        block_dev_t *dev = NULL;   /* no block device (65.3) */
#elif defined(CONFIG_BOARD_ESP32P4)
        block_dev_t *dev = sdmmc_get_device();   /* 35.2 */
#else
        block_dev_t *dev = virtio_blk_get_device();
#endif
        /* A card gets a card's layout (fat32_mkfs(): MBR, aligned FAT32,
         * FSInfo), not the bare volume /ram0 gets. */
        uint32_t vol_id = 0;
        random_bytes(&vol_id, sizeof vol_id);
        if (!dev) return -1;
        if (fat32_mkfs(dev, label, vol_id, format_progress) == 0) {
            cprintf("\n");
        } else if (dev->num_blocks >= 131072u || fat32_format(dev) != 0) {
            return -1;      /* too small for FAT32 proper (QEMU's image): the bare volume */
        }
        g_sd_mounted = (fat32_init(&g_fat32_sd, dev) == 0);
        return g_sd_mounted ? 0 : -1;
    } else if (m && strcmp(m->name, "ram0") == 0) {
        block_dev_t *dev = ramdisk_get_device();
        if (!dev || fat32_format(dev) != 0) return -1;
        g_ram_mounted = (fat32_init(&g_fat32_ram, dev) == 0);
        return g_ram_mounted ? 0 : -1;
    } else if (m && strcmp(m->name, "flash0") == 0 && m->read_only) {
        printk("[VFS Error] '/flash0/' (Embedded Flash ROMDisk) cannot be formatted (read-only)\n");
        return -1;
    }
    printk("[VFS Error] format: '%s' is not a formattable storage volume\n", path ? path : "");
    return -1;
}

int vfs_format(const char *path) { return vfs_format_label(path, NULL); }

int vfs_format_label(const char *path, const char *label) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_format_on(m, rel, is_root, path, label);
    mount_put(m);
    return r;
}

static int vfs_remove_on(mount_entry_t *m, const char *rel, bool is_root, const char *path) {
    (void)is_root;
    if (!path) return -1;
    if (!m || !rel || rel[0] == '\0') return -1;

    if (m->kind == MOUNT_FAT32) {
        if (m->read_only || !mount_is_active(m)) return -1;
        return fat32_remove_file(m->fs, rel);
    } else if (m->kind == MOUNT_REMOTE9P) {
        return p9_remote_remove(m->remote, rel);
    }
    return -1;
}

int vfs_remove(const char *path) {
    if (privileged_path_refused(path)) return -1;
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_remove_on(m, rel, is_root, path);
    mount_put(m);
    return r;
}

static int vfs_mkdir_on(mount_entry_t *m, const char *rel, bool is_root, const char *path) {
    (void)is_root;
    if (!path) return -1;
    if (!m || !rel || rel[0] == '\0') return -1;

    if (m->kind == MOUNT_FAT32) {
        if (m->read_only || !mount_is_active(m)) return -1;
        return fat32_mkdir(m->fs, rel);
    } else if (m->kind == MOUNT_REMOTE9P) {
        return p9_remote_mkdir(m->remote, rel);
    }
    return -1;
}

int vfs_mkdir(const char *path) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_mkdir_on(m, rel, is_root, path);
    mount_put(m);
    return r;
}

static int vfs_rmdir_on(mount_entry_t *m, const char *rel, bool is_root, const char *path) {
    (void)is_root;
    if (!path) return -1;
    if (!m || !rel || rel[0] == '\0') return -1;

    if (m->kind == MOUNT_FAT32) {
        if (m->read_only || !mount_is_active(m)) return -1;
        return fat32_rmdir(m->fs, rel);
    } else if (m->kind == MOUNT_REMOTE9P) {
        return p9_remote_remove(m->remote, rel); // server dispatches file vs. dir removal itself
    }
    return -1;
}

int vfs_rmdir(const char *path) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    int r = vfs_rmdir_on(m, rel, is_root, path);
    mount_put(m);
    return r;
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

    uint8_t copy_buf[512];   /* each copy its own: two terminals may copy at once */
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

static void vfs_ls_on(mount_entry_t *m, const char *rel, bool is_root, const char *path) {

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
            cprintf("\nDirectory Listing (/proc/%s%s):\n", rel[0] ? rel : "", rel[0] ? "/" : "");
            cprintf("Name        Type\n----------  ----\n");
            char open_path[64];
            if (rel[0] != '\0') {
                ksnprintf(open_path, sizeof(open_path), "/proc/%s", rel);
            } else {
                ksnprintf(open_path, sizeof(open_path), "/proc");
            }
            int fd = vfs_open(open_path, VFS_O_READ);
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
            cprintf("Name        Type\n----------  ----\nuart        char device\nnull        bit bucket\nzero        null generator\neeprom      i2c eeprom (4KB)\nclipboard   the clipboard (8KB)\n\n");
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

void vfs_ls(const char *path) {
    bool is_root;
    const char *rel = NULL;
    mount_entry_t *m = mount_get(path, &rel, &is_root);
    vfs_ls_on(m, rel, is_root, path);
    mount_put(m);
}
