#ifndef LUGALOS_FS_GEMTEXT_H
#define LUGALOS_FS_GEMTEXT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GEM_LINE_MAX    512
#define GEM_URL_MAX     128
#define GEM_LABEL_MAX   128

typedef enum {
    GEM_LINE_BLANK = 0,     /* Empty or whitespace-only line */
    GEM_LINE_TEXT,          /* Regular body paragraph text */
    GEM_LINE_LINK,          /* => url [label] */
    GEM_LINE_H1,            /* # Heading 1 */
    GEM_LINE_H2,            /* ## Heading 2 */
    GEM_LINE_H3,            /* ### Heading 3 */
    GEM_LINE_LIST,          /* * Unordered bullet item */
    GEM_LINE_QUOTE,         /* > Blockquote */
    GEM_LINE_PRE_TOGGLE,    /* ``` [alt text] */
    GEM_LINE_PRE_TEXT,      /* Raw verbatim line inside preformatted block */
} gem_line_type_t;

typedef struct {
    gem_line_type_t type;
    const char     *text;                 /* Pointer to text payload */
    char            url[GEM_URL_MAX];     /* For GEM_LINE_LINK: target URL or path */
    char            label[GEM_LABEL_MAX]; /* For GEM_LINE_LINK: link title/label */
    char            alt[GEM_LABEL_MAX];   /* For GEM_LINE_PRE_TOGGLE: alt text / language */
} gem_line_t;

/* Parse a single line of Gemtext.
 * `raw_line` is a null-terminated string containing one line. Trailing \r\n
 * is handled automatically.
 * `in_preformatted` tracks preformatted state across lines (caller initializes to false).
 * Fills `out` structure with line semantics and tokens.
 * Returns true if line was parsed successfully.
 */
bool gemtext_parse_line(const char *raw_line, bool *in_preformatted, gem_line_t *out);

/* Resolves a link target relative to a base URL or path.
 * If target is absolute (starts with '/' or has '://'), it is returned directly
 * (or joined to the scheme+host if target starts with '/' and base has '://').
 * If target is relative, it is resolved against the directory portion of base_url.
 * Returns true on success, false if output buffer too small.
 */
bool gemtext_resolve_url(const char *base_url, const char *target, char *out_url, size_t out_sz);

#endif /* LUGALOS_FS_GEMTEXT_H */
