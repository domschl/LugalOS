/* See drivers/radio/nvs_ram.h. 45.3b, plan/phase45_esp32c6.md. */

#include "nvs_ram.h"

#ifndef RADIO_TEXT
#define RADIO_TEXT
#endif

enum { T_NONE = 0, T_I8, T_U8, T_U16, T_BLOB };

static RADIO_TEXT int name_len(const char *s) {
    int n = 0;
    while (s[n] && n <= NVS_NAME_MAX) n++;
    return n;
}

static RADIO_TEXT int name_eq(const char *a, const char *b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static RADIO_TEXT void name_copy(char *dst, const char *src) {
    int i = 0;
    for (; src[i] && i < NVS_NAME_MAX; i++) dst[i] = src[i];
    dst[i] = 0;
}

RADIO_TEXT void nvs_ram_init(nvs_store_t *s, nvs_env_t env) {
    char *p = (char *)s;
    for (uint32_t i = 0; i < sizeof *s; i++) p[i] = 0;
    s->env = env;
}

RADIO_TEXT void nvs_ram_deinit(nvs_store_t *s) {
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (s->e[i].used && s->e[i].type == T_BLOB && s->e[i].v.blob) s->env.free(s->e[i].v.blob);
    nvs_env_t env = s->env;
    nvs_ram_init(s, env);
}

RADIO_TEXT int nvs_ram_open(nvs_store_t *s, const char *name, unsigned mode, uint32_t *handle) {
    if (!name || !handle) return NVS_ERR_INVALID_ARG;
    int n = name_len(name);
    if (n == 0 || n > NVS_NAME_MAX) return NVS_ERR_INVALID_NAME;
    int ns = -1;
    for (int i = 0; i < NVS_MAX_NAMESPACES; i++)
        if (s->ns[i].used && name_eq(s->ns[i].name, name)) { ns = i; break; }
    if (ns < 0) {
        if (mode == NVS_READONLY) return NVS_ERR_NOT_FOUND;           /* IDF: it does not exist yet */
        for (int i = 0; i < NVS_MAX_NAMESPACES; i++)
            if (!s->ns[i].used) { ns = i; break; }
        if (ns < 0) return NVS_ERR_NOT_ENOUGH_SPACE;
        s->ns[ns].used = 1;
        name_copy(s->ns[ns].name, name);
    }
    for (int i = 0; i < NVS_MAX_HANDLES; i++) {
        if (s->h[i].open) continue;
        s->h[i].open = 1; s->h[i].ns = (uint8_t)ns; s->h[i].rw = mode != NVS_READONLY;
        *handle = (uint32_t)i + 1;
        return NVS_OK;
    }
    return NVS_ERR_NO_MEM;
}

static RADIO_TEXT int hcheck(const nvs_store_t *s, uint32_t h) {
    return h >= 1 && h <= NVS_MAX_HANDLES && s->h[h - 1].open;
}

RADIO_TEXT void nvs_ram_close(nvs_store_t *s, uint32_t h) {
    if (hcheck(s, h)) s->h[h - 1].open = 0;
}

RADIO_TEXT int nvs_ram_commit(nvs_store_t *s, uint32_t h) {
    return hcheck(s, h) ? NVS_OK : NVS_ERR_INVALID_HANDLE;
}

static RADIO_TEXT nvs_entry_t *find(nvs_store_t *s, uint32_t h, const char *key) {
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (s->e[i].used && s->e[i].ns == s->h[h - 1].ns && name_eq(s->e[i].key, key)) return &s->e[i];
    return 0;
}

/* The entry to write for (h, key), or an error: validates the handle, the
 * access mode and the key, reuses an existing entry of the same name. A type
 * change on an existing key replaces it (IDF does the same). */
static RADIO_TEXT int slot_for_write(nvs_store_t *s, uint32_t h, const char *key, nvs_entry_t **out) {
    if (!hcheck(s, h)) return NVS_ERR_INVALID_HANDLE;
    if (!key) return NVS_ERR_INVALID_ARG;
    if (!s->h[h - 1].rw) return NVS_ERR_READ_ONLY;
    int n = name_len(key);
    if (n == 0) return NVS_ERR_INVALID_ARG;
    if (n > NVS_NAME_MAX) return NVS_ERR_KEY_TOO_LONG;
    nvs_entry_t *e = find(s, h, key);
    if (!e) {
        for (int i = 0; i < NVS_MAX_ENTRIES; i++) if (!s->e[i].used) { e = &s->e[i]; break; }
        if (!e) return NVS_ERR_NOT_ENOUGH_SPACE;
        e->used = 1; e->ns = s->h[h - 1].ns; e->type = T_NONE; e->len = 0;
        name_copy(e->key, key);
    } else if (e->type == T_BLOB && e->v.blob) {
        s->env.free(e->v.blob);
        e->v.blob = 0;
    }
    *out = e;
    return NVS_OK;
}

static RADIO_TEXT int slot_for_read(nvs_store_t *s, uint32_t h, const char *key, int type, nvs_entry_t **out) {
    if (!hcheck(s, h)) return NVS_ERR_INVALID_HANDLE;
    if (!key) return NVS_ERR_INVALID_ARG;
    if (name_len(key) > NVS_NAME_MAX) return NVS_ERR_KEY_TOO_LONG;
    nvs_entry_t *e = find(s, h, key);
    if (!e) return NVS_ERR_NOT_FOUND;
    if (e->type != type) return NVS_ERR_TYPE_MISMATCH;
    *out = e;
    return NVS_OK;
}

#define SETTER(name, ctype, tag, field)                                              \
    RADIO_TEXT int nvs_ram_set_##name(nvs_store_t *s, uint32_t h, const char *key, ctype v) { \
        nvs_entry_t *e; int r = slot_for_write(s, h, key, &e);                       \
        if (r) return r;                                                             \
        e->type = tag; e->len = sizeof(ctype); e->v.field = v;                       \
        return NVS_OK;                                                               \
    }
#define GETTER(name, ctype, tag, field)                                              \
    RADIO_TEXT int nvs_ram_get_##name(nvs_store_t *s, uint32_t h, const char *key, ctype *out) { \
        nvs_entry_t *e; int r = slot_for_read(s, h, key, tag, &e);                   \
        if (r) return r;                                                             \
        if (!out) return NVS_ERR_INVALID_ARG;                                        \
        *out = e->v.field;                                                           \
        return NVS_OK;                                                               \
    }

SETTER(i8, int8_t, T_I8, i8)    GETTER(i8, int8_t, T_I8, i8)
SETTER(u8, uint8_t, T_U8, u8)   GETTER(u8, uint8_t, T_U8, u8)
SETTER(u16, uint16_t, T_U16, u16) GETTER(u16, uint16_t, T_U16, u16)

RADIO_TEXT int nvs_ram_set_blob(nvs_store_t *s, uint32_t h, const char *key, const void *v, size_t len) {
    if (!v && len) return NVS_ERR_INVALID_ARG;
    nvs_entry_t *e; int r = slot_for_write(s, h, key, &e);
    if (r) return r;
    void *copy = 0;
    if (len) {
        copy = s->env.alloc((uint32_t)len);
        if (!copy) { if (e->type == T_NONE) e->used = 0; return NVS_ERR_NO_MEM; }
        for (size_t i = 0; i < len; i++) ((uint8_t *)copy)[i] = ((const uint8_t *)v)[i];
    }
    e->type = T_BLOB; e->len = (uint32_t)len; e->v.blob = copy;
    return NVS_OK;
}

RADIO_TEXT int nvs_ram_get_blob(nvs_store_t *s, uint32_t h, const char *key, void *out, size_t *len) {
    nvs_entry_t *e; int r = slot_for_read(s, h, key, T_BLOB, &e);
    if (r) return r;
    if (!len) return NVS_ERR_INVALID_ARG;
    if (!out) { *len = e->len; return NVS_OK; }
    if (*len < e->len) { *len = e->len; return NVS_ERR_INVALID_LENGTH; }
    for (uint32_t i = 0; i < e->len; i++) ((uint8_t *)out)[i] = ((uint8_t *)e->v.blob)[i];
    *len = e->len;
    return NVS_OK;
}

RADIO_TEXT int nvs_ram_erase_key(nvs_store_t *s, uint32_t h, const char *key) {
    if (!hcheck(s, h)) return NVS_ERR_INVALID_HANDLE;
    if (!s->h[h - 1].rw) return NVS_ERR_READ_ONLY;
    if (!key) return NVS_ERR_INVALID_ARG;
    nvs_entry_t *e = find(s, h, key);
    if (!e) return NVS_ERR_NOT_FOUND;
    if (e->type == T_BLOB && e->v.blob) s->env.free(e->v.blob);
    e->used = 0;
    return NVS_OK;
}
