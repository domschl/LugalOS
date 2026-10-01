/*
 * The screen to a file -- 37.5a. See kernel/include/kernel/screenshot.h.
 */

#include "kernel/screenshot.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "fs/vfs.h"

#include <string.h>

#define SHOT_DIR   "/sd0/screenshots"
#define SHOT_ROWS  8u               /* pixel rows per write: 800 bytes */

static uint8_t reverse8(uint8_t b) {
    b = (uint8_t)((b & 0xf0u) >> 4 | (b & 0x0fu) << 4);
    b = (uint8_t)((b & 0xccu) >> 2 | (b & 0x33u) << 2);
    return (uint8_t)((b & 0xaau) >> 1 | (b & 0x55u) << 1);
}

int screenshot_save(const char *path, char *out, uint32_t cap) {
    const uint8_t *fb;
    unsigned w, h, stride;
    if (!console_pixels(&fb, &w, &h, &stride) || cap == 0) return -1;
    char name[48];
    if (path) {
        strncpy(name, path, sizeof(name) - 1u);
        name[sizeof(name) - 1u] = '\0';
    } else {
        (void)vfs_mkdir(SHOT_DIR);               /* fails harmlessly if it exists */
        vfs_stat_t st;
        int k = 1;
        for (; k < 1000; k++) {
            ksnprintf(name, sizeof(name), SHOT_DIR "/shot-%03d.pbm", k);
            if (vfs_stat(name, &st) != 0) break;
        }
        if (k == 1000) return -1;
    }
    int fd = vfs_open(name, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return -1;
    char hdr[24];
    int hn = ksnprintf(hdr, sizeof(hdr), "P4\n%u %u\n", w, h);
    uint64_t off = 0;
    bool ok = vfs_pwrite(fd, hdr, (uint32_t)hn, off) == hn;
    off += (uint64_t)hn;
    uint8_t buf[SHOT_ROWS * 100u];
    unsigned row_bytes = (w + 7u) / 8u;
    if (row_bytes > 100u) ok = false;            /* wider than the buffer: not this panel */
    for (unsigned y = 0; ok && y < h; y += SHOT_ROWS) {
        unsigned rows = h - y < SHOT_ROWS ? h - y : SHOT_ROWS, n = 0;
        for (unsigned r = 0; r < rows; r++)
            for (unsigned b = 0; b < row_bytes; b++) buf[n++] = reverse8(fb[(y + r) * stride + b]);
        ok = vfs_pwrite(fd, buf, n, off) == (int)n;
        off += n;
    }
    vfs_close(fd);
    if (!ok) return -1;
    strncpy(out, name, cap - 1u);
    out[cap - 1u] = '\0';
    return 0;
}
