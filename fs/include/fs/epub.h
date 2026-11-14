#ifndef LUGALOS_FS_EPUB_H
#define LUGALOS_FS_EPUB_H

#include <stdbool.h>
#include <stdint.h>

/* Freestanding on-device EPUB (ZIP + DEFLATE + OPF/NCX + XHTML) reader.
 * Zero libc heap allocations; caller supplies PSRAM or host scratch. */

#define EPUB_MAX_SPINE 128
#define EPUB_MAX_PATH  160
#define EPUB_MAX_TITLE 96

typedef int (*epub_read_fn)(void *ctx, uint32_t off, void *dst, uint32_t len);

typedef enum {
    EPUB_OK = 0,
    EPUB_ERR_IO = -1,
    EPUB_ERR_FORMAT = -2,
    EPUB_ERR_DRM = -3,
    EPUB_ERR_NOMEM = -4,
    EPUB_ERR_NOTFOUND = -5,
} epub_err_t;

typedef struct {
    char     href[EPUB_MAX_PATH];
    char     title[EPUB_MAX_TITLE];
    uint32_t zip_local_off;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t crc32;
    uint16_t method;
} epub_spine_item_t;

typedef struct {
    epub_read_fn      read_fn;
    void             *read_ctx;
    uint32_t          file_size;
    uint32_t          cd_off;
    uint32_t          cd_size;
    uint16_t          cd_entries;

    char              title[EPUB_MAX_TITLE];
    char              author[EPUB_MAX_TITLE];
    char              opf_dir[EPUB_MAX_PATH];

    epub_spine_item_t spine[EPUB_MAX_SPINE];
    int               spine_count;
} epub_book_t;

/* Parse ZIP directory, DRM check, container.xml, OPF spine, and NCX/NAV TOC.
 * `scratch` (recommended >= 256 KB in PSRAM) is used temporarily during open. */
epub_err_t epub_open(epub_book_t *book, epub_read_fn read_fn, void *ctx,
                     uint32_t file_size, uint8_t *scratch, uint32_t scratch_cap);

/* Extract and convert spine item `chapter_idx` from XHTML into UTF-8 Markdown. */
epub_err_t epub_load_chapter_md(const epub_book_t *book, int chapter_idx,
                                uint8_t *scratch, uint32_t scratch_cap,
                                char *md_out, uint32_t md_cap, uint32_t *out_len);

/* Resolve `rel_href` (stripping any #fragment) relative to `from_chapter`
 * and return the matching spine index, or -1 if not found. */
int epub_find_spine_href(const epub_book_t *book, int from_chapter, const char *rel_href);

/* Standalone XHTML -> UTF-8 Markdown stream converter. */
uint32_t epub_xhtml_to_md(const uint8_t *xhtml, uint32_t xhtml_len,
                          char *md_out, uint32_t md_cap);

#endif /* LUGALOS_FS_EPUB_H */
