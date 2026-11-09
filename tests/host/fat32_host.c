#define _GNU_SOURCE   /* strcasestr */
/*
 * fs/fat32.c on the host, under ASan/UBSan or valgrind.
 *
 * Two parts:
 *
 * 1. **Functional, against a model.** Directories, files of the sizes where
 *    cluster arithmetic goes wrong (0, 1, 511..513, a cluster, many), writes
 *    past EOF, appends, truncation, removal, a directory grown past its first
 *    cluster, and a remount -- every file read back and compared with what the
 *    model says it holds. Then everything is removed and the free count must
 *    be back where it started: a leaked cluster is a failure.
 *
 * 2. **Corrupt images.** The image from part 1, with random bytes changed --
 *    weighted towards the boot sector, the FATs and the directories -- mounted
 *    and walked, read, written and deleted. An SD card is whatever someone
 *    last wrote to it on a PC, so the driver must survive any of them: the
 *    sanitizers catch a bad access, and an alarm per image catches a loop
 *    that never ends (a cyclic cluster chain, a directory that contains
 *    itself).
 *
 * Usage: fat32_host [iterations [seed]]. Prints the seed; a failure is
 * replayed by passing it back.
 */

#include "fs/fat32.h"
#include "shim.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define SECTOR 512u

static uint8_t *g_disk;
static uint32_t g_disk_blocks;

static bool     g_pulled;      /* the card is gone: every access fails */
static uint32_t g_pulled_calls;
static int64_t  g_fail_read_lba = -1;    /* this one sector's reads fail */
static int64_t  g_fail_write_from = -1;  /* writes at or past this sector fail */

static int dev_read(block_dev_t *d, void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    if (g_pulled) { g_pulled_calls++; return -1; }
    if (lba >= g_disk_blocks || n > g_disk_blocks - lba) return -1;
    if (g_fail_read_lba >= 0 && (int64_t)lba <= g_fail_read_lba && g_fail_read_lba < (int64_t)(lba + n))
        return -1;
    memcpy(buf, g_disk + (size_t)lba * SECTOR, (size_t)n * SECTOR);
    return 0;
}

static int dev_write(block_dev_t *d, const void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    if (g_pulled) { g_pulled_calls++; return -1; }
    if (lba >= g_disk_blocks || n > g_disk_blocks - lba) return -1;
    if (g_fail_write_from >= 0 && (int64_t)(lba + n) > g_fail_write_from) return -1;
    memcpy(g_disk + (size_t)lba * SECTOR, buf, (size_t)n * SECTOR);
    return 0;
}

static block_dev_t g_dev = { "hostdisk", SECTOR, 0, dev_read, dev_write };

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

/* --- Part 1: the model --- */

#define MAX_FILES 160
static struct { char path[600]; uint8_t *data; uint32_t len; int live; } g_model[MAX_FILES];
static int g_nfiles;

static void fill(uint8_t *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 131u + seed * 7u + (i >> 9));
}

static int model_find(const char *path) {
    for (int i = 0; i < g_nfiles; i++) if (g_model[i].live && strcmp(g_model[i].path, path) == 0) return i;
    return -1;
}

static void model_set(const char *path, const uint8_t *data, uint32_t len) {
    int i = model_find(path);
    if (i < 0) {
        if (g_nfiles >= MAX_FILES) abort();
        i = g_nfiles++;
        snprintf(g_model[i].path, sizeof(g_model[i].path), "%s", path);
        g_model[i].live = 1;
        g_model[i].data = NULL;
    }
    free(g_model[i].data);
    g_model[i].data = malloc(len ? len : 1);
    memcpy(g_model[i].data, data, len);
    g_model[i].len = len;
}

static void verify_all(fat32_fs_t *fs, const char *when) {
    for (int i = 0; i < g_nfiles; i++) {
        if (!g_model[i].live) continue;
        fat32_dir_entry_t e;
        int r = fat32_find_file(fs, g_model[i].path, &e);
        CHECK(r == 0, "%s: %s not found", when, g_model[i].path);
        if (r != 0) continue;
        CHECK(e.file_size == g_model[i].len, "%s: %s is %u bytes, model %u", when,
              g_model[i].path, e.file_size, g_model[i].len);
        uint8_t *buf = malloc(g_model[i].len + 64);
        int n = fat32_read_file(fs, &e, buf, g_model[i].len + 64);
        CHECK(n == (int)g_model[i].len, "%s: %s read %d of %u", when, g_model[i].path, n, g_model[i].len);
        if (n == (int)g_model[i].len)
            CHECK(memcmp(buf, g_model[i].data, g_model[i].len) == 0, "%s: %s content differs",
                  when, g_model[i].path);
        free(buf);
    }
}

static uint32_t free_blocks(fat32_fs_t *fs) {
    uint32_t total = 0, fr = 0;
    fat32_statfs(fs, &total, &fr);
    return fr;
}

static void functional(void) {
    g_disk_blocks = 8192;                         /* 4 MB: clusters of one sector */
    g_disk = calloc(g_disk_blocks, SECTOR);
    g_dev.num_blocks = g_disk_blocks;
    CHECK(fat32_format_quiet(&g_dev, true) == 0, "format");

    fat32_fs_t fs;
    memset(&fs, 0, sizeof(fs));
    CHECK(fat32_init_quiet(&fs, &g_dev, true) == 0, "init");
    uint32_t free0 = free_blocks(&fs);

    CHECK(fat32_mkdir(&fs, "A") == 0, "mkdir A");
    CHECK(fat32_mkdir(&fs, "A/B") == 0, "mkdir A/B");
    CHECK(fat32_mkdir(&fs, "A/B/C/") == 0, "mkdir A/B/C/ (trailing slash)");
    CHECK(fat32_mkdir(&fs, "A") != 0, "mkdir of an existing name succeeded");
    CHECK(fat32_mkdir(&fs, "NOPE/X") != 0, "mkdir under a missing parent succeeded");

    static const uint32_t sizes[] = { 0, 1, 511, 512, 513, 1024, 4095, 4096, 4097, 20000, 70001 };
    const char *dirs[] = { "", "A/", "A/B/", "A/B/C/" };
    uint8_t *buf = malloc(80000);
    int k = 0;
    for (unsigned d = 0; d < 4; d++)
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++, k++) {
            char path[64];
            snprintf(path, sizeof(path), "%sF%03d.BIN", dirs[d], k);
            fill(buf, sizes[s], (uint32_t)k);
            CHECK(fat32_write_file(&fs, path, buf, sizes[s]) == 0, "write %s", path);
            model_set(path, buf, sizes[s]);
        }
    verify_all(&fs, "after writes");

    /* Overwrite with a different size, both ways. */
    fill(buf, 9000, 999);
    CHECK(fat32_write_file(&fs, "F000.BIN", buf, 9000) == 0, "overwrite grow");
    model_set("F000.BIN", buf, 9000);
    fill(buf, 10, 998);
    CHECK(fat32_write_file(&fs, "F010.BIN", buf, 10) == 0, "overwrite shrink");
    model_set("F010.BIN", buf, 10);

    /* A write past EOF zero-fills the gap. */
    {
        int i = model_find("A/F012.BIN");
        uint32_t old = g_model[i].len, off = old + 3000, n = 700;
        uint8_t *nd = calloc(off + n, 1);
        memcpy(nd, g_model[i].data, old);
        fill(nd + off, n, 77);
        CHECK(fat32_write_at(&fs, "A/F012.BIN", nd + off, n, off, NULL) == (int)n, "write_at past EOF");
        model_set("A/F012.BIN", nd, off + n);
        free(nd);
    }
    /* Appends. */
    for (int a = 0; a < 5; a++) {
        int i = model_find("A/B/F023.BIN");
        uint32_t old = g_model[i].len, n = 333u * (uint32_t)(a + 1);
        uint8_t *nd = malloc(old + n);
        memcpy(nd, g_model[i].data, old);
        fill(nd + old, n, 50u + (uint32_t)a);
        CHECK(fat32_append_file(&fs, "A/B/F023.BIN", nd + old, n) >= 0, "append %d", a);
        model_set("A/B/F023.BIN", nd, old + n);
        free(nd);
    }
    /* Truncate. */
    CHECK(fat32_truncate(&fs, "A/B/C/F043.BIN") == 0, "truncate");
    model_set("A/B/C/F043.BIN", buf, 0);

    /* A directory grown well past one cluster. */
    CHECK(fat32_mkdir(&fs, "MANY") == 0, "mkdir MANY");
    for (int i = 0; i < 80; i++) {
        char path[64];
        snprintf(path, sizeof(path), "MANY/M%04d.TXT", i);
        fill(buf, (uint32_t)(i * 37), (uint32_t)i);
        CHECK(fat32_write_file(&fs, path, buf, (uint32_t)(i * 37)) == 0, "write %s", path);
        model_set(path, buf, (uint32_t)(i * 37));
    }
    verify_all(&fs, "after edits");

    /* The rules about kinds. */
    CHECK(fat32_remove_file(&fs, "A") != 0, "rm of a directory succeeded");
    CHECK(fat32_rmdir(&fs, "A") != 0, "rmdir of a non-empty directory succeeded");
    CHECK(fat32_rmdir(&fs, "F001.BIN") != 0, "rmdir of a file succeeded");
    fat32_dir_entry_t e;
    CHECK(fat32_find_file(&fs, "F001.BIN/", &e) != 0, "a file found with a trailing slash");
    CHECK(fat32_find_file(&fs, "A/B/", &e) == 0, "a directory not found with a trailing slash");

    /* Everything again, from a fresh mount of the same bytes. */
    fat32_sync(&fs);
    fat32_fs_t fs2;
    memset(&fs2, 0, sizeof(fs2));
    CHECK(fat32_init_quiet(&fs2, &g_dev, true) == 0, "remount");
    verify_all(&fs2, "after remount");

    /* Keep a populated image for part 2. */
    uint8_t *image = malloc((size_t)g_disk_blocks * SECTOR);
    memcpy(image, g_disk, (size_t)g_disk_blocks * SECTOR);

    /* Then remove it all: the free count must come back exactly. */
    for (int i = 0; i < g_nfiles; i++) {
        if (!g_model[i].live) continue;
        CHECK(fat32_remove_file(&fs2, g_model[i].path) == 0, "remove %s", g_model[i].path);
        g_model[i].live = 0;
    }
    CHECK(fat32_rmdir(&fs2, "A/B/C") == 0, "rmdir C");
    CHECK(fat32_rmdir(&fs2, "A/B") == 0, "rmdir B");
    CHECK(fat32_rmdir(&fs2, "A") == 0, "rmdir A");
    CHECK(fat32_rmdir(&fs2, "MANY") == 0, "rmdir MANY");
    uint32_t free1 = free_blocks(&fs2);
    uint32_t recount = fat32_recount(&fs2);
    CHECK(free1 == free0, "free blocks %u at start, %u after removing everything", free0, free1);
    CHECK(recount * fs2.bpb.sec_per_clus == free1, "recount %u clusters, statfs %u blocks", recount, free1);

    for (int i = 0; i < g_nfiles; i++) free(g_model[i].data);
    free(buf);
    memcpy(g_disk, image, (size_t)g_disk_blocks * SECTOR);
    free(image);
    printf("fat32_host: functional pass, %d files, %s\n", g_nfiles, g_failures ? "FAILED" : "ok");
}

/* --- Part 2: corrupt images --- */

static volatile sig_atomic_t g_iter;
static uint64_t g_seed0;

static void on_alarm(int sig) {
    (void)sig;
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "FAIL: hang on corrupt image %d (seed %llu)\n",
                     (int)g_iter, (unsigned long long)g_seed0);
    (void)!write(2, msg, (size_t)n);
    _exit(3);
}

/* Every entry read back by index and then found again by its path, so the
 * long-name sets of a damaged directory are parsed both ways. */
static void walk(fat32_fs_t *fs, const char *dir, uint32_t clus, int depth, uint8_t *buf, uint32_t cap) {
    if (depth > 4) return;
    fat32_cursor_t cur = {0};
    for (uint32_t idx = 0; idx < 96; idx++) {
        char name[FAT32_NAME_MAX];
        fat32_dir_entry_t e, again;
        if (fat32_readdir(fs, clus, idx, (idx & 1) ? &cur : NULL, name, sizeof(name), &e) != 0) break;
        if (name[0] == '.') continue;
        char path[600];
        snprintf(path, sizeof(path), "%s%s", dir, name);
        (void)fat32_find_file(fs, path, &again);
        if (e.attr & FAT32_ATTR_DIRECTORY) {
            uint32_t c = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
            strncat(path, "/", sizeof(path) - strlen(path) - 1);
            if (c >= 2) walk(fs, path, c, depth + 1, buf, cap);
        } else {
            uint32_t want = e.file_size < cap ? e.file_size : cap;
            (void)fat32_read_at(fs, &e, buf, want, 0, NULL);
            (void)fat32_read_at(fs, &e, buf, 100, e.file_size > 50 ? e.file_size - 50 : 0, NULL);
        }
    }
}

static void corrupt_images(int iterations, uint64_t seed) {
    size_t bytes = (size_t)g_disk_blocks * SECTOR;
    uint8_t *base = malloc(bytes);
    memcpy(base, g_disk, bytes);
    uint8_t *buf = malloc(131072);
    signal(SIGALRM, on_alarm);
    g_seed0 = seed;
    uint64_t s = seed | 1;

    /* Where FAT32 keeps its structure, read from the image itself. */
    fat32_bpb_t *bpb = (fat32_bpb_t *)base;
    uint32_t meta_end = bpb->reserved_sec_cnt + bpb->num_fats * bpb->fat_sz32 + 256u;

    int mounted = 0;
    for (int it = 0; it < iterations; it++) {
        g_iter = it;
        memcpy(g_disk, base, bytes);
        int nmut = 1 + (int)(host_rand(&s) % 24u);
        for (int m = 0; m < nmut; m++) {
            uint32_t r = host_rand(&s);
            size_t off;
            switch (r % 4u) {
                case 0:  off = host_rand(&s) % SECTOR; break;                       /* boot sector */
                case 1:  off = host_rand(&s) % ((size_t)meta_end * SECTOR); break;  /* FATs, root */
                default: off = host_rand(&s) % bytes; break;                        /* anywhere */
            }
            uint32_t v = host_rand(&s);
            switch ((v >> 8) % 4u) {
                case 0:  g_disk[off] = (uint8_t)v; break;
                case 1:  g_disk[off] ^= (uint8_t)(1u << (v % 8u)); break;
                case 2:  g_disk[off] = 0xFF; break;
                default: g_disk[off] = 0x00; break;
            }
        }
        /* One time in four, a geometry field of the boot sector set to
         * something a byte flip would rarely produce: zero, all ones, or a
         * plausible-looking random value. */
        if (host_rand(&s) % 4u == 0) {
            static const uint8_t field_off[] = { 11, 13, 14, 16, 32, 36, 44, 48 };
            static const uint8_t field_len[] = {  2,  1,  2,  1,  4,  4,  4,  2 };
            unsigned f = host_rand(&s) % sizeof(field_off);
            uint32_t v = host_rand(&s);
            switch (host_rand(&s) % 3u) { case 0: v = 0; break; case 1: v = 0xFFFFFFFFu; break; default: break; }
            for (unsigned b = 0; b < field_len[f]; b++) g_disk[field_off[f] + b] = (uint8_t)(v >> (8 * b));
        }

        alarm(5);
        fat32_fs_t fs;
        memset(&fs, 0, sizeof(fs));
        if (fat32_init_quiet(&fs, &g_dev, true) == 0) {
            mounted++;
            walk(&fs, "", fs.root_dir_cluster, 0, buf, 131072);
            fat32_dir_entry_t e;
            (void)fat32_find_file(&fs, "A/B/F023.BIN", &e);
            fill(buf, 3000, (uint32_t)it);
            (void)fat32_write_file(&fs, "NEW.BIN", buf, 3000);
            (void)fat32_write_at(&fs, "A/F012.BIN", buf, 1000, 5000, NULL);
            (void)fat32_append_file(&fs, "MANY/M0007.TXT", buf, 600);
            (void)fat32_mkdir(&fs, "A/B/D");
            (void)fat32_remove_file(&fs, "F005.BIN");
            (void)fat32_truncate(&fs, "A/F013.BIN");
            (void)fat32_rmdir(&fs, "A/B/C");
            uint32_t t, f;
            (void)fat32_statfs(&fs, &t, &f);
            (void)fat32_recount(&fs);
            walk(&fs, "", fs.root_dir_cluster, 0, buf, 131072);
            {
                uint32_t nb = (fs.total_clusters + 2u + 7u) / 8u;
                uint8_t *bits = malloc(nb ? nb : 1);
                uint32_t q[32];
                fat32_check_t r;
                (void)fat32_check(&fs, bits, nb, q, 32, &r);
                free(bits);
            }
        }
        alarm(0);
    }
    free(buf);
    free(base);
    printf("fat32_host: %d corrupt images (seed %llu), %d of them mounted, no fault\n",
           iterations, (unsigned long long)seed, mounted);
}

/* --- Part 3: the card pulled from a mounted volume ---
 * Every access fails from then on. What the shell does next -- its history
 * append, and the mkdir that follows when that fails -- must fail after a
 * handful of device calls, not one per cluster: a failed read empties the
 * FAT cache, and the allocator used to take each unreadable entry for "in
 * use" and try the next, which on the LCD-7's card was most of an hour. */
static void pulled_card(void) {
    memset(g_disk, 0, (size_t)g_disk_blocks * SECTOR);
    CHECK(fat32_format_quiet(&g_dev, true) == 0, "format");
    fat32_fs_t fs;
    memset(&fs, 0, sizeof(fs));
    CHECK(fat32_init_quiet(&fs, &g_dev, true) == 0, "init");
    CHECK(fat32_mkdir(&fs, "SYSTEM") == 0, "mkdir before the pull");
    g_pulled = true;
    g_pulled_calls = 0;
    CHECK(fat32_append_file(&fs, "SYSTEM/HIST.TXT", "x\n", 2) < 0, "append with no card succeeded");
    CHECK(fat32_mkdir(&fs, "NEW") != 0, "mkdir with no card succeeded");
    CHECK(fat32_write_file(&fs, "F.TXT", "abc", 3) < 0, "write with no card succeeded");
    CHECK(g_pulled_calls < 64, "%u device calls with no card -- one per cluster?", g_pulled_calls);
    printf("fat32_host: pulled card, %u device calls for an append, a mkdir and a write, %s\n",
           g_pulled_calls, g_failures ? "FAILED" : "ok");
    g_pulled = false;
}

/* --- Part 4: single failed transfers on a working card ---
 * A block driver can fail one call (its task did not answer in time) and
 * then work again. Every such failure must reach the caller: a data write
 * that went nowhere reported as success left the old cluster's bytes in a
 * new file, a failed read returned whatever was in the buffer, and a
 * directory sector that was not read was written back over its sixteen
 * entries. */
static void failed_transfers(void) {
    memset(g_disk, 0, (size_t)g_disk_blocks * SECTOR);
    CHECK(fat32_format_quiet(&g_dev, true) == 0, "format");
    fat32_fs_t fs;
    memset(&fs, 0, sizeof(fs));
    CHECK(fat32_init_quiet(&fs, &g_dev, true) == 0, "init");
    static uint8_t a[700], b[700], back[700];
    for (int i = 0; i < 700; i++) { a[i] = (uint8_t)('a' + i % 26); b[i] = (uint8_t)('A' + i % 26); }
    CHECK(fat32_write_file(&fs, "A.TXT", a, 700) == 0, "write A");
    CHECK(fat32_write_file(&fs, "B.TXT", b, 700) == 0, "write B");
    uint32_t root_lba = fs.data_start_sector + (fs.root_dir_cluster - 2) * fs.bpb.sec_per_clus;

    /* Data writes fail: both write paths say so. */
    g_fail_write_from = fs.data_start_sector;
    CHECK(fat32_write_at(&fs, "A.TXT", b, 700, 0, NULL) < 0, "a failed data write reported success");
    CHECK(fat32_write_file(&fs, "C.TXT", a, 700) < 0, "a failed whole-file write reported success");
    g_fail_write_from = -1;

    /* A data read fails: no bytes are passed off as the file's. */
    fat32_dir_entry_t e;
    CHECK(fat32_find_file(&fs, "B.TXT", &e) == 0, "find B");
    uint32_t first = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
    g_fail_read_lba = fs.data_start_sector + (first - 2) * fs.bpb.sec_per_clus;
    CHECK(fat32_read_at(&fs, &e, back, 700, 0, NULL) < 0, "a failed data read reported success");
    g_fail_read_lba = -1;

    /* The root directory's sector unreadable while a write grows A: the
     * write fails and B's entry beside it survives. */
    CHECK(fat32_find_file(&fs, "A.TXT", &e) == 0, "find A");
    g_fail_read_lba = root_lba;
    (void)fat32_write_at(&fs, "A.TXT", a, 700, 700, NULL);
    g_fail_read_lba = -1;
    CHECK(fat32_find_file(&fs, "B.TXT", &e) == 0 && e.file_size == 700, "B's entry damaged");
    memset(back, 0, sizeof back);
    CHECK(fat32_read_at(&fs, &e, back, 700, 0, NULL) == 700 && memcmp(back, b, 700) == 0,
          "B's content damaged");
    printf("fat32_host: failed single transfers reported, %s\n", g_failures ? "FAILED" : "ok");
}

/* --- Part 5: the read-only check (`fsck`) ---
 * A volume this code wrote is clean; one with a directory entry pointed at
 * another file's cluster is cross-linked, and the cluster it used to own is
 * lost. */
static void check_volume(void) {
    memset(g_disk, 0, (size_t)g_disk_blocks * SECTOR);
    CHECK(fat32_format_quiet(&g_dev, true) == 0, "format");
    fat32_fs_t fs;
    memset(&fs, 0, sizeof(fs));
    CHECK(fat32_init_quiet(&fs, &g_dev, true) == 0, "init");
    static uint8_t data[3000];
    memset(data, 'x', sizeof data);
    CHECK(fat32_write_file(&fs, "A.TXT", data, 3000) == 0, "write A");
    CHECK(fat32_write_file(&fs, "B.TXT", data, 700) == 0, "write B");
    CHECK(fat32_write_file(&fs, "EMPTY.TXT", data, 0) == 0, "write EMPTY");
    CHECK(fat32_mkdir(&fs, "D") == 0, "mkdir D");
    CHECK(fat32_write_file(&fs, "D/C.TXT", data, 1500) == 0, "write D/C");

    uint32_t bytes = (fs.total_clusters + 2u + 7u) / 8u;
    uint8_t *bits = malloc(bytes);
    uint32_t q[64];
    fat32_check_t r;
    CHECK(fat32_check(&fs, bits, bytes, q, 64, &r) == 0, "check ran");
    CHECK(r.files == 4 && r.dirs == 1, "check found %u files, %u dirs", r.files, r.dirs);
    CHECK(r.cross == 0 && r.bad_chain == 0 && r.size == 0 && r.lost == 0,
          "clean volume reported %u cross %u chain %u size %u lost", r.cross, r.bad_chain, r.size, r.lost);

    /* B's entry pointed at A's first cluster, through the raw disk. */
    fat32_dir_entry_t ea, eb;
    CHECK(fat32_find_file(&fs, "A.TXT", &ea) == 0 && fat32_find_file(&fs, "B.TXT", &eb) == 0, "find A, B");
    uint32_t root_lba = fs.data_start_sector + (fs.root_dir_cluster - 2) * fs.bpb.sec_per_clus;
    fat32_dir_entry_t *ents = (fat32_dir_entry_t *)(g_disk + (size_t)root_lba * SECTOR);
    for (int i = 0; i < 16; i++)
        if (memcmp(ents[i].name, "B       TXT", 11) == 0) {
            ents[i].fst_clus_hi = ea.fst_clus_hi;
            ents[i].fst_clus_lo = ea.fst_clus_lo;
        }
    CHECK(fat32_check(&fs, bits, bytes, q, 64, &r) == 0, "check ran");
    CHECK(r.cross >= 1, "a cross-linked entry not reported");
    CHECK(r.lost >= 1, "B's old cluster not reported lost");
    free(bits);
    printf("fat32_host: check finds a clean volume clean and a cross-link, %s\n",
           g_failures ? "FAILED" : "ok");
}

/* --- Part 6: fat32_mkfs(), judged by dosfstools ---
 * A partitioned card image: mounts, takes files, our check finds it clean,
 * and so does fsck.fat when the host has it. */
static uint8_t *g_big;
static uint32_t g_big_blocks;
static int big_read(block_dev_t *d, void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    if (lba >= g_big_blocks || n > g_big_blocks - lba) return -1;
    memcpy(buf, g_big + (size_t)lba * SECTOR, (size_t)n * SECTOR);
    return 0;
}
static int big_write(block_dev_t *d, const void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    if (lba >= g_big_blocks || n > g_big_blocks - lba) return -1;
    memcpy(g_big + (size_t)lba * SECTOR, buf, (size_t)n * SECTOR);
    return 0;
}

static void mkfs_card(void) {
    g_big_blocks = 72u * 2048u;                     /* 72 MB: FAT32 needs ~36 MB at least */
    g_big = calloc(g_big_blocks, SECTOR);
    block_dev_t dev = { "card", SECTOR, g_big_blocks, big_read, big_write };
    memset(g_big, 0xA5, (size_t)g_big_blocks * SECTOR);   /* an old format's leftovers */
    CHECK(fat32_mkfs(&dev, "testcard", 0x12345678u, NULL) == 0, "mkfs");
    fat32_fs_t fs;
    memset(&fs, 0, sizeof fs);
    CHECK(fat32_init_quiet(&fs, &dev, true) == 0, "mount after mkfs");
    CHECK(fs.fsinfo_lba != 0 && fs.free_count != FAT32_FREE_UNKNOWN, "FSInfo not found after mkfs");
    CHECK((fs.data_start_sector % 8192u) == 0, "data area at %u, not on a 4 MB boundary", fs.data_start_sector);
    uint32_t free0 = fs.free_count;
    static uint8_t data[20000];
    for (int i = 0; i < (int)sizeof data; i++) data[i] = (uint8_t)i;
    CHECK(fat32_mkdir(&fs, "DIR") == 0, "mkdir");
    CHECK(fat32_write_file(&fs, "DIR/A.BIN", data, sizeof data) == 0, "write");
    CHECK(fat32_write_file(&fs, "B.TXT", "hello", 5) == 0, "write");
    fat32_sync(&fs);
    uint32_t nb = (fs.total_clusters + 2u + 7u) / 8u;
    uint8_t *bits = malloc(nb);
    uint32_t q[16];
    fat32_check_t r;
    CHECK(fat32_check(&fs, bits, nb, q, 16, &r) == 0 && r.cross + r.bad_chain + r.size + r.lost == 0,
          "our check on a fresh card: %u cross %u chain %u size %u lost", r.cross, r.bad_chain, r.size, r.lost);
    free(bits);
    CHECK(free0 == fs.total_clusters - 1u, "FSInfo free %u, want %u", free0, fs.total_clusters - 1u);
    const char *img = "../../build/host/mkfs_card.img";
    FILE *f = fopen(img, "wb");
    if (f) {
        fwrite(g_big, SECTOR, g_big_blocks, f);
        fclose(f);
        /* The partition, as fsck.fat wants it: offset 4 MB. */
        int rc = system("command -v fsck.fat >/dev/null 2>&1 || exit 77; "
                        "dd if=../../build/host/mkfs_card.img of=../../build/host/mkfs_part.img "
                        "bs=512 skip=8192 status=none && fsck.fat -n ../../build/host/mkfs_part.img >/dev/null");
        int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        if (code == 77) printf("fat32_host: fsck.fat not installed, skipped\n");
        else CHECK(code == 0, "fsck.fat found problems (exit %d): fsck.fat -n build/host/mkfs_part.img", code);
    }
    free(g_big);
    printf("fat32_host: mkfs card image (spc %u, %u clusters) %s\n", fs.bpb.sec_per_clus,
           fs.total_clusters, g_failures ? "FAILED" : "ok, fsck.fat agrees");
}

/* --- Part 6: long file names ---
 * Names written and read back exactly: mixed case, Latin-1, a character
 * outside the BMP, the longest name, names sharing their first letters (the
 * aliases must differ), lower-case 8.3 names (stored without a long name).
 * Matching ignores case; the alias opens the file too; invalid names are
 * refused; rename and move keep content; a removed set leaves no long name
 * behind. The volume is left populated for the corrupt-image part. */

static const char *const k_ldir = "Long Directory Name";

static bool dir_has(fat32_fs_t *fs, const char *dir, const char *name, fat32_dir_entry_t *out) {
    fat32_dir_entry_t d;
    if (fat32_find_file(fs, dir, &d) != 0) return false;
    uint32_t clus = ((uint32_t)d.fst_clus_hi << 16) | d.fst_clus_lo;
    if (clus == 0) clus = fs->root_dir_cluster;
    fat32_cursor_t cur = {0};
    char n[FAT32_NAME_MAX];
    fat32_dir_entry_t e;
    for (uint32_t i = 0; fat32_readdir(fs, clus, i, &cur, n, sizeof n, &e) == 0; i++) {
        if (strcmp(n, name) == 0) {
            if (out) *out = e;
            return true;
        }
    }
    return false;
}

static void put(fat32_fs_t *fs, const char *path, uint32_t len, uint32_t seed) {
    uint8_t *b = malloc(len ? len : 1);
    fill(b, len, seed);
    CHECK(fat32_write_file(fs, path, b, len) == 0, "write %s", path);
    model_set(path, b, len);
    free(b);
}

static void long_names(void) {
    /* A card's layout (fat32_mkfs), so fsck.fat has nothing to say about
     * anything but the names. */
    uint8_t *small = g_disk;
    uint32_t small_blocks = g_disk_blocks;
    g_disk_blocks = g_dev.num_blocks = 81920;
    g_disk = calloc(g_disk_blocks, SECTOR);
    CHECK(fat32_mkfs(&g_dev, "LFN", 0x4c464e, NULL) == 0, "mkfs");
    fat32_fs_t fs;
    memset(&fs, 0, sizeof fs);
    CHECK(fat32_init_quiet(&fs, &g_dev, true) == 0, "init");
    g_nfiles = 0;
    uint32_t free0 = free_blocks(&fs);

    char longest[FAT32_LFN_MAX + 1];
    memset(longest, 'x', FAT32_LFN_MAX);
    memcpy(longest, "The longest name", 16);
    longest[FAT32_LFN_MAX] = '\0';
    char p[600];

    CHECK(fat32_mkdir(&fs, k_ldir) == 0, "mkdir %s", k_ldir);
    CHECK(fat32_mkdir(&fs, "Long Directory Name/Bücher") == 0, "mkdir Bücher");
    static const char *const names[] = {
        "Ein längerer Dateiname mit Umlauten äöüß ÄÖÜ.md",
        "chapter one.md", "chapter two.md", "chapter three.md", "chapter four.md",
        "chapter five.md", "chapter six.md", "chapter seven.md",
        "notes.txt", "NOTES2.TXT", "Mixed.Txt", "a.b.c.d", "trailing space inside .txt",
        "smile \xF0\x9F\x98\x80 here.txt", ".hidden", "lugalos.lisp", "lugalos.lis~",
    };
    uint32_t seed = 500;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", k_ldir, names[i]);
        put(&fs, p, 100u + i * 997u, seed++);
    }
    snprintf(p, sizeof p, "%s/%s", k_ldir, longest);
    put(&fs, p, 4000, seed++);
    put(&fs, "Long Directory Name/Bücher/Faust I.md", 7000, seed++);
    verify_all(&fs, "long names");

    /* Listed exactly as created. */
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        CHECK(dir_has(&fs, k_ldir, names[i], NULL), "'%s' not listed as created", names[i]);
    CHECK(dir_has(&fs, k_ldir, longest, NULL), "the longest name not listed");
    CHECK(dir_has(&fs, "", k_ldir, NULL), "'%s' not listed", k_ldir);

    /* The lisp file and its backup copy are two files (the 8.3-era alias). */
    fat32_dir_entry_t a, b;
    CHECK(fat32_find_file(&fs, "Long Directory Name/lugalos.lisp", &a) == 0 &&
          fat32_find_file(&fs, "Long Directory Name/lugalos.lis~", &b) == 0 &&
          a.fst_clus_lo != b.fst_clus_lo, "lugalos.lisp and lugalos.lis~ are one file");

    /* Lower-case 8.3 names are 8.3 entries with case flags. */
    fat32_dir_entry_t e;
    CHECK(dir_has(&fs, k_ldir, "notes.txt", &e) && memcmp(e.name, "NOTES   TXT", 11) == 0 && e.nt_res == 0x18,
          "notes.txt not stored as NOTES.TXT with case flags");
    CHECK(dir_has(&fs, k_ldir, "NOTES2.TXT", &e) && memcmp(e.name, "NOTES2  TXT", 11) == 0 && e.nt_res == 0,
          "NOTES2.TXT not stored as itself");

    /* Aliases: unique, and each opens its file. */
    char aliases[8][12];
    for (int i = 0; i < 7; i++) {
        snprintf(p, sizeof p, "%s/%s", k_ldir, names[1 + i]);
        CHECK(fat32_find_file(&fs, p, &e) == 0, "find %s", p);
        memcpy(aliases[i], e.name, 11);
        aliases[i][11] = '\0';
        for (int j = 0; j < i; j++) CHECK(strcmp(aliases[i], aliases[j]) != 0, "alias %s twice", aliases[i]);
        char an[16];
        int k = 0;
        for (int c = 0; c < 8 && e.name[c] != ' '; c++) an[k++] = e.name[c];
        if (e.name[8] != ' ') {
            an[k++] = '.';
            for (int c = 8; c < 11 && e.name[c] != ' '; c++) an[k++] = e.name[c];
        }
        an[k] = '\0';
        char ap[64];
        snprintf(ap, sizeof ap, "%s/%s", k_ldir, an);
        fat32_dir_entry_t e2;
        CHECK(fat32_find_file(&fs, ap, &e2) == 0 && e2.fst_clus_lo == e.fst_clus_lo, "alias %s does not open %s", an, p);
    }
    CHECK(memcmp(aliases[0], "CHAPTE~1MD ", 11) == 0, "first alias is %s, not CHAPTE~1.MD", aliases[0]);

    /* Case does not matter for ASCII and Latin-1. */
    CHECK(fat32_find_file(&fs, "LONG DIRECTORY NAME/EIN LÄNGERER DATEINAME MIT UMLAUTEN ÄÖÜß äöü.MD", &e) == 0,
          "case-insensitive Latin-1 lookup");
    CHECK(fat32_find_file(&fs, "long directory name/bÜCHER/faust i.MD", &e) == 0, "case-insensitive path");
    /* A name differing only in case is the same file. */
    put(&fs, "Long Directory Name/MIXED.TXT", 333, seed++);
    CHECK(dir_has(&fs, k_ldir, "Mixed.Txt", NULL) && !dir_has(&fs, k_ldir, "MIXED.TXT", NULL),
          "writing MIXED.TXT did not keep Mixed.Txt's name");
    {   /* the model: one file, under the name it keeps */
        int was = model_find("Long Directory Name/Mixed.Txt");
        int now = model_find("Long Directory Name/MIXED.TXT");
        if (was >= 0) g_model[was].live = 0;
        if (now >= 0) snprintf(g_model[now].path, sizeof g_model[now].path, "Long Directory Name/Mixed.Txt");
    }

    /* Refused: forbidden characters, a trailing dot or space, too long, not UTF-8. */
    static const char *const bad[] = {
        "Long Directory Name/a:b", "Long Directory Name/a*b", "Long Directory Name/a?b",
        "Long Directory Name/a\"b", "Long Directory Name/a<b", "Long Directory Name/a|b",
        "Long Directory Name/dot.", "Long Directory Name/space ", "Long Directory Name/bad\xC3",
        "Long Directory Name/\x01x",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(fat32_write_file(&fs, bad[i], (const uint8_t *)"x", 1) != 0, "'%s' accepted", bad[i]);
    char toolong[FAT32_LFN_MAX + 2];
    memset(toolong, 'y', FAT32_LFN_MAX + 1);
    toolong[FAT32_LFN_MAX + 1] = '\0';
    snprintf(p, sizeof p, "%s/%s", k_ldir, toolong);
    CHECK(fat32_write_file(&fs, p, (const uint8_t *)"x", 1) != 0, "a 256-unit name accepted");

    /* A buffer too small for the long name gets the alias. */
    {
        fat32_dir_entry_t d;
        CHECK(fat32_find_file(&fs, k_ldir, &d) == 0, "find dir");
        uint32_t clus = ((uint32_t)d.fst_clus_hi << 16) | d.fst_clus_lo;
        char small[13];
        bool saw_alias = false;
        for (uint32_t i = 0; fat32_readdir(&fs, clus, i, NULL, small, sizeof small, &e) == 0; i++) {
            CHECK(strlen(small) < sizeof small, "readdir overflowed a small buffer");
            if (strchr(small, '~')) saw_alias = true;
        }
        CHECK(saw_alias, "no alias given for a long name in a 13-byte buffer");
    }

    /* Rename: within the directory, a change of case, onto an existing name,
     * a move to another directory, a directory moved, a directory into itself. */
    CHECK(fat32_rename(&fs, "Long Directory Name/chapter one.md", "Long Directory Name/Chapter One (renamed).md") == 0,
          "rename in place");
    CHECK(!dir_has(&fs, k_ldir, "chapter one.md", NULL) && dir_has(&fs, k_ldir, "Chapter One (renamed).md", NULL),
          "renamed file listed under the wrong name");
    CHECK(fat32_rename(&fs, "Long Directory Name/chapter two.md", "Long Directory Name/CHAPTER TWO.md") == 0,
          "rename to another case");
    CHECK(dir_has(&fs, k_ldir, "CHAPTER TWO.md", NULL) && !dir_has(&fs, k_ldir, "chapter two.md", NULL), "case rename");
    CHECK(fat32_rename(&fs, "Long Directory Name/chapter three.md", "Long Directory Name/chapter four.md") != 0,
          "rename onto an existing file succeeded");
    CHECK(fat32_rename(&fs, "Long Directory Name/chapter five.md", "Long Directory Name/Bücher/chapter five.md") == 0,
          "move to another directory");
    CHECK(fat32_rename(&fs, "Long Directory Name/Bücher", "Moved Books") == 0, "move a directory to the root");
    CHECK(fat32_rename(&fs, "Moved Books", "Moved Books/inside") != 0, "a directory moved into itself");
    CHECK(fat32_find_file(&fs, "Moved Books/../Long Directory Name/notes.txt", &e) == 0, "'..' of a moved directory");
    CHECK(fat32_rename(&fs, "Long Directory Name/nope.md", "Long Directory Name/x.md") != 0, "rename of a missing file");
    struct { const char *from, *to; } moved[] = {
        { "Long Directory Name/chapter one.md", "Long Directory Name/Chapter One (renamed).md" },
        { "Long Directory Name/chapter two.md", "Long Directory Name/CHAPTER TWO.md" },
        { "Long Directory Name/chapter five.md", "Moved Books/chapter five.md" },
        { "Long Directory Name/Bücher/Faust I.md", "Moved Books/Faust I.md" },
    };
    for (unsigned i = 0; i < sizeof moved / sizeof moved[0]; i++) {
        int k = model_find(moved[i].from);
        CHECK(k >= 0, "model %s", moved[i].from);
        if (k >= 0) snprintf(g_model[k].path, sizeof g_model[k].path, "%s", moved[i].to);
    }
    verify_all(&fs, "after renames");

    /* A write through a stale place fails once the file is renamed away. */
    {
        fat32_loc_t loc;
        CHECK(fat32_lookup(&fs, "Long Directory Name/chapter six.md", &e, &loc) == 0, "lookup chapter six");
        CHECK(fat32_write_at_loc(&fs, &loc, &e, "abc", 3, 0, NULL) == 3, "write_at_loc");
        CHECK(fat32_rename(&fs, "Long Directory Name/chapter six.md", "Long Directory Name/six.md") == 0, "rename six");
        CHECK(fat32_write_at_loc(&fs, &loc, &e, "abc", 3, 0, NULL) < 0, "a write through a stale place succeeded");
        int k = model_find("Long Directory Name/chapter six.md");
        if (k >= 0) {
            memcpy(g_model[k].data, "abc", 3);
            snprintf(g_model[k].path, sizeof g_model[k].path, "Long Directory Name/six.md");
        }
    }

    /* Removing a long name removes its whole set: the slots are reused, and
     * no orphaned long-name slot attaches itself to the next file there. */
    snprintf(p, sizeof p, "%s/%s", k_ldir, longest);
    CHECK(fat32_remove_file(&fs, p) == 0, "remove the longest name");
    int k = model_find(p);
    if (k >= 0) g_model[k].live = 0;
    put(&fs, "Long Directory Name/AFTER.TXT", 10, seed++);
    CHECK(dir_has(&fs, k_ldir, "AFTER.TXT", NULL), "a file in reused slots got a stale long name");

    fat32_sync(&fs);
    fat32_fs_t fs2;
    memset(&fs2, 0, sizeof fs2);
    CHECK(fat32_init_quiet(&fs2, &g_dev, true) == 0, "remount");
    verify_all(&fs2, "long names after remount");

    /* fsck.fat reads it back: the names it lists are ours. */
    FILE *f = fopen("../../build/host/lfn.img", "wb");
    if (f) {
        fwrite(g_disk + 8192u * SECTOR, SECTOR, g_disk_blocks - 8192u, f);   /* the partition */
        fclose(f);
        int rc = system("command -v fsck.fat >/dev/null 2>&1 || exit 77; "
                        "fsck.fat -n -l ../../build/host/lfn.img > ../../build/host/lfn.fsck 2>&1");
        int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        if (code == 77) {
            printf("fat32_host: fsck.fat not installed, long-name image not checked\n");
        } else {
            CHECK(code == 0, "fsck.fat found problems (exit %d): see build/host/lfn.fsck", code);
            FILE *l = fopen("../../build/host/lfn.fsck", "r");
            char *txt = calloc(1, 65536);
            if (l) { (void)!fread(txt, 1, 65535, l); fclose(l); }
            /* fsck.fat shows an 8.3-only name in upper case (it ignores the
             * case flags), and characters outside its code page its own way. */
            for (int i = 0; i < g_nfiles; i++) {
                if (!g_model[i].live || strstr(g_model[i].path, "\xF0")) continue;
                char want[700];
                snprintf(want, sizeof want, "Checking file /%s", g_model[i].path);
                CHECK(strstr(txt, want) != NULL || strcasestr(txt, want) != NULL,
                      "fsck.fat does not list /%s", g_model[i].path);
            }
            free(txt);
        }
    }

    /* Everything removed: no cluster lost, and the directories empty. */
    for (int i = 0; i < g_nfiles; i++) {
        if (!g_model[i].live) continue;
        CHECK(fat32_remove_file(&fs2, g_model[i].path) == 0, "remove %s", g_model[i].path);
    }
    CHECK(fat32_rmdir(&fs2, "Moved Books") == 0, "rmdir Moved Books");
    CHECK(fat32_rmdir(&fs2, k_ldir) == 0, "rmdir %s", k_ldir);
    CHECK(free_blocks(&fs2) == free0, "free blocks %u at start, %u after removing everything", free0, free_blocks(&fs2));

    /* The small volume back, populated with long names for the corrupt-image part. */
    for (int i = 0; i < g_nfiles; i++) free(g_model[i].data);
    g_nfiles = 0;
    free(g_disk);
    g_disk = small;
    g_disk_blocks = g_dev.num_blocks = small_blocks;
    memset(g_disk, 0, (size_t)g_disk_blocks * SECTOR);
    CHECK(fat32_format_quiet(&g_dev, true) == 0, "format");
    memset(&fs2, 0, sizeof fs2);
    CHECK(fat32_init_quiet(&fs2, &g_dev, true) == 0, "init");
    CHECK(fat32_mkdir(&fs2, k_ldir) == 0, "mkdir again");
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", k_ldir, names[i]);
        put(&fs2, p, 50u + i * 300u, seed++);
    }
    for (int i = 0; i < 60; i++) {
        snprintf(p, sizeof p, "%s/file with a long name number %d.txt", k_ldir, i);
        put(&fs2, p, (uint32_t)i * 41u, seed++);
    }
    fat32_sync(&fs2);
    for (int i = 0; i < g_nfiles; i++) free(g_model[i].data);
    g_nfiles = 0;
    printf("fat32_host: long names, aliases, case, rename %s\n",
           g_failures ? "FAILED" : (f ? "ok, fsck.fat agrees" : "ok"));
}

/* --- Part 7: interchange ---
 * A volume made by mkfs.fat, filled by mtools with names a PC would write,
 * read here; and names written here, read by mtools. Skipped (and said so)
 * where the tools are not installed. */
static void interchange(void) {
    const char *img = "../../build/host/xchg.img";
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "command -v mkfs.fat >/dev/null 2>&1 || exit 77; command -v mcopy >/dev/null 2>&1 || exit 78; "
             "rm -f %s && mkfs.fat -F 32 -s 1 -C %s 40000 >/dev/null && "
             "printf 'from a PC\\n' > ../../build/host/xchg.txt && "
             "mmd -i %s '::/Documents and Settings' && "
             "mcopy -i %s ../../build/host/xchg.txt '::/Documents and Settings/Grüße aus Köln.TXT' && "
             "mcopy -i %s ../../build/host/xchg.txt '::/MixedCase.Md' && "
             "mcopy -i %s ../../build/host/xchg.txt '::/lower.txt'",
             img, img, img, img, img, img);
    int rc = system(cmd);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    if (code == 77 || code == 78) {
        printf("fat32_host: interchange skipped (%s not installed)\n", code == 77 ? "mkfs.fat" : "mtools");
        return;
    }
    CHECK(code == 0, "building %s with mtools failed (exit %d)", img, code);
    FILE *f = fopen(img, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    uint32_t blocks = (uint32_t)(ftell(f) / SECTOR);
    fseek(f, 0, SEEK_SET);
    uint8_t *saved = g_disk;
    uint32_t saved_blocks = g_disk_blocks;
    g_disk = malloc((size_t)blocks * SECTOR);
    g_disk_blocks = g_dev.num_blocks = blocks;
    CHECK(fread(g_disk, SECTOR, blocks, f) == blocks, "read %s", img);
    fclose(f);

    fat32_fs_t fs;
    memset(&fs, 0, sizeof fs);
    CHECK(fat32_init_quiet(&fs, &g_dev, true) == 0, "mount the mkfs.fat image");
    uint8_t buf[64];
    fat32_dir_entry_t e;
    static const char *const theirs[] = {
        "Documents and Settings/Grüße aus Köln.TXT", "MixedCase.Md", "lower.txt",
    };
    for (unsigned i = 0; i < sizeof theirs / sizeof theirs[0]; i++) {
        CHECK(fat32_find_file(&fs, theirs[i], &e) == 0, "mtools' '%s' not found", theirs[i]);
        int n = fat32_read_file(&fs, &e, buf, sizeof buf);
        CHECK(n == 10 && memcmp(buf, "from a PC\n", 10) == 0, "mtools' '%s' reads wrong", theirs[i]);
    }
    CHECK(dir_has(&fs, "Documents and Settings", "Grüße aus Köln.TXT", NULL), "mtools' long name not listed exactly");
    CHECK(dir_has(&fs, "", "lower.txt", NULL), "mtools' lower-case 8.3 name not listed in lower case");

    static const char *const ours[] = {
        "Documents and Settings/Written by LugalOS.md", "Ünïcödé Ordner", "Ünïcödé Ordner/Datei mit Leerzeichen.txt",
    };
    CHECK(fat32_write_file(&fs, ours[0], (const uint8_t *)"from LugalOS\n", 13) == 0, "write %s", ours[0]);
    CHECK(fat32_mkdir(&fs, ours[1]) == 0, "mkdir %s", ours[1]);
    CHECK(fat32_write_file(&fs, ours[2], (const uint8_t *)"from LugalOS\n", 13) == 0, "write %s", ours[2]);
    CHECK(fat32_rename(&fs, "MixedCase.Md", "Renamed by LugalOS.md") == 0, "rename theirs");
    fat32_sync(&fs);
    f = fopen(img, "wb");
    if (f) {
        fwrite(g_disk, SECTOR, blocks, f);
        fclose(f);
    }
    snprintf(cmd, sizeof cmd,
             "mtype -i %s '::/Documents and Settings/Written by LugalOS.md' | grep -qx 'from LugalOS' && "
             "mtype -i %s '::/Ünïcödé Ordner/Datei mit Leerzeichen.txt' | grep -qx 'from LugalOS' && "
             "mtype -i %s '::/Renamed by LugalOS.md' | grep -qx 'from a PC' && "
             "! mtype -i %s '::/MixedCase.Md' >/dev/null 2>&1 && "
             "fsck.fat -n %s >/dev/null", img, img, img, img, img);
    rc = system(cmd);
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "mtools or fsck.fat disagree with what was written: %s", img);
    free(g_disk);
    g_disk = saved;
    g_disk_blocks = g_dev.num_blocks = saved_blocks;
    printf("fat32_host: interchange with mkfs.fat and mtools %s\n", g_failures ? "FAILED" : "ok");
}

/* --- Part 8: a large directory on a large volume ---
 * 1000 long names sharing their first letters, on a 256 GB volume (a sparse
 * file, so it costs only what is written): create, list, look up, remove.
 * Allocation must start from the FSInfo hint rather than scan the FAT, and a
 * listing must be one pass over the directory. */
static int g_big_fd = -1;
static uint64_t g_big_reads;

static int bigdev_read(block_dev_t *d, void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    g_big_reads += n;
    return pread(g_big_fd, buf, (size_t)n * SECTOR, (off_t)lba * SECTOR) == (ssize_t)(n * SECTOR) ? 0 : -1;
}

static int bigdev_write(block_dev_t *d, const void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    return pwrite(g_big_fd, buf, (size_t)n * SECTOR, (off_t)lba * SECTOR) == (ssize_t)(n * SECTOR) ? 0 : -1;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void large_directory(void) {
    const char *img = "../../build/host/big256.img";
    unlink(img);
    g_big_fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (g_big_fd < 0 || ftruncate(g_big_fd, 256ull * 1000 * 1000 * 1000) != 0) {
        printf("fat32_host: large directory skipped (no sparse 256 GB file here)\n");
        if (g_big_fd >= 0) close(g_big_fd);
        return;
    }
    block_dev_t dev = { "big", SECTOR, (uint32_t)(256ull * 1000 * 1000 * 1000 / SECTOR), bigdev_read, bigdev_write };
    CHECK(fat32_mkfs(&dev, "BIG", 0x1234, NULL) == 0, "mkfs 256 GB");
    fat32_fs_t fs;
    memset(&fs, 0, sizeof fs);
    static uint32_t cache[128];
    fat32_set_fat_cache(&fs, cache);
    CHECK(fat32_init_quiet(&fs, &dev, true) == 0, "mount 256 GB");
    CHECK(fat32_mkdir(&fs, "Books") == 0, "mkdir Books");

    enum { N = 1000 };
    char p[160];
    double t0 = now_s();
    g_big_reads = 0;
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "Books/Chapter number %04d of a book with a long title.md", i);
        CHECK(fat32_write_file(&fs, p, (const uint8_t *)p, (uint32_t)strlen(p)) == 0, "create %s", p);
    }
    double t_create = now_s() - t0;
    uint64_t r_create = g_big_reads;

    fat32_dir_entry_t d;
    CHECK(fat32_find_file(&fs, "Books", &d) == 0, "find Books");
    uint32_t clus = ((uint32_t)d.fst_clus_hi << 16) | d.fst_clus_lo;
    fat32_cursor_t cur = {0};
    char n[FAT32_NAME_MAX];
    fat32_dir_entry_t e;
    t0 = now_s();
    g_big_reads = 0;
    uint32_t count = 0;
    while (fat32_readdir(&fs, clus, count, &cur, n, sizeof n, &e) == 0) count++;
    double t_list = now_s() - t0;
    uint64_t r_list = g_big_reads;
    CHECK(count == N + 2, "listed %u entries, want %u", count, N + 2);
    /* 1000 names of 4 long-name slots and one 8.3 slot each: 5002 slots, 313
     * sectors. One pass: each call re-reads the sector its cursor resumes
     * in, and a few FAT sectors at cluster boundaries -- never a walk from
     * the directory's start. */
    uint32_t dir_sectors = (uint32_t)((N * 5u + 2u + 15u) / 16u);
    CHECK(r_list < (uint64_t)N + 2u * dir_sectors, "listing read %llu sectors for a %u-sector directory",
          (unsigned long long)r_list, dir_sectors);

    t0 = now_s();
    for (int i = 0; i < N; i += 37) {
        snprintf(p, sizeof p, "BOOKS/chapter NUMBER %04d of a book with a long title.MD", i);
        CHECK(fat32_find_file(&fs, p, &e) == 0 && e.file_size == strlen(p), "look up %s", p);
    }
    for (int i = 0; i < N; i++) {
        snprintf(p, sizeof p, "Books/Chapter number %04d of a book with a long title.md", i);
        CHECK(fat32_remove_file(&fs, p) == 0, "remove %s", p);
    }
    double t_rest = now_s() - t0;
    CHECK(fat32_rmdir(&fs, "Books") == 0, "rmdir Books");

    uint32_t nb = (fs.total_clusters + 2u + 7u) / 8u;
    uint8_t *bits = malloc(nb);
    uint32_t q[64];
    fat32_check_t r;
    CHECK(fat32_check(&fs, bits, nb, q, 64, &r) == 0 && r.cross == 0 && r.lost == 0 && r.bad_chain == 0,
          "fsck after the large directory: cross %u lost %u bad %u", r.cross, r.lost, r.bad_chain);
    free(bits);
    close(g_big_fd);
    unlink(img);
    /* Bounds generous enough for a slow CI machine, tight enough to catch a
     * scan of the FAT (8 million entries) or of the directory per entry read. */
    CHECK(t_create < 20.0 && t_list < 1.0 && t_rest < 20.0, "too slow: create %.2f s, list %.2f s, lookup+remove %.2f s",
          t_create, t_list, t_rest);
    printf("fat32_host: 1000 long names on 256 GB (%u clusters): create %.2f s (%llu sector reads), "
           "list %.3f s (%llu reads, %u-sector dir), lookup+remove %.2f s %s\n",
           fs.total_clusters, t_create, (unsigned long long)r_create, t_list, (unsigned long long)r_list,
           dir_sectors, t_rest, g_failures ? "FAILED" : "ok");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* in order with sanitizer reports on stderr */
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    functional();
    if (g_failures) return 1;
    pulled_card();
    if (g_failures) return 1;
    failed_transfers();
    if (g_failures) return 1;
    check_volume();
    if (g_failures) return 1;
    mkfs_card();
    if (g_failures) return 1;
    interchange();
    if (g_failures) return 1;
    large_directory();
    if (g_failures) return 1;
    long_names();
    if (g_failures) return 1;
    corrupt_images(iterations, seed);
    free(g_disk);
    return g_failures ? 1 : 0;
}
