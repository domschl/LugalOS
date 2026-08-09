#include "kernel/uaccess.h"
#include "kernel/mem_domain.h"
#include "kernel/sched.h"
#include <string.h>

/* See kernel/include/kernel/uaccess.h for the rationale. */

int copy_from_user(void *dst, uintptr_t usrc, uint32_t len) {
    if (!dst) return -1;
    if (len == 0) return 0;
    if (!mem_domain_permits(sched_current_domain(), usrc, len, MEM_R)) return -1;
    memcpy(dst, (const void *)usrc, len);
    return 0;
}

int copy_to_user(uintptr_t udst, const void *src, uint32_t len) {
    if (!src) return -1;
    if (len == 0) return 0;
    if (!mem_domain_permits(sched_current_domain(), udst, len, MEM_W)) return -1;
    memcpy((void *)udst, src, len);
    return 0;
}

int strncpy_from_user(char *dst, uintptr_t usrc, uint32_t max) {
    if (!dst || max == 0) return -1;

    const mem_domain_t *d = sched_current_domain();
    for (uint32_t i = 0; i < max; i++) {
        /* Checked one byte at a time, because the length is exactly what is
         * not yet known -- validating a guessed range would either trust the
         * caller or read past what it was allowed to. */
        if (!mem_domain_permits(d, usrc + i, 1, MEM_R)) return -1;
        char c = *(const volatile char *)(usrc + i);
        dst[i] = c;
        if (c == '\0') return (int)i;
    }
    /* Ran out of room without finding a terminator. Reporting failure rather
     * than truncating: a silently shortened path is a different path, and the
     * caller would have no way to tell. */
    return -1;
}
