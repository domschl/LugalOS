/* drivers/icons.c -- the built-in icons, by name (drivers/include/drivers/icons.h). */

#include "drivers/icons.h"

#include <string.h>

/* drivers/icons_data.c, generated: sorted by name (strcmp). */
extern const icon_t   k_icons[];
extern const unsigned k_icon_count;

const icon_t *icon_find(const char *name) {
    if (!name) return NULL;
    unsigned lo = 0, hi = k_icon_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(name, k_icons[mid].name);
        if (c == 0) return &k_icons[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

const icon_t *icon_or(const char *name, const char *fallback) {
    const icon_t *i = icon_find(name);
    return i ? i : icon_find(fallback);
}

ugui_image_t icon_image(const icon_t *icon, int size, int *scale) {
    ugui_image_t img = { 0, 0, NULL };
    int s = 1;
    if (icon) {
        if (size >= 64) {
            img = (ugui_image_t){ 64, 64, icon->px64 };
            s = size / 64;
        } else {
            img = (ugui_image_t){ 32, 32, icon->px32 };
            s = size >= 32 ? size / 32 : 1;
        }
    }
    if (scale) *scale = s;
    return img;
}

const icon_t *icon_at(unsigned i) {
    return i < k_icon_count ? &k_icons[i] : NULL;
}

int icons_check(void) {
    int bad = 0;
    for (unsigned i = 0; i < k_icon_count; i++) {
        if (!k_icons[i].name || !k_icons[i].px32 || !k_icons[i].px64) bad++;
        if (i > 0 && strcmp(k_icons[i - 1].name, k_icons[i].name) >= 0) bad++;
        if (k_icons[i].name && icon_find(k_icons[i].name) != &k_icons[i]) bad++;
    }
    return bad;
}
