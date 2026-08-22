#include "kernel/scratch.h"
#include "kernel/palloc.h"

/* See kernel/include/kernel/scratch.h for the rule and the rationale. */

bool scratch_acquire(scratch_t *s, uint32_t bytes) {
    if (!s) return false;
    s->base = NULL;
    s->pages = 0;
    if (bytes == 0) return false;

    uint32_t pages = (bytes + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    void *p = palloc_pages(pages);
    if (!p) return false;   /* caller reports; this layer has no context to */

    s->base = p;
    s->pages = pages;
    return true;
}

void scratch_release(scratch_t *s) {
    if (!s || !s->base) return;
    palloc_free(s->base, s->pages);
    s->base = NULL;
    s->pages = 0;
}
