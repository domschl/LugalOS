#include "kernel/path.h"
#include "kernel/printk.h"
#include "fs/vfs.h"
#include <string.h>

/* See kernel/include/kernel/path.h for the rationale. */

static char g_volumes[PATH_MAX_VOLUMES][PATH_VOLUME_MAX];
static int  g_count;

/* Most volatile first: a utility written to the RAM disk shadows the one
 * shipped in flash. See the header on why that is deliberate and what it
 * implies. */
static const char *const g_default[] = { "ram0", "sd0", "flash0" };

void path_init(void) {
    g_count = 0;
    for (unsigned i = 0; i < sizeof(g_default) / sizeof(g_default[0]); i++) {
        if (g_count >= PATH_MAX_VOLUMES) break;
        /* Bounded copy: the defaults are short, but this is the same routine
         * path_set() uses on caller-supplied text and there is no reason for
         * two of them. */
        uint32_t n = 0;
        while (g_default[i][n] && n < PATH_VOLUME_MAX - 1) {
            g_volumes[g_count][n] = g_default[i][n];
            n++;
        }
        g_volumes[g_count][n] = '\0';
        g_count++;
    }
}

int path_set(const char *spec) {
    if (!spec) return -1;

    /* Parsed into a scratch copy first, so a malformed spec cannot leave the
     * machine with a half-built path and nothing resolvable. Only a spec that
     * yields at least one volume replaces the live one. */
    char tmp[PATH_MAX_VOLUMES][PATH_VOLUME_MAX];
    int count = 0;

    const char *p = spec;
    while (*p && count < PATH_MAX_VOLUMES) {
        while (*p == ' ' || *p == '\t' || *p == ':' || *p == '/') p++;
        if (!*p) break;

        uint32_t n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ':' && *p != '/') {
            if (n < PATH_VOLUME_MAX - 1) tmp[count][n++] = *p;
            p++;
        }
        tmp[count][n] = '\0';
        if (n > 0) count++;
    }

    if (count == 0) return -1;

    for (int i = 0; i < count; i++) {
        memcpy(g_volumes[i], tmp[i], PATH_VOLUME_MAX);
    }
    g_count = count;
    return count;
}

int path_count(void) {
    return g_count;
}

const char *path_volume(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return g_volumes[index];
}

int path_format(char *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    uint32_t used = 0;
    for (int i = 0; i < g_count; i++) {
        used += (uint32_t)ksnprintf(buf + used, cap - used, "%s%s",
                                    i ? " " : "", g_volumes[i]);
    }
    return (int)used;
}

int path_resolve(const char *subdir, const char *name, const char *suffix,
                 char *out, uint32_t out_len) {
    if (!subdir || !name || !out || out_len == 0) return -1;
    if (!suffix) suffix = "";

    for (int i = 0; i < g_count; i++) {
        ksnprintf(out, out_len, "/%s/system/%s/%s%s",
                  g_volumes[i], subdir, name, suffix);

        /* vfs_stat() rather than an open/close pair: resolution runs on every
         * unrecognised word typed at the shell, and most of those are typos
         * or Lisp symbols that will match nothing on any volume. It must be
         * cheap to fail. */
        vfs_stat_t st;
        if (vfs_stat(out, &st) == 0 && !st.is_dir) {
            return 0;
        }
    }

    out[0] = '\0';
    return -1;
}
