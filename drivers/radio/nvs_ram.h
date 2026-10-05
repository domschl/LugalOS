#ifndef LUGALOS_RADIO_NVS_RAM_H
#define LUGALOS_RADIO_NVS_RAM_H

#include <stdint.h>
#include <stddef.h>

/* A key/value store in RAM, with the semantics of the subset of ESP-IDF's NVS
 * that the Wi-Fi blob and its PHY use (45.3b, plan/phase45_esp32c6.md).
 *
 * The blob persists little: PHY calibration data, a few Wi-Fi settings. What it
 * needs from the store is not durability but the *protocol* -- above all that a
 * read of something never written says NOT_FOUND, because that is how it learns
 * to apply defaults, and that a get_blob with a short buffer says how long the
 * value is. This implements those, with IDF's own error values, over a
 * caller-supplied allocator (the radio's heap in the domain; malloc on the
 * host). Persistence -- writing the store to flash when the kernel asks -- is
 * a later layer's, and an empty store at boot is a legitimate state: the PHY
 * simply recalibrates.
 *
 * Namespaces up to 8, entries up to 64, names and keys up to 15 characters, as
 * IDF's. Values: i8, u8, u16 and blobs (the only types the blob's table uses).
 *
 * Not thread-safe: the owner serialises, as for the heap. */

#define NVS_OK                  0
#define NVS_FAIL               (-1)
#define NVS_ERR_NO_MEM          0x101
#define NVS_ERR_INVALID_ARG     0x102
#define NVS_ERR_NOT_FOUND       0x1102
#define NVS_ERR_TYPE_MISMATCH   0x1103
#define NVS_ERR_READ_ONLY       0x1104
#define NVS_ERR_NOT_ENOUGH_SPACE 0x1105
#define NVS_ERR_INVALID_NAME    0x1106
#define NVS_ERR_INVALID_HANDLE  0x1107
#define NVS_ERR_KEY_TOO_LONG    0x1109
#define NVS_ERR_INVALID_LENGTH  0x110c

#define NVS_READONLY  0
#define NVS_READWRITE 1

#define NVS_MAX_NAMESPACES 8
#define NVS_MAX_ENTRIES    64
#define NVS_MAX_HANDLES    8
#define NVS_NAME_MAX       15

typedef struct {
    void *(*alloc)(uint32_t);
    void  (*free)(void *);
} nvs_env_t;

typedef struct {
    char     name[NVS_NAME_MAX + 1];
    uint8_t  used;
} nvs_ns_t;

typedef struct {
    uint8_t  used, type, ns;
    char     key[NVS_NAME_MAX + 1];
    uint32_t len;
    union { int8_t i8; uint8_t u8; uint16_t u16; void *blob; } v;
} nvs_entry_t;

typedef struct {
    nvs_env_t   env;
    nvs_ns_t    ns[NVS_MAX_NAMESPACES];
    nvs_entry_t e[NVS_MAX_ENTRIES];
    struct { uint8_t open, ns, rw; } h[NVS_MAX_HANDLES];
} nvs_store_t;

void nvs_ram_init(nvs_store_t *s, nvs_env_t env);
/* Releases every blob. */
void nvs_ram_deinit(nvs_store_t *s);

int nvs_ram_open(nvs_store_t *s, const char *name, unsigned mode, uint32_t *handle);
void nvs_ram_close(nvs_store_t *s, uint32_t handle);
int nvs_ram_commit(nvs_store_t *s, uint32_t handle);

int nvs_ram_set_i8(nvs_store_t *s, uint32_t h, const char *key, int8_t v);
int nvs_ram_get_i8(nvs_store_t *s, uint32_t h, const char *key, int8_t *out);
int nvs_ram_set_u8(nvs_store_t *s, uint32_t h, const char *key, uint8_t v);
int nvs_ram_get_u8(nvs_store_t *s, uint32_t h, const char *key, uint8_t *out);
int nvs_ram_set_u16(nvs_store_t *s, uint32_t h, const char *key, uint16_t v);
int nvs_ram_get_u16(nvs_store_t *s, uint32_t h, const char *key, uint16_t *out);
int nvs_ram_set_blob(nvs_store_t *s, uint32_t h, const char *key, const void *v, size_t len);
/* `out` NULL: only reports the length. A short buffer: INVALID_LENGTH, with the
 * needed length in *len -- IDF's contract. */
int nvs_ram_get_blob(nvs_store_t *s, uint32_t h, const char *key, void *out, size_t *len);
int nvs_ram_erase_key(nvs_store_t *s, uint32_t h, const char *key);

#endif
