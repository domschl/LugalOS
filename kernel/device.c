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


/* --- Exclusive use of a wire (C8, plan/phase6_memory_and_processes.md) ---
 *
 * One holder per wire. The table is indexed by dev_wire_t and stores the
 * holding device's name, which is what makes a refusal informative: "uart is
 * already held by uartslip" tells the operator what to release, where a bare
 * failure would not.
 */
static const char *g_wire_owner[DEV_WIRE_VIRTIO + 1];

static const dev_driver_t *find_driver(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_num_devs; i++) {
        if (strcmp(g_devs[i].drv->name, name) == 0) return g_devs[i].drv;
    }
    return NULL;
}

int dev_claim(const char *name) {
    const dev_driver_t *drv = find_driver(name);
    if (!drv) return -1;
    if (drv->wire == DEV_WIRE_NONE) return 0; /* not an exclusive resource */
    /* A sharer neither takes the wire nor collides with whoever has it. */
    if (drv->flags & DEV_F_SHARES_WIRE) return 0;

    const char *owner = g_wire_owner[drv->wire];
    if (owner && strcmp(owner, name) != 0) {
        printk("[Device] '%s' cannot take its wire: '%s' already holds it\n",
               name, owner);
        return -1;
    }
    g_wire_owner[drv->wire] = drv->name;
    return 0;
}

void dev_release(const char *name) {
    const dev_driver_t *drv = find_driver(name);
    if (!drv || drv->wire == DEV_WIRE_NONE) return;
    if (g_wire_owner[drv->wire] &&
        strcmp(g_wire_owner[drv->wire], name) == 0) {
        g_wire_owner[drv->wire] = NULL;
    }
}

const char *dev_wire_owner(const char *name) {
    const dev_driver_t *drv = find_driver(name);
    if (!drv || drv->wire == DEV_WIRE_NONE) return NULL;
    return g_wire_owner[drv->wire];
}

static const char *wire_name(dev_wire_t w) {
    switch (w) {
        case DEV_WIRE_UART0:  return "uart0";
        case DEV_WIRE_ACM0:   return "acm0";
        case DEV_WIRE_ACM1:   return "acm1";
        case DEV_WIRE_VIRTIO: return "virtio";
        default:              return "-";
    }
}

bool dev_binding_info(uint32_t index, const char **name_out, const char **kind_out,
                      const char **wire_out, bool *present_out, bool *bound_out) {
    /* Walks only the devices that drive a wire: the others are not bindable
     * and listing them would make the interesting rows harder to find. */
    uint32_t seen = 0;
    for (int i = 0; i < g_num_devs; i++) {
        if (g_devs[i].drv->wire == DEV_WIRE_NONE) continue;
        if (seen++ != index) continue;

        const dev_driver_t *drv = g_devs[i].drv;
        if (name_out) *name_out = drv->name;
        if (kind_out) *kind_out = kind_name(drv->kind);
        if (wire_out) *wire_out = wire_name(drv->wire);
        if (present_out) *present_out = g_devs[i].present;
        if (bound_out) {
            const char *owner = g_wire_owner[drv->wire];
            *bound_out = owner && strcmp(owner, drv->name) == 0;
        }
        return true;
    }
    return false;
}
