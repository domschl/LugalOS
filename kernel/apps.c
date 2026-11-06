/* kernel/apps.c -- the application registry and the file associations
 * (kernel/apps.h). */

#include "kernel/apps.h"
#include "lugalos_config.h"

#include <string.h>

/* Sorted by id (strcmp), for find_sorted(). Entries for applications a build
 * leaves out are left out with them. */
static const app_desc_t k_apps[] = {
#if CONFIG_ENABLE_CERES || CONFIG_ENABLE_SCROLLER
    { "ceres",    "Ceres",    "ceres",       "ceres",    "ceres",    APP_KBD | APP_FULLSCREEN | APP_SINGLETON | APP_WINDOW },
#endif
#if CONFIG_ENABLE_CHESS
    { "chess",    "Chess",    "chess touch", "chess",    "Chess",    APP_TOUCH | APP_KBD | APP_SINGLETON | APP_WINDOW },
#endif
    { "editor",   "Editor",   "e %s",        "editor",   NULL,       APP_KBD },
#if CONFIG_ENABLE_MOONBASE
    { "moonbase", "Moonbase", "moonbase",    "moonbase", "moonbase", APP_TOUCH | APP_KBD | APP_FULLSCREEN | APP_SINGLETON | APP_WINDOW },
#endif
    { "reader",   "Reader",   "reader %s",   "reader",   NULL,       APP_TOUCH | APP_KBD },
    { "term",     "Terminal", "vterm new",   "terminal", NULL,       APP_KBD },
};

typedef struct {
    const char *ext;            /* lower case, with the dot */
    const char *app_id;
} app_assoc_t;

/* Sorted by extension (strcmp). */
static const app_assoc_t k_assoc[] = {
    { ".c",    "editor" },
    { ".gmi",  "reader" },
    { ".h",    "editor" },
    { ".lisp", "editor" },
    { ".lsp",  "editor" },
    { ".md",   "reader" },
    { ".txt",  "editor" },
};

#define N_APPS  (sizeof(k_apps) / sizeof(k_apps[0]))
#define N_ASSOC (sizeof(k_assoc) / sizeof(k_assoc[0]))

/* Binary search over a table sorted by the string at `key_of(i)`. */
static int find_sorted(const char *key, unsigned n, const char *(*key_of)(unsigned)) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(key, key_of(mid));
        if (c == 0) return (int)mid;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return -1;
}

static const char *app_key(unsigned i) { return k_apps[i].id; }
static const char *assoc_key(unsigned i) { return k_assoc[i].ext; }

static const char *last_char(const char *s, char c) {
    const char *last = NULL;
    for (; *s; s++)
        if (*s == c) last = s;
    return last;
}

const app_desc_t *app_find(const char *id) {
    if (!id) return NULL;
    int i = find_sorted(id, (unsigned)N_APPS, app_key);
    return i >= 0 ? &k_apps[i] : NULL;
}

const app_desc_t *app_for_path(const char *path) {
    if (!path) return NULL;
    const char *base = last_char(path, '/');
    base = base ? base + 1 : path;
    const char *dot = last_char(base, '.');
    if (!dot || dot == base) return NULL;
    char ext[8];
    size_t n = strlen(dot);
    if (n >= sizeof(ext)) return NULL;
    for (size_t i = 0; i <= n; i++) {
        char c = dot[i];
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    int i = find_sorted(ext, (unsigned)N_ASSOC, assoc_key);
    return i >= 0 ? app_find(k_assoc[i].app_id) : NULL;
}

bool app_command(const app_desc_t *a, const char *path, char *out, size_t cap) {
    if (!a || !out || cap == 0) return false;
    size_t o = 0;
    for (const char *p = a->exec; *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            if (path && *path) {
                size_t n = strlen(path);
                if (o + n >= cap) return false;
                memcpy(out + o, path, n);
                o += n;
            } else if (o > 0 && out[o - 1] == ' ') {
                o--;                                     /* no path: no trailing space either */
            }
            p++;
            continue;
        }
        if (o + 1 >= cap) return false;
        out[o++] = *p;
    }
    out[o] = '\0';
    return true;
}

const app_desc_t *app_at(unsigned i) {
    return i < N_APPS ? &k_apps[i] : NULL;
}

const app_desc_t *app_assoc_at(unsigned i, const char **ext) {
    if (i >= N_ASSOC) return NULL;
    if (ext) *ext = k_assoc[i].ext;
    return app_find(k_assoc[i].app_id);
}

int apps_check(void) {
    int bad = 0;
    for (unsigned i = 1; i < N_APPS; i++)
        if (strcmp(k_apps[i - 1].id, k_apps[i].id) >= 0) bad++;
    for (unsigned i = 1; i < N_ASSOC; i++)
        if (strcmp(k_assoc[i - 1].ext, k_assoc[i].ext) >= 0) bad++;
    for (unsigned i = 0; i < N_ASSOC; i++)
        if (!app_find(k_assoc[i].app_id)) bad++;
    for (unsigned i = 0; i < N_APPS; i++)
        if (app_find(k_apps[i].id) != &k_apps[i]) bad++;
    return bad;
}
