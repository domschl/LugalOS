/* fs/include/fs/markdown.h — LugalOS Restricted Markdown Header.
 *
 * Freestanding C, zero FPU, bounded memory.
 * Defines block line structures, inline span iterator, table cell parser,
 * heading slug generator, and URL/anchor resolver.
 */

#ifndef LUGALOS_FS_MARKDOWN_H
#define LUGALOS_FS_MARKDOWN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MD_LINE_MAX         512
#define MD_URL_MAX          128
#define MD_LABEL_MAX        128
#define MD_LANG_MAX         32
#define MD_FOOTNOTE_ID_MAX  16
#define MD_TABLE_MAX_COLS   8
#define MD_CELL_MAX         128

typedef enum {
    MD_LINE_BLANK = 0,      /* Empty or whitespace-only */
    MD_LINE_TEXT,           /* Standard paragraph text */
    MD_LINE_H1,             /* # H1 */
    MD_LINE_H2,             /* ## H2 */
    MD_LINE_H3,             /* ### H3 */
    MD_LINE_H4,             /* #### H4 */
    MD_LINE_CODE_TOGGLE,    /* ``` [lang] */
    MD_LINE_CODE_TEXT,      /* Verbatim code line inside ``` fence */
    MD_LINE_QUOTE,          /* > Blockquote text */
    MD_LINE_ALERT,          /* > [!NOTE], > [!TIP], > [!WARNING], etc. */
    MD_LINE_ULIST,          /* - item, * item, + item */
    MD_LINE_OLIST,          /* 1. item, 2. item */
    MD_LINE_HRULE,          /* ---, ***, ___ */
    MD_LINE_TABLE_HDR,      /* | Header 1 | Header 2 | */
    MD_LINE_TABLE_SEP,      /* |:---|:---:|---:| */
    MD_LINE_TABLE_ROW,      /* | Cell 1 | Cell 2 | */
    MD_LINE_FOOTNOTE_DEF,   /* [^id]: Footnote text */
    MD_LINE_MATH_TOGGLE,    /* $$ or \[ toggle */
    MD_LINE_MATH_TEXT,      /* Verbatim math line inside $$ block */
    MD_LINE_TOC,            /* [TOC] */
    MD_LINE_GEM_LINK,       /* Legacy => url [label] */
} md_line_type_t;

typedef enum {
    MD_ALERT_NONE = 0,
    MD_ALERT_NOTE,
    MD_ALERT_TIP,
    MD_ALERT_IMPORTANT,
    MD_ALERT_WARNING,
    MD_ALERT_CAUTION,
} md_alert_type_t;

typedef struct {
    md_line_type_t  type;
    const char     *text;                         /* Pointer to text payload */
    uint8_t         indent_level;                 /* 0..2 for lists */
    md_alert_type_t alert_type;                   /* For MD_LINE_ALERT */
    int             order_num;                    /* For MD_LINE_OLIST (e.g. 1) */
    char            lang[MD_LANG_MAX];            /* For MD_LINE_CODE_TOGGLE */
    char            footnote_id[MD_FOOTNOTE_ID_MAX]; /* For MD_LINE_FOOTNOTE_DEF */
    char            url[MD_URL_MAX];              /* For MD_LINE_GEM_LINK */
    char            label[MD_LABEL_MAX];          /* For MD_LINE_GEM_LINK */
} md_line_t;

/* Inline span style bitmask */
#define MD_STYLE_NONE       0
#define MD_STYLE_BOLD       (1 << 0)
#define MD_STYLE_ITALIC     (1 << 1)
#define MD_STYLE_CODE       (1 << 2)
#define MD_STYLE_LINK       (1 << 3)
#define MD_STYLE_IMAGE      (1 << 4)
#define MD_STYLE_FOOTNOTE   (1 << 5)
#define MD_STYLE_MATH       (1 << 6)

typedef struct {
    const char     *text;                         /* Pointer to span text in line or buf */
    size_t          len;                          /* Byte length of text */
    uint16_t        style;                        /* Bitmask of MD_STYLE_* */
    char            link_url[MD_URL_MAX];         /* Target for MD_STYLE_LINK / IMAGE */
    char            link_text[MD_LABEL_MAX];      /* Label/alt for LINK / IMAGE */
    char            footnote_id[MD_FOOTNOTE_ID_MAX]; /* For MD_STYLE_FOOTNOTE */
    char            unescaped[MD_LINE_MAX];       /* Storage when text requires entity decoding */
} md_span_t;

typedef struct {
    const char     *src;
    size_t          pos;
    size_t          len;
    uint16_t        active_style;
} md_span_iter_t;

/* --- Parser Entry Points --- */

/* Parse a single block-level line.
 * `in_code_fence` and `in_math_block` track multiline state across lines.
 */
bool md_parse_line(const char *raw_line,
                   bool *in_code_fence,
                   bool *in_math_block,
                   md_line_t *out);

/* Split a table row into cells. Returns number of columns found (up to MD_TABLE_MAX_COLS). */
int md_parse_table_cells(const char *line, char cells[MD_TABLE_MAX_COLS][MD_CELL_MAX]);

/* Check if a line is a GFM table separator row (|---|:---:|). */
bool md_is_table_separator(const char *line);

/* Initialize inline span iterator for a line of text. */
void md_span_init(md_span_iter_t *iter, const char *line);

/* Yield next formatted inline span. Returns false when line is exhausted. */
bool md_span_next(md_span_iter_t *iter, md_span_t *span);

/* Clean markdown formatting and links from a heading title for display in Table of Contents. */
void md_heading_clean_title(const char *heading_text, char *out_title, size_t out_sz);

/* Generate a lowercase URL/anchor slug from a heading text.
 * e.g. "Quick Examples by Domain" -> "quick-examples-by-domain".
 */
void md_heading_slug(const char *heading_text, char *out_slug, size_t out_sz);

/* Resolves a link target or #anchor relative to base_url. */
bool md_resolve_url(const char *base_url, const char *target, char *out_url, size_t out_sz);

#endif /* LUGALOS_FS_MARKDOWN_H */
