#ifndef NET_GIT_H
#define NET_GIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(LUGALOS_HOST_TEST)
#include "lugalos_config.h"
#endif

/*
 * Freestanding Git client for LugalOS:
 *   - Loose objects (.git/objects/xx/yyyy...) + .git/index v2
 *   - Packfile v2 unpacker (including OFS_DELTA and REF_DELTA) and packer
 *   - Smart HTTP(S) and SSH (git-upload-pack / git-receive-pack) transports
 *
 * Uses dynamic allocation (malloc in host tests, palloc_pages_bulk on device)
 * so non-PSRAM microcontrollers incur zero static SRAM overhead.
 */

#define GIT_OK              0
#define GIT_ERR_INVAL      -1
#define GIT_ERR_IO         -2
#define GIT_ERR_NOMEM      -3
#define GIT_ERR_CORRUPT    -4
#define GIT_ERR_NOTFOUND   -5
#define GIT_ERR_NET        -6
#define GIT_ERR_PROTO      -7

#define GIT_OBJ_COMMIT     1
#define GIT_OBJ_TREE       2
#define GIT_OBJ_BLOB       3
#define GIT_OBJ_TAG        4
#define GIT_OBJ_OFS_DELTA  6
#define GIT_OBJ_REF_DELTA  7

#define GIT_SHA1_LEN       20u
#define GIT_HEX_LEN        40u

/* Hex helpers */
void git_sha1_to_hex(const uint8_t sha1[GIT_SHA1_LEN], char hex_out[GIT_HEX_LEN + 1u]);
bool git_hex_to_sha1(const char *hex, uint8_t sha1_out[GIT_SHA1_LEN]);

/* Computes Git object SHA-1: SHA1("<type> <len>\0" || data). */
void git_hash_object(int type, const void *data, size_t len,
                     uint8_t sha1_out[GIT_SHA1_LEN]);

/* Applies a Git binary delta (`delta`, `delta_len`) to `base` (`base_len`).
 * Allocates `*out` (caller frees with git_free) or writes into caller buffer. */
int git_apply_delta(const uint8_t *base, size_t base_len,
                    const uint8_t *delta, size_t delta_len,
                    uint8_t **out_buf, size_t *out_len);

/* Host/device memory helpers (used across git modules and host tests). */
void *git_alloc(size_t bytes);
void  git_free(void *ptr, size_t bytes);

/* Repository operations */
int git_init(const char *repo_dir);
int git_write_loose_obj(const char *repo_dir, int type,
                        const void *data, size_t len,
                        uint8_t sha1_out[GIT_SHA1_LEN]);
int git_read_loose_obj(const char *repo_dir, const uint8_t sha1[GIT_SHA1_LEN],
                       int *type_out, uint8_t **data_out, size_t *len_out);

/* Unpacks a PACK v2 buffer into `repo_dir/.git/objects/`. */
int git_unpack_packfile(const char *repo_dir, const uint8_t *pack, size_t pack_len);

/* Packs `n_objs` loose objects from `repo_dir` into a newly allocated PACK v2 buffer. */
int git_build_packfile(const char *repo_dir, const uint8_t (*sha1s)[GIT_SHA1_LEN],
                       size_t n_objs, uint8_t **pack_out, size_t *pack_len);

/* Index v2 operations */
typedef struct {
    uint32_t mode;
    uint32_t size;
    uint8_t  sha1[GIT_SHA1_LEN];
    char     path[256];
} git_index_entry_t;

int git_index_read(const char *repo_dir, git_index_entry_t **entries_out, size_t *count_out);
int git_index_write(const char *repo_dir, const git_index_entry_t *entries, size_t count);

/* High-level porcelain */
int git_add_file(const char *repo_dir, const char *rel_path);
int git_commit(const char *repo_dir, const char *author, const char *message,
               uint8_t commit_sha1_out[GIT_SHA1_LEN]);
int git_checkout_tree(const char *repo_dir, const uint8_t tree_sha1[GIT_SHA1_LEN]);
int git_checkout_ref(const char *repo_dir, const char *ref_or_hex);

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_GIT_CLIENT)
void cmd_git(const char *args);
#endif

#endif /* NET_GIT_H */
