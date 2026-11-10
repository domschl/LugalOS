/* kernel/settings.c -- the settings store (kernel/include/kernel/settings.h). */

#include "kernel/settings.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "fs/vfs.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4)
#include "drivers/audio_esp32p4.h"
#include "drivers/dsi_esp32p4.h"
#endif
#if defined(CONFIG_BOARD_TAB5)
#include "kernel/console.h"
#include "kernel/orient.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)
#include "drivers/lcd7.h"
#endif

#include <string.h>

static const char *const k_orient[] = { "landscape", "portrait", "auto" };
static const char *const k_text_size[] = { "small", "medium", "large" };
static const char *const k_face[] = { "shortcuts", "files" };

#if defined(CONFIG_BOARD_TAB5)
#define VOLUME_DEFAULT 70
#define ORIENT_DEFAULT 2      /* auto: the Tab5 turns with the hand that holds it (owner, 2026-11-10) */
#else
#define VOLUME_DEFAULT 60
#define ORIENT_DEFAULT 0
#endif

/* Sorted by key (strcmp). */
static const setting_def_t k_settings[] = {
    { "audio.volume",       SETTING_INT,  VOLUME_DEFAULT, 0, 100, NULL },
    { "display.brightness", SETTING_INT,  100, 10, 100, NULL },
    { "display.orient",     SETTING_ENUM, ORIENT_DEFAULT, 0, 2, k_orient },
    { "input.natural_scroll", SETTING_BOOL, 1, 0, 1, NULL },
    { "input.pointer_speed", SETTING_INT, 100, 25, 400, NULL },
    { "launcher.face",      SETTING_ENUM, 0,   0,  1,   k_face },
    { "ui.click_sound",     SETTING_BOOL, 0,   0,  1,   NULL },
    { "ui.osk_preview",     SETTING_BOOL, 1,   0,  1,   NULL },
    { "ui.text_size",       SETTING_ENUM, 1,   0,  2,   k_text_size },
};
#define N_SETTINGS (sizeof(k_settings) / sizeof(k_settings[0]))

/* guarded by: g_settings_lock -- set from the input path (the launcher) and
 * the shell, written to the file by the shell's idle turn. */
static int16_t    g_value[N_SETTINGS];
static bool       g_loaded;               /* g_value holds values (else the defaults) */
static uint64_t   g_dirty_ms;             /* the last unsaved change; 0: saved */
static spinlock_t g_settings_lock = SPINLOCK_INIT;

#define SETTINGS_SD    "/sd0/system/settings"
#define SETTINGS_FLASH "/flash0/system/settings"
#define SAVE_AFTER_MS  2000u

static int index_of(const char *key) {
    unsigned lo = 0, hi = N_SETTINGS;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(key, k_settings[mid].key);
        if (c == 0) return (int)mid;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return -1;
}

const setting_def_t *settings_find(const char *key) {
    int i = key ? index_of(key) : -1;
    return i >= 0 ? &k_settings[i] : NULL;
}

const setting_def_t *settings_at(unsigned i) {
    return i < N_SETTINGS ? &k_settings[i] : NULL;
}

static void defaults_once(void) {
    if (g_loaded) return;
    for (unsigned i = 0; i < N_SETTINGS; i++) g_value[i] = k_settings[i].def;
    g_loaded = true;
}

int settings_get(const char *key) {
    int i = key ? index_of(key) : -1;
    if (i < 0) return -1;
    uintptr_t f = spin_lock_irqsave(&g_settings_lock);
    defaults_once();
    int v = g_value[i];
    spin_unlock_irqrestore(&g_settings_lock, f);
    return v;
}

/* A setting's effect on the machine. Where a board has nothing to apply it
 * to, it is kept for whoever reads it (a view, later). */
static void apply(const setting_def_t *d, int v) {
    (void)d; (void)v;
#if defined(CONFIG_BOARD_ESP32P4)
    if (strcmp(d->key, "audio.volume") == 0) audio_p4_set_volume(v);
#endif
#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
    if (strcmp(d->key, "display.brightness") == 0) dsi_lcd_set_backlight((unsigned)v);
#elif defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)
    if (strcmp(d->key, "display.brightness") == 0) lcd7_set_backlight((unsigned)v);
#endif
#if defined(CONFIG_BOARD_TAB5)
    if (strcmp(d->key, "display.orient") == 0) {
        if (v == 2) {
            orient_set_auto(true);
        } else {
            /* Turned only if it is the other way round now: a landscape
             * display upside down stays as it is (`orient off` keeps it). */
            orient_set_auto(false);
            int rot = dsi_lcd_get_orientation();
            bool portrait_now = rot == 0 || rot == 180;
            if (portrait_now != (v == 1) && dsi_lcd_set_orientation(v == 1 ? 0 : 270) == 0) console_sync_readers();
        }
    }
#endif
}

static int clamp(const setting_def_t *d, int v) {
    return v < d->min ? d->min : v > d->max ? d->max : v;
}

bool settings_set(const char *key, int v) {
    int i = key ? index_of(key) : -1;
    if (i < 0) return false;
    const setting_def_t *d = &k_settings[i];
    v = clamp(d, v);
    uintptr_t f = spin_lock_irqsave(&g_settings_lock);
    defaults_once();
    bool changed = g_value[i] != v;
    g_value[i] = (int16_t)v;
    if (changed) g_dirty_ms = time_get_ms();
    spin_unlock_irqrestore(&g_settings_lock, f);
    apply(d, v);
    return true;
}

/* text as a value of d's kind, or false. */
static bool parse(const setting_def_t *d, const char *text, int *v) {
    if (!text || !*text) return false;
    if (d->kind == SETTING_ENUM)
        for (int i = 0; i <= d->max; i++)
            if (strcmp(text, d->names[i]) == 0) { *v = i; return true; }
    if (d->kind == SETTING_BOOL) {
        if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0) { *v = 1; return true; }
        if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0) { *v = 0; return true; }
    }
    int n = 0;
    bool neg = *text == '-';
    const char *p = text + (neg ? 1 : 0);
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    if (*p) return false;
    *v = neg ? -n : n;
    return true;
}

bool settings_set_text(const char *key, const char *text) {
    const setting_def_t *d = settings_find(key);
    int v;
    return d && parse(d, text, &v) && settings_set(key, v);
}

void settings_format(const setting_def_t *d, int v, char *out, unsigned cap) {
    if (d->kind == SETTING_ENUM && v >= 0 && v <= d->max) ksnprintf(out, cap, "%s", d->names[v]);
    else if (d->kind == SETTING_BOOL) ksnprintf(out, cap, "%s", v ? "on" : "off");
    else ksnprintf(out, cap, "%d", v);
}

/* One `key=value` line of the file: applied when the key is known and the
 * value fits it; anything else is skipped (an older or newer file). */
static void load_line(char *line) {
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    const setting_def_t *d = settings_find(line);
    int v;
    if (d && parse(d, eq + 1, &v)) {
        int i = index_of(line);
        uintptr_t f = spin_lock_irqsave(&g_settings_lock);
        g_value[i] = (int16_t)clamp(d, v);
        spin_unlock_irqrestore(&g_settings_lock, f);
    }
}

void settings_load(void) {
    uintptr_t f = spin_lock_irqsave(&g_settings_lock);
    defaults_once();
    spin_unlock_irqrestore(&g_settings_lock, f);
    char buf[512];
    int n = vfs_read(SETTINGS_SD, buf, sizeof(buf) - 1);
    if (n < 0) n = vfs_read(SETTINGS_FLASH, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        for (char *p = buf; *p;) {
            char *e = p;
            while (*e && *e != '\n' && *e != '\r') e++;
            char end = *e;
            *e = '\0';
            if (*p && *p != '#') load_line(p);
            if (!end) break;
            p = e + 1;
        }
    }
    for (unsigned i = 0; i < N_SETTINGS; i++) apply(&k_settings[i], settings_get(k_settings[i].key));
}

void settings_poll(void) {
    uintptr_t f = spin_lock_irqsave(&g_settings_lock);
    bool due = g_dirty_ms && time_get_ms() - g_dirty_ms >= SAVE_AFTER_MS;
    int16_t snap[N_SETTINGS];
    if (due) {
        memcpy(snap, g_value, sizeof snap);
        g_dirty_ms = 0;
    }
    spin_unlock_irqrestore(&g_settings_lock, f);
    if (!due) return;
    char buf[512];
    unsigned o = (unsigned)ksnprintf(buf, sizeof buf, "# LugalOS settings (`settings` in the shell)\n");
    for (unsigned i = 0; i < N_SETTINGS && o < sizeof buf; i++) {
        /* Only what was changed: a setting at its default is left out, so a
         * new default reaches a card saved before it. */
        if (snap[i] == k_settings[i].def) continue;
        char v[16];
        settings_format(&k_settings[i], snap[i], v, sizeof v);
        o += (unsigned)ksnprintf(buf + o, sizeof buf - o, "%s=%s\n", k_settings[i].key, v);
    }
    if (o > sizeof buf) o = sizeof buf;
    bool sd = vfs_volume_mounted("sd0");
    const char *path = sd ? SETTINGS_SD : SETTINGS_FLASH;
    (void)vfs_mkdir(sd ? "/sd0/system" : "/flash0/system");
    if (vfs_write(path, buf, o) < 0) printk("[Settings] could not write %s\n", path);
}

int settings_check(void) {
    int bad = 0;
    for (unsigned i = 0; i < N_SETTINGS; i++) {
        const setting_def_t *d = &k_settings[i];
        if (i > 0 && strcmp(k_settings[i - 1].key, d->key) >= 0) bad++;
        if (d->def < d->min || d->def > d->max) bad++;
        if (d->kind == SETTING_ENUM && !d->names) bad++;
        if (settings_find(d->key) != d) bad++;
    }
    return bad;
}
