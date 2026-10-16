/*
 * The screen to a file -- 37.5a. See kernel/include/kernel/screenshot.h.
 */

#include "kernel/screenshot.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "fs/vfs.h"

#include <string.h>

#define SHOT_DIR   "/sd0/screenshots"
#define SHOT_BUF   800u             /* bytes per write: 8 rows at 800 px, 6 at 1024 */
#define SHOT_MAX_ROW_BYTES 200u     /* 1600 px, the RAM screen's widest (47.6) */

static uint8_t reverse8(uint8_t b) {
    b = (uint8_t)((b & 0xf0u) >> 4 | (b & 0x0fu) << 4);
    b = (uint8_t)((b & 0xccu) >> 2 | (b & 0x33u) << 2);
    return (uint8_t)((b & 0xaau) >> 1 | (b & 0x55u) << 1);
}

int screenshot_save(const char *path, char *out, uint32_t cap) {
    const uint8_t *fb;
    unsigned w, h, stride, format;
    if (!console_pixels_format(&fb, &w, &h, &stride, &format) || cap == 0) return -1;
    char name[48];
    if (path) {
        strncpy(name, path, sizeof(name) - 1u);
        name[sizeof(name) - 1u] = '\0';
    } else {
        (void)vfs_mkdir(SHOT_DIR);               /* fails harmlessly if it exists */
        vfs_stat_t st;
        const char *ext = ".pbm";
        if (format == SCREEN_FORMAT_GRAY8) ext = ".pgm";
        else if (format == SCREEN_FORMAT_RGB565) ext = ".ppm";
        int k = 1;
        for (; k < 1000; k++) {
            ksnprintf(name, sizeof(name), SHOT_DIR "/shot-%03d%s", k, ext);
            if (vfs_stat(name, &st) != 0) break;
        }
        if (k == 1000) return -1;
    }
    int fd = vfs_open(name, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return -1;
    char hdr[32];
    int hn;
    if (format == SCREEN_FORMAT_GRAY8) {
        hn = ksnprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", w, h);
    } else if (format == SCREEN_FORMAT_RGB565) {
        hn = ksnprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", w, h);
    } else {
        hn = ksnprintf(hdr, sizeof(hdr), "P4\n%u %u\n", w, h);
    }
    uint64_t off = 0;
    bool ok = vfs_pwrite(fd, hdr, (uint32_t)hn, off) == hn;
    off += (uint64_t)hn;

    if (format == SCREEN_FORMAT_GRAY8) {
        if (stride == w) {
            uint32_t total = w * h;
            for (uint32_t pos = 0; ok && pos < total; ) {
                uint32_t chunk = total - pos;
                if (chunk > 2048u) chunk = 2048u;
                ok = vfs_pwrite(fd, fb + pos, chunk, off) == (int)chunk;
                off += chunk;
                pos += chunk;
            }
        } else {
            for (unsigned y = 0; ok && y < h; y++) {
                ok = vfs_pwrite(fd, fb + y * stride, w, off) == (int)w;
                off += w;
            }
        }
    } else if (format == SCREEN_FORMAT_RGB565) {
        uint8_t rgb_buf[4096];
        unsigned buf_len = 0;
        for (unsigned y = 0; ok && y < h; y++) {
            const uint16_t *row = (const uint16_t *)(fb + y * stride);
            for (unsigned x = 0; x < w; x++) {
                uint16_t pix = row[x];
                uint8_t r = (uint8_t)((pix >> 11) & 0x1fu);
                uint8_t g = (uint8_t)((pix >> 5) & 0x3fu);
                uint8_t b = (uint8_t)(pix & 0x1fu);
                rgb_buf[buf_len++] = (uint8_t)((r << 3) | (r >> 2));
                rgb_buf[buf_len++] = (uint8_t)((g << 2) | (g >> 4));
                rgb_buf[buf_len++] = (uint8_t)((b << 3) | (b >> 2));
                if (buf_len >= sizeof(rgb_buf) - 3u) {
                    ok = vfs_pwrite(fd, rgb_buf, buf_len, off) == (int)buf_len;
                    off += buf_len;
                    buf_len = 0;
                    if (!ok) break;
                }
            }
        }
        if (ok && buf_len > 0) {
            ok = vfs_pwrite(fd, rgb_buf, buf_len, off) == (int)buf_len;
            off += buf_len;
        }
    } else {
        uint8_t buf[SHOT_BUF];
        unsigned row_bytes = (w + 7u) / 8u;
        if (row_bytes > SHOT_MAX_ROW_BYTES) ok = false;   /* wider than any screen here */
        /* As many whole rows per write as fit: 800 bytes held up to 47.6, when
         * the buffer was 8 rows of the RP2350's 100 and refused anything wider. */
        unsigned per = ok ? SHOT_BUF / row_bytes : 1u;
        for (unsigned y = 0; ok && y < h; y += per) {
            unsigned rows = h - y < per ? h - y : per, n = 0;
            for (unsigned r = 0; r < rows; r++)
                for (unsigned b = 0; b < row_bytes; b++) buf[n++] = reverse8(fb[(y + r) * stride + b]);
            ok = vfs_pwrite(fd, buf, n, off) == (int)n;
            off += n;
        }
    }
    vfs_close(fd);
    if (!ok) return -1;
    if (out && cap > 0) {
        strncpy(out, name, cap - 1u);
        out[cap - 1u] = '\0';
    }
    return 0;
}
