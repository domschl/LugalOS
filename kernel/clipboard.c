/*
 * The clipboard. See
 * kernel/include/kernel/clipboard.h.
 */

#include "kernel/clipboard.h"
#include "kernel/palloc.h"
#include "kernel/lock.h"

#include <string.h>

#define CLIP_PAGES ((CLIPBOARD_MAX + PAGE_SIZE - 1u) / PAGE_SIZE)

/* guarded by: g_clip_lock for every write (and the first allocation).
 * accepted: clipboard_data() hands out the buffer unlocked, so a paste during
 * a copy in another terminal can read part of each. */
static char    *g_clip;
static uint32_t g_clip_len;
static ylock_t  g_clip_lock;

static bool clip_ready(void) {
    if (!g_clip) g_clip = (char *)palloc_pages(CLIP_PAGES);
    return g_clip != NULL;
}

bool clipboard_set(const char *s, uint32_t n) {
    if (n > CLIPBOARD_MAX) return false;
    ylock_acquire(&g_clip_lock);
    bool ok = clip_ready();
    if (ok) {
        memcpy(g_clip, s, n);
        g_clip_len = n;
    }
    ylock_release(&g_clip_lock);
    return ok;
}

int clipboard_write_at(const char *s, uint32_t n, uint32_t offset) {
    ylock_acquire(&g_clip_lock);
    int rc = -1;
    if ((offset == 0 || offset == g_clip_len) && offset + n <= CLIPBOARD_MAX && clip_ready()) {
        memcpy(g_clip + offset, s, n);
        g_clip_len = offset + n;
        rc = (int)n;
    }
    ylock_release(&g_clip_lock);
    return rc;
}

const char *clipboard_data(uint32_t *n) {
    *n = g_clip ? g_clip_len : 0;
    return *n ? g_clip : NULL;
}
