/* user/ceres/src/ceres_storage.c — High-Score & Stats Persistence
 *
 * Implements persistent save/load across LugalOS VFS and host file systems.
 */

#include "ceres_storage.h"
#include <string.h>

#if defined(__LUGALOS__) || defined(CONFIG_BOARD_ESP32P4)
#include "fs/vfs.h"
#else
#include <stdio.h>
#endif

static uint32_t compute_checksum(const ceres_save_data_t *d) {
    uint32_t c = 0x5A5A5A5Au;
    c ^= d->magic;
    c = (c << 7) | (c >> 25);
    c ^= d->version;
    c = (c << 7) | (c >> 25);
    c ^= d->hi_score;
    c = (c << 7) | (c >> 25);
    c ^= d->total_rescued;
    c = (c << 7) | (c >> 25);
    c ^= d->games_played;
    c = (c << 7) | (c >> 25);
    c ^= d->highest_sector;
    return c;
}

bool ceres_storage_load(ceres_save_data_t *out) {
    if (!out) return false;

    memset(out, 0, sizeof(*out));

#if defined(__LUGALOS__) || defined(CONFIG_BOARD_ESP32P4)
    static const char * const paths[] = {
        "/sd0/ceres.dat",
        "/flash0/ceres.dat",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        int bytes = vfs_read(paths[i], out, sizeof(ceres_save_data_t));
        if (bytes == (int)sizeof(ceres_save_data_t)) {
            if (out->magic == CERES_SAVE_MAGIC &&
                out->version == CERES_SAVE_VERSION &&
                out->checksum == compute_checksum(out)) {
                return true;
            }
        }
    }
    return false;
#else
    FILE *f = fopen("ceres.dat", "rb");
    if (!f) return false;

    size_t read_bytes = fread(out, 1, sizeof(ceres_save_data_t), f);
    fclose(f);

    if (read_bytes == sizeof(ceres_save_data_t)) {
        if (out->magic == CERES_SAVE_MAGIC &&
            out->version == CERES_SAVE_VERSION &&
            out->checksum == compute_checksum(out)) {
            return true;
        }
    }
    return false;
#endif
}

bool ceres_storage_save(const ceres_save_data_t *in) {
    if (!in) return false;

    ceres_save_data_t to_write = *in;
    to_write.magic = CERES_SAVE_MAGIC;
    to_write.version = CERES_SAVE_VERSION;
    to_write.checksum = compute_checksum(&to_write);

#if defined(__LUGALOS__) || defined(CONFIG_BOARD_ESP32P4)
    if (vfs_volume_mounted("sd0") && vfs_volume_writable("sd0")) {
        int rc = vfs_write("/sd0/ceres.dat", &to_write, sizeof(to_write));
        if (rc == (int)sizeof(to_write)) return true;
    }
    if (vfs_volume_mounted("flash0") && vfs_volume_writable("flash0")) {
        int rc = vfs_write("/flash0/ceres.dat", &to_write, sizeof(to_write));
        if (rc == (int)sizeof(to_write)) return true;
    }
    return false;
#else
    FILE *f = fopen("ceres.dat", "wb");
    if (!f) return false;

    size_t written = fwrite(&to_write, 1, sizeof(to_write), f);
    fclose(f);
    return (written == sizeof(to_write));
#endif
}
