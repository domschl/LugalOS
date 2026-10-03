#ifndef LUGALOS_TESTS_HOST_SHIM_H
#define LUGALOS_TESTS_HOST_SHIM_H

#include <stdint.h>

/* The in-memory files behind the shim's vfs_read()/vfs_write(). */
void host_file_put(const char *path, const void *data, uint32_t len);
const uint8_t *host_file_get(const char *path, uint32_t *len);
void host_files_clear(void);

/* A small deterministic PRNG, so a failing fuzz run is replayed exactly from
 * the seed it prints. */
static inline uint32_t host_rand(uint64_t *s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return (uint32_t)(*s >> 32);
}

#endif
