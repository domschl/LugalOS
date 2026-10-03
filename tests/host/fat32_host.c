/*
 * fs/fat32.c on the host, under ASan/UBSan or valgrind (phase 40 review).
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR 512u

static uint8_t *g_disk;
static uint32_t g_disk_blocks;

static int dev_read(block_dev_t *d, void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    if (lba >= g_disk_blocks || n > g_disk_blocks - lba) return -1;
    memcpy(buf, g_disk + (size_t)lba * SECTOR, (size_t)n * SECTOR);
    return 0;
}

static int dev_write(block_dev_t *d, const void *buf, uint32_t lba, uint32_t n) {
    (void)d;
    if (lba >= g_disk_blocks || n > g_disk_blocks - lba) return -1;
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
static struct { char path[64]; uint8_t *data; uint32_t len; int live; } g_model[MAX_FILES];
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

static void walk(fat32_fs_t *fs, uint32_t clus, int depth, uint8_t *buf, uint32_t cap) {
    if (depth > 4) return;
    for (uint32_t idx = 0; idx < 96; idx++) {
        char name[32];
        fat32_dir_entry_t e;
        if (fat32_readdir(fs, clus, idx, name, sizeof(name), &e) != 0) break;
        if (name[0] == '.') continue;
        if (e.attr & FAT32_ATTR_DIRECTORY) {
            uint32_t c = ((uint32_t)e.fst_clus_hi << 16) | e.fst_clus_lo;
            if (c >= 2) walk(fs, c, depth + 1, buf, cap);
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
            walk(&fs, fs.root_dir_cluster, 0, buf, 131072);
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
            walk(&fs, fs.root_dir_cluster, 0, buf, 131072);
        }
        alarm(0);
    }
    free(buf);
    free(base);
    printf("fat32_host: %d corrupt images (seed %llu), %d of them mounted, no fault\n",
           iterations, (unsigned long long)seed, mounted);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* in order with sanitizer reports on stderr */
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    functional();
    if (g_failures) return 1;
    corrupt_images(iterations, seed);
    free(g_disk);
    return g_failures ? 1 : 0;
}
