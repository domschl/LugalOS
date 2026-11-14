#include "liblugal.h"

/* U-mode test for Milestone 80.1:
 * - POSIX-style VFS syscalls (open, write, lseek, read, fstat, stat, readdir, unlink)
 * - /srv/canvas and /srv/sensor channel endpoints via liblugal.h */

static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int _start(void) {
    /* 1. VFS open/write/lseek/read/fstat/close on /ram0 */
    const char *path = "/ram0/uvfstest.txt";
    int fd = uopen(path, LUGAL_O_WRITE | LUGAL_O_CREATE | LUGAL_O_TRUNC);
    if (fd < 0) {
        uprint("UVFS_OPEN_W_FAIL\n");
        return 1;
    }
    static const char msg[] = "hello_lugal_vfs_0123456789";
    long nw = uwrite(fd, msg, sizeof(msg) - 1u);
    if (nw != (long)(sizeof(msg) - 1u)) {
        uprint("UVFS_WRITE_FAIL\n");
        uclose(fd);
        return 2;
    }
    uclose(fd);

    fd = uopen(path, LUGAL_O_READ);
    if (fd < 0) {
        uprint("UVFS_OPEN_R_FAIL\n");
        return 3;
    }
    lugal_stat_t st;
    if (ufstat(fd, &st) < 0 || st.size != (unsigned int)(sizeof(msg) - 1u) || st.is_dir != 0) {
        uprint("UVFS_FSTAT_FAIL\n");
        uclose(fd);
        return 4;
    }
    if (ulseek(fd, 6, LUGAL_SEEK_SET) != 6) {
        uprint("UVFS_LSEEK_FAIL\n");
        uclose(fd);
        return 5;
    }
    char rbuf[32];
    long nr = uread(fd, rbuf, 5);
    uclose(fd);
    if (nr != 5) {
        uprint("UVFS_READ_FAIL\n");
        return 6;
    }
    rbuf[5] = '\0';
    if (!streq(rbuf, "lugal")) {
        uprint("UVFS_DATA_FAIL\n");
        return 7;
    }

    if (ustat(path, &st) < 0 || st.size != (unsigned int)(sizeof(msg) - 1u)) {
        uprint("UVFS_STAT_FAIL\n");
        return 8;
    }

    /* 2. readdir on /ram0 */
    int dfd = uopen("/ram0", LUGAL_O_READ);
    if (dfd < 0) {
        uprint("UVFS_OPENDIR_FAIL\n");
        return 9;
    }
    int found = 0;
    for (unsigned int idx = 0; idx < 32; idx++) {
        char name[64];
        int rc = ureaddir(dfd, idx, name, sizeof(name), &st);
        if (rc < 0) break;
        if (streq_ci(name, "uvfstest.txt")) { found = 1; break; }
    }
    uclose(dfd);
    if (!found) {
        uprint("UVFS_READDIR_FAIL\n");
        return 10;
    }

    if (uunlink(path) < 0) {
        uprint("UVFS_UNLINK_FAIL\n");
        return 11;
    }
    uprint("UVFS_OK\n");

    /* 3. Channel endpoints: /srv/canvas and /srv/sensor */
    uint8_t creq[1] = { 'y' }, creply[LUGAL_SCREEN_REPLY_LEN];
    long cn = uchan_call("canvas", creq, 1, creply, sizeof(creply));
    if (cn < 1) {
        uprint("UCANVAS_CHAN_FAIL\n");
        return 12;
    }
    uprint("UCANVAS_CHAN_OK\n");

    char sbuf[128];
    long sn = lugal_sensor_list(sbuf, sizeof(sbuf));
    if (sn < 0) {
        uprint("USENSOR_CHAN_FAIL\n");
        return 13;
    }
    uprint("USENSOR_CHAN_OK\n");

    return 0;
}
