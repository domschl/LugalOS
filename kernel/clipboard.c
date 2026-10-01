/*
 * The clipboard -- 37.5a, plan/phase37_screen_layouts_and_apps.md. See
 * kernel/include/kernel/clipboard.h.
 */

#include "kernel/clipboard.h"
#include "kernel/palloc.h"

#include <string.h>

#define CLIP_PAGES ((CLIPBOARD_MAX + PAGE_SIZE - 1u) / PAGE_SIZE)

static char    *g_clip;
static uint32_t g_clip_len;

static bool clip_ready(void) {
    if (!g_clip) g_clip = (char *)palloc_pages(CLIP_PAGES);
    return g_clip != NULL;
}

bool clipboard_set(const char *s, uint32_t n) {
    if (n > CLIPBOARD_MAX || !clip_ready()) return false;
    memcpy(g_clip, s, n);
    g_clip_len = n;
    return true;
}

int clipboard_write_at(const char *s, uint32_t n, uint32_t offset) {
    if (offset != 0 && offset != g_clip_len) return -1;
    if (offset + n > CLIPBOARD_MAX || !clip_ready()) return -1;
    memcpy(g_clip + offset, s, n);
    g_clip_len = offset + n;
    return (int)n;
}

const char *clipboard_data(uint32_t *n) {
    *n = g_clip ? g_clip_len : 0;
    return *n ? g_clip : NULL;
}
