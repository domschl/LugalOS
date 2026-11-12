/* drivers/shortcuts.c -- the launcher's shortcuts (drivers/shortcuts.h). */

#include "drivers/shortcuts.h"
#include "kernel/console.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO))

/* No screen, no launcher: nothing kept. */
void shortcuts_load(void) {}
int shortcuts_count(void) { return 0; }
const shortcut_t *shortcuts_at(int i) { (void)i; return 0; }
const char *shortcuts_icon(const shortcut_t *s) { (void)s; return "app"; }
const char *shortcuts_path_icon(const char *name, bool dir) { (void)name; return dir ? "folder" : "doc"; }
bool shortcuts_add(shortcut_type_t type, const char *target, const char *label, const char *icon) {
    (void)type; (void)target; (void)label; (void)icon;
    return false;
}
bool shortcuts_remove(int i) { (void)i; return false; }
bool shortcuts_move(int i, int by) { (void)i; (void)by; return false; }
bool shortcuts_rename(int i, const char *label) { (void)i; (void)label; return false; }
bool shortcuts_reset(void) { return false; }
const char *shortcuts_type_name(shortcut_type_t t) { (void)t; return "app"; }
int shortcuts_type_of(const char *name) { (void)name; return -1; }
void shortcuts_cmd(const char *args) { (void)args; cprintf("shortcuts: no screen on this board\n"); }

#else

#include "drivers/view.h"
#include "fs/vfs.h"
#include "kernel/apps.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"

#include <string.h>

#define SC_NVME  "/nvme0/system/shortcuts"
#define SC_SD    "/sd0/system/shortcuts"
#define SC_FLASH "/flash0/system/shortcuts"
#define SC_FILE_MAX (SHORTCUT_MAX * (8 + SHORTCUT_TARGET + SHORTCUT_LABEL + SHORTCUT_ICON))

/* single task: views (the launcher; the shell's `shortcuts` through
 * view_call()). */
static struct {
    bool       loaded;
    int        n;
    shortcut_t sc[SHORTCUT_MAX];
    char       file[SC_FILE_MAX];   /* the file's text, read or to be written */
} s_sc BULK_BSS;

static const char *const k_types[] = { "app", "doc", "dir", "book" };

const char *shortcuts_type_name(shortcut_type_t t) {
    return (unsigned)t < 4u ? k_types[t] : "app";
}

int shortcuts_type_of(const char *name) {
    for (int i = 0; i < 4; i++)
        if (name && strcmp(name, k_types[i]) == 0) return i;
    return -1;
}

static void copy(char *dst, const char *src, unsigned cap) {
    unsigned n = 0;
    while (src && src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

/* The last part of a path ("/sd0/docs/a.md" -> "a.md"). */
static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1]) b = p + 1;
    return b;
}

static bool append(shortcut_type_t type, const char *target, const char *label, const char *icon) {
    if (s_sc.n >= SHORTCUT_MAX || !target || !*target) return false;
    shortcut_t *s = &s_sc.sc[s_sc.n++];
    s->type = (uint8_t)type;
    copy(s->target, target, sizeof s->target);
    if (label && *label) {
        copy(s->label, label, sizeof s->label);
    } else if (type == SHORTCUT_APP) {
        const app_desc_t *a = app_find(target);
        copy(s->label, a ? a->title : target, sizeof s->label);
    } else {
        copy(s->label, base_name(target), sizeof s->label);
    }
    copy(s->icon, icon, sizeof s->icon);
    return true;
}

/* What a person starts by touch first: the applications that need no file,
 * the help, the documents folder. */
static void defaults(void) {
    s_sc.n = 0;
    static const char *const k_apps[] = { "settings", "term", "chess", "moonbase" };
    for (unsigned i = 0; i < sizeof k_apps / sizeof k_apps[0]; i++)
        if (app_find(k_apps[i])) (void)append(SHORTCUT_APP, k_apps[i], NULL, NULL);
    bool nvme = vfs_volume_mounted("nvme0");
    (void)append(SHORTCUT_DOC, nvme ? "/nvme0/help/index.md" : "/sd0/help/index.md", "Help", "reader");
    (void)append(SHORTCUT_DIR, nvme ? "/nvme0/docs" : "/sd0/docs", "Documents", "folder");
}

/* One line, `type<TAB>target<TAB>label<TAB>icon`, the last two optional. */
static void parse_line(char *line) {
    char *f[4] = { line, NULL, NULL, NULL };
    for (int k = 1; k < 4; k++) {
        char *t = f[k - 1] ? strchr(f[k - 1], '\t') : NULL;
        if (!t) break;
        *t = '\0';
        f[k] = t + 1;
    }
    int type = shortcuts_type_of(f[0]);
    if (type >= 0 && f[1] && *f[1]) (void)append((shortcut_type_t)type, f[1], f[2], f[3]);
}

void shortcuts_load(void) {
    int n = -1;
    if (vfs_volume_mounted("nvme0")) n = vfs_read(SC_NVME, s_sc.file, sizeof s_sc.file - 1);
    if (n < 0 && vfs_volume_mounted("sd0")) n = vfs_read(SC_SD, s_sc.file, sizeof s_sc.file - 1);
    if (n < 0) n = vfs_read(SC_FLASH, s_sc.file, sizeof s_sc.file - 1);
    s_sc.loaded = true;
    if (n <= 0) {
        defaults();
        return;
    }
    s_sc.file[n] = '\0';
    s_sc.n = 0;
    for (char *p = s_sc.file; *p;) {
        char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        char end = *e;
        *e = '\0';
        if (*p && *p != '#') parse_line(p);
        if (!end) break;
        p = e + 1;
    }
}

static void once(void) {
    if (!s_sc.loaded) shortcuts_load();
}

int shortcuts_count(void) {
    once();
    return s_sc.n;
}

const shortcut_t *shortcuts_at(int i) {
    once();
    return i >= 0 && i < s_sc.n ? &s_sc.sc[i] : NULL;
}

const char *shortcuts_icon(const shortcut_t *s) {
    if (!s) return "app";
    if (s->icon[0]) return s->icon;
    if (s->type == SHORTCUT_APP) {
        const app_desc_t *a = app_find(s->target);
        return a && a->icon ? a->icon : "app";
    }
    if (s->type == SHORTCUT_BOOK) return "book";
    return shortcuts_path_icon(s->target, s->type == SHORTCUT_DIR);
}

/* ASCII case folded: FAT names match in any case. */
static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return false;
    for (const char *a = s + sl - xl, *b = suffix; *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        if (ca != *b) return false;
    }
    return true;
}

const char *shortcuts_path_icon(const char *name, bool dir) {
    if (dir) return "folder";
    static const struct { const char *ext, *icon; } k_ext[] = {
        { ".md", "doc-md" }, { ".gmi", "doc-gmi" }, { ".txt", "doc-txt" }, { ".lisp", "doc-lisp" },
        { ".lsp", "doc-lisp" }, { ".c", "doc-c" }, { ".h", "doc-c" },
    };
    for (unsigned i = 0; i < sizeof k_ext / sizeof k_ext[0]; i++)
        if (ends_with(name, k_ext[i].ext)) return k_ext[i].icon;
    return "doc";
}

static bool save(void) {
    unsigned o = (unsigned)ksnprintf(s_sc.file, sizeof s_sc.file,
                                     "# LugalOS shortcuts: type<TAB>target<TAB>label<TAB>icon (the launcher)\n");
    for (int i = 0; i < s_sc.n && o < sizeof s_sc.file; i++) {
        const shortcut_t *s = &s_sc.sc[i];
        o += (unsigned)ksnprintf(s_sc.file + o, sizeof s_sc.file - o, "%s\t%s\t%s\t%s\n",
                                 shortcuts_type_name((shortcut_type_t)s->type), s->target, s->label, s->icon);
    }
    if (o >= sizeof s_sc.file) return false;
    bool nvme = vfs_volume_mounted("nvme0");
    bool sd = !nvme && vfs_volume_mounted("sd0");
    const char *path = nvme ? SC_NVME : (sd ? SC_SD : SC_FLASH);
    (void)vfs_mkdir(nvme ? "/nvme0/system" : (sd ? "/sd0/system" : "/flash0/system"));
    return vfs_write(path, s_sc.file, o) >= 0;
}

bool shortcuts_add(shortcut_type_t type, const char *target, const char *label, const char *icon) {
    once();
    for (int i = 0; i < s_sc.n; i++)
        if (s_sc.sc[i].type == type && strcmp(s_sc.sc[i].target, target ? target : "") == 0) return true;
    return append(type, target, label, icon) && save();
}

bool shortcuts_remove(int i) {
    once();
    if (i < 0 || i >= s_sc.n) return false;
    memmove(&s_sc.sc[i], &s_sc.sc[i + 1], (size_t)(s_sc.n - i - 1) * sizeof s_sc.sc[0]);
    s_sc.n--;
    return save();
}

bool shortcuts_move(int i, int by) {
    once();
    int to = i + by;
    if (i < 0 || i >= s_sc.n || to < 0 || to >= s_sc.n || by == 0) return false;
    shortcut_t t = s_sc.sc[i];
    if (to > i) memmove(&s_sc.sc[i], &s_sc.sc[i + 1], (size_t)(to - i) * sizeof t);
    else memmove(&s_sc.sc[to + 1], &s_sc.sc[to], (size_t)(i - to) * sizeof t);
    s_sc.sc[to] = t;
    return save();
}

bool shortcuts_rename(int i, const char *label) {
    once();
    if (i < 0 || i >= s_sc.n || !label || !*label) return false;
    copy(s_sc.sc[i].label, label, sizeof s_sc.sc[i].label);
    return save();
}

bool shortcuts_reset(void) {
    (void)vfs_remove(SC_SD);
    (void)vfs_remove(SC_FLASH);
    defaults();
    s_sc.loaded = true;
    return true;
}

/* --- `shortcuts` from the shell, carried out on the views task --- */

typedef struct {
    const char *args;
    char        out[1024];
} sc_call_t;

static void sc_list(sc_call_t *c) {
    unsigned o = (unsigned)strlen(c->out);
    for (int i = 0; i < s_sc.n && o < sizeof c->out; i++) {
        const shortcut_t *s = &s_sc.sc[i];
        o += (unsigned)ksnprintf(c->out + o, sizeof c->out - o, "  [%d] %-4s %-28s %s (%s)\n", i,
                                 shortcuts_type_name((shortcut_type_t)s->type), s->label, s->target, shortcuts_icon(s));
    }
}

static int number(const char **p) {
    while (**p == ' ') (*p)++;
    int sign = 1, v = 0;
    if (**p == '-') { sign = -1; (*p)++; }
    while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
    return sign * v;
}

/* The next space-separated word into w ("" at the end). */
static const char *word(const char *p, char *w, unsigned cap) {
    while (*p == ' ') p++;
    unsigned n = 0;
    while (*p && *p != ' ' && n + 1 < cap) w[n++] = *p++;
    w[n] = '\0';
    while (*p && *p != ' ') p++;
    return p;
}

static void sc_call(void *arg) {
    sc_call_t *c = arg;
    once();
    const char *p = c->args;
    while (*p == ' ') p++;
    bool ok = true;
    if (!*p || strcmp(p, "list") == 0) {
        /* listed below */
    } else if (strncmp(p, "add ", 4) == 0) {
        char type[8], target[SHORTCUT_TARGET];
        p = word(p + 4, type, sizeof type);
        p = word(p, target, sizeof target);
        while (*p == ' ') p++;
        int t = shortcuts_type_of(type);
        ok = t >= 0 && target[0] && shortcuts_add((shortcut_type_t)t, target, p, NULL);
    } else if (strncmp(p, "remove ", 7) == 0) {
        p += 7;
        ok = shortcuts_remove(number(&p));
    } else if (strncmp(p, "move ", 5) == 0) {
        p += 5;
        int i = number(&p);
        ok = shortcuts_move(i, number(&p));
    } else if (strncmp(p, "rename ", 7) == 0) {
        p += 7;
        int i = number(&p);
        while (*p == ' ') p++;
        ok = shortcuts_rename(i, p);
    } else if (strcmp(p, "reset") == 0) {
        ok = shortcuts_reset();
    } else {
        ksnprintf(c->out, sizeof c->out,
                  "usage: shortcuts [list | add app|doc|dir|book TARGET [LABEL] | remove N | move N BY | rename N LABEL | reset]\n");
        return;
    }
    ksnprintf(c->out, sizeof c->out, "shortcuts: %s, %d\n", ok ? "ok" : "refused", s_sc.n);
    sc_list(c);
}

void shortcuts_cmd(const char *args) {
    /* accepted: one shell runs `shortcuts` at a time in practice; a second
     * one meanwhile would print the first one's answer. */
    static sc_call_t c BULK_BSS;
    c.args = args ? args : "";
    c.out[0] = '\0';
    view_call(sc_call, &c);
    cprintf("%s", c.out);
}

#endif
