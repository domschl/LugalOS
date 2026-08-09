#include "kernel/device.h"
#include "kernel/printk.h"
#include <string.h>

/* See kernel/include/kernel/device.h for the rationale. */

typedef struct {
    const dev_driver_t *drv;
    bool probed;
    bool present;
} dev_slot_t;

static dev_slot_t g_devs[DEV_MAX];
static uint32_t   g_num_devs;

static const char *kind_name(dev_kind_t k) {
    switch (k) {
        case DEV_KIND_CONSOLE: return "console";
        case DEV_KIND_P9LINK:  return "p9link";
        case DEV_KIND_CLOCK:   return "clock";
        case DEV_KIND_EEPROM:  return "eeprom";
        case DEV_KIND_BLOCK:   return "block";
    }
    return "?";
}

int dev_register(const dev_driver_t *drv) {
    if (!drv || !drv->name) return -1;
    if (g_num_devs >= DEV_MAX) {
        printk("[Dev] Registry full, dropping '%s'\n", drv->name);
        return -1;
    }
    for (uint32_t i = 0; i < g_num_devs; i++) {
        if (strcmp(g_devs[i].drv->name, drv->name) == 0) {
            printk("[Dev] Duplicate device name '%s' ignored\n", drv->name);
            return -1;
        }
    }
    g_devs[g_num_devs].drv = drv;
    g_devs[g_num_devs].probed = false;
    g_devs[g_num_devs].present = false;
    g_num_devs++;
    return 0;
}

void dev_probe_all(void) {
    for (uint32_t i = 0; i < g_num_devs; i++) {
        dev_slot_t *s = &g_devs[i];
        if (s->probed) continue;
        s->probed = true;
        /* A NULL probe means "present, nothing to initialize" -- e.g. the
         * UART links, whose hardware was already brought up during boot
         * bootstrap before the registry existed. */
        s->present = s->drv->probe ? (s->drv->probe() == 0) : true;
    }

    printk("[Dev] Registry: ");
    for (uint32_t i = 0; i < g_num_devs; i++) {
        printk("%s%s%s", i ? ", " : "", g_devs[i].drv->name,
               g_devs[i].present ? "" : "(absent)");
    }
    printk("\n");
}

void *dev_get(const char *name, dev_kind_t kind) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_num_devs; i++) {
        const dev_driver_t *d = g_devs[i].drv;
        if (strcmp(d->name, name) != 0) continue;
        if (d->kind != kind) return NULL;
        if (!g_devs[i].present || !d->get) return NULL;
        return d->get();
    }
    return NULL;
}

bool dev_info(uint32_t index, const char **name_out, const char **kind_out,
              bool *present_out) {
    if (index >= g_num_devs) return false;
    if (name_out)    *name_out    = g_devs[index].drv->name;
    if (kind_out)    *kind_out    = kind_name(g_devs[index].drv->kind);
    if (present_out) *present_out = g_devs[index].present;
    return true;
}

void *dev_next_with_flags(uint32_t *cursor, dev_kind_t kind, uint32_t flags) {
    if (!cursor) return NULL;
    while (*cursor < g_num_devs) {
        dev_slot_t *s = &g_devs[*cursor];
        (*cursor)++;
        if (!s->present || !s->drv->get) continue;
        if (s->drv->kind != kind) continue;
        if ((s->drv->flags & flags) != flags) continue;
        void *obj = s->drv->get();
        if (obj) return obj;
    }
    return NULL;
}
