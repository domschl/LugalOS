/* fs/markdown.c — LugalOS Restricted Markdown Line Parser & Span Tokenizer.
 *
 * Freestanding C, zero FPU, zero heap allocation.
 */

#include "fs/markdown.h"
#include <string.h>

static inline bool md_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool md_is_blank(char c) {
    return c == ' ' || c == '\t';
}

static inline bool md_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline char md_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static inline bool md_is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* --- Block Level Parser --- */

bool md_parse_line(const char *raw_line,
                   bool *in_code_fence,
                   bool *in_math_block,
                   md_line_t *out) {
    if (!raw_line || !in_code_fence || !in_math_block || !out) return false;

    memset(out, 0, sizeof(*out));
    out->type = MD_LINE_TEXT;
    out->text = "";

    /* 1. Handle active fenced code blocks */
    if (*in_code_fence) {
        const char *p = raw_line;
        while (md_is_blank(*p)) p++;
        if ((p[0] == '`' && p[1] == '`' && p[2] == '`') ||
            (p[0] == '~' && p[1] == '~' && p[2] == '~')) {
            *in_code_fence = false;
            out->type = MD_LINE_CODE_TOGGLE;
            return true;
        }
        out->type = MD_LINE_CODE_TEXT;
        out->text = raw_line;
        return true;
    }

    /* 2. Handle active display math blocks */
    if (*in_math_block) {
        const char *p = raw_line;
        while (md_is_blank(*p)) p++;
        if ((p[0] == '$' && p[1] == '$') || (p[0] == '\\' && p[1] == ']')) {
            *in_math_block = false;
            out->type = MD_LINE_MATH_TOGGLE;
            return true;
        }
        out->type = MD_LINE_MATH_TEXT;
        out->text = raw_line;
        return true;
    }

    /* Skip leading whitespace for indentation measurement */
    const char *p = raw_line;
    uint8_t indent = 0;
    while (*p == ' ' || *p == '\t') {
        if (*p == '\t') indent += 4;
        else indent++;
        p++;
    }

    /* Check for blank lines */
    if (*p == '\0' || *p == '\r' || *p == '\n') {
        out->type = MD_LINE_BLANK;
        return true;
    }

    /* 3. Check for fenced code block toggle (``` or ~~~) */
    if ((p[0] == '`' && p[1] == '`' && p[2] == '`') ||
        (p[0] == '~' && p[1] == '~' && p[2] == '~')) {
        *in_code_fence = true;
        out->type = MD_LINE_CODE_TOGGLE;
        p += 3;
        while (md_is_blank(*p)) p++;
        size_t l = 0;
        while (*p && !md_is_space(*p) && l < sizeof(out->lang) - 1) {
            out->lang[l++] = *p++;
        }
        out->lang[l] = '\0';
        out->text = out->lang;
        return true;
    }

    /* 4. Check for display math block toggle ($$ or \[) */
    if (p[0] == '$' && p[1] == '$') {
        /* Check if it closes on the same line: e.g. "$$ formula $$" */
        const char *close = strstr(p + 2, "$$");
        if (close && close != p + 2) {
            /* Single line math block */
            out->type = MD_LINE_MATH_TEXT;
            out->text = raw_line;
            return true;
        }
        *in_math_block = true;
        out->type = MD_LINE_MATH_TOGGLE;
        return true;
    }
    if (p[0] == '\\' && p[1] == '[') {
        const char *close = strstr(p + 2, "\\]");
        if (close && close != p + 2) {
            out->type = MD_LINE_MATH_TEXT;
            out->text = raw_line;
            return true;
        }
        *in_math_block = true;
        out->type = MD_LINE_MATH_TOGGLE;
        return true;
    }

    /* 5. Legacy Gemtext link (=>) for 100% .gmi compatibility */
    if (p[0] == '=' && p[1] == '>') {
        out->type = MD_LINE_GEM_LINK;
        p += 2;
        while (md_is_blank(*p)) p++;

        size_t u = 0;
        while (*p && !md_is_space(*p) && u < sizeof(out->url) - 1) {
            out->url[u++] = *p++;
        }
        out->url[u] = '\0';

        while (md_is_blank(*p)) p++;
        size_t l = 0;
        while (*p && *p != '\r' && *p != '\n' && l < sizeof(out->label) - 1) {
            out->label[l++] = *p++;
        }
        while (l > 0 && md_is_space(out->label[l - 1])) l--;
        out->label[l] = '\0';

        if (out->label[0] == '\0') {
            strncpy(out->label, out->url, sizeof(out->label) - 1);
        }
        out->text = out->label;
        return true;
    }

    /* 6. Headings (# H1 .. #### H4) */
    if (*p == '#') {
        int hashes = 0;
        while (p[hashes] == '#') hashes++;
        if (hashes >= 1 && hashes <= 4 && (p[hashes] == ' ' || p[hashes] == '\t')) {
            switch (hashes) {
                case 1: out->type = MD_LINE_H1; break;
                case 2: out->type = MD_LINE_H2; break;
                case 3: out->type = MD_LINE_H3; break;
                case 4: out->type = MD_LINE_H4; break;
            }
            p += hashes;
            while (md_is_blank(*p)) p++;
            out->text = p;
            return true;
        }
    }

    /* 7. Table of Contents marker ([TOC] or [[TOC]]) */
    if (strncmp(p, "[TOC]", 5) == 0 || strncmp(p, "[[TOC]]", 7) == 0) {
        out->type = MD_LINE_TOC;
        out->text = "Table of Contents";
        return true;
    }

    /* 8. Footnote definition ([^id]: text) */
    if (p[0] == '[' && p[1] == '^') {
        const char *rb = strchr(p + 2, ']');
        if (rb && rb[1] == ':') {
            size_t id_len = (size_t)(rb - (p + 2));
            if (id_len > 0 && id_len < sizeof(out->footnote_id)) {
                memcpy(out->footnote_id, p + 2, id_len);
                out->footnote_id[id_len] = '\0';
                p = rb + 2;
                while (md_is_blank(*p)) p++;
                out->type = MD_LINE_FOOTNOTE_DEF;
                out->text = p;
                return true;
            }
        }
    }

    /* 9. Horizontal rule (---, ***, ___ >= 3 chars) */
    if (*p == '-' || *p == '*' || *p == '_') {
        char ch = *p;
        int count = 0;
        const char *c = p;
        bool valid_hrule = true;
        while (*c && *c != '\r' && *c != '\n') {
            if (*c == ch) {
                count++;
            } else if (!md_is_blank(*c)) {
                valid_hrule = false;
                break;
            }
            c++;
        }
        if (valid_hrule && count >= 3) {
            out->type = MD_LINE_HRULE;
            return true;
        }
    }

    /* 10. Blockquote and Alerts (>) */
    if (*p == '>') {
        p++;
        while (md_is_blank(*p)) p++;

        /* Check GFM alert headers */
        if (strncmp(p, "[!NOTE]", 7) == 0) {
            out->type = MD_LINE_ALERT;
            out->alert_type = MD_ALERT_NOTE;
            p += 7;
        } else if (strncmp(p, "[!TIP]", 6) == 0) {
            out->type = MD_LINE_ALERT;
            out->alert_type = MD_ALERT_TIP;
            p += 6;
        } else if (strncmp(p, "[!IMPORTANT]", 12) == 0) {
            out->type = MD_LINE_ALERT;
            out->alert_type = MD_ALERT_IMPORTANT;
            p += 12;
        } else if (strncmp(p, "[!WARNING]", 10) == 0) {
            out->type = MD_LINE_ALERT;
            out->alert_type = MD_ALERT_WARNING;
            p += 10;
        } else if (strncmp(p, "[!CAUTION]", 10) == 0) {
            out->type = MD_LINE_ALERT;
            out->alert_type = MD_ALERT_CAUTION;
            p += 10;
        } else {
            out->type = MD_LINE_QUOTE;
        }
        while (md_is_blank(*p)) p++;
        out->text = p;
        return true;
    }

    /* 11. Unordered lists (-, *, + followed by space) */
    if ((*p == '-' || *p == '*' || *p == '+') && (p[1] == ' ' || p[1] == '\t')) {
        out->type = MD_LINE_ULIST;
        out->indent_level = indent >= 4 ? 2 : (indent >= 2 ? 1 : 0);
        p += 2;
        while (md_is_blank(*p)) p++;
        out->text = p;
        return true;
    }

    /* 12. Ordered lists (digit+ followed by . or ) and space) */
    if (md_is_digit(*p)) {
        const char *d = p;
        int num = 0;
        while (md_is_digit(*d)) {
            num = num * 10 + (*d - '0');
            d++;
        }
        if ((*d == '.' || *d == ')') && (d[1] == ' ' || d[1] == '\t')) {
            out->type = MD_LINE_OLIST;
            out->order_num = num;
            out->indent_level = indent >= 4 ? 2 : (indent >= 2 ? 1 : 0);
            p = d + 2;
            while (md_is_blank(*p)) p++;
            out->text = p;
            return true;
        }
    }

    /* 13. GFM Pipe Tables */
    if (*p == '|') {
        if (md_is_table_separator(raw_line)) {
            out->type = MD_LINE_TABLE_SEP;
            out->text = p;
            return true;
        }
        out->type = MD_LINE_TABLE_ROW;
        out->text = p;
        return true;
    }

    /* Default: regular paragraph text */
    out->type = MD_LINE_TEXT;
    out->text = p;
    return true;
}

/* --- Table Parsing --- */

bool md_is_table_separator(const char *line) {
    if (!line) return false;
    const char *p = line;
    while (md_is_blank(*p)) p++;
    if (*p != '|') return false;

    bool has_dash = false;
    while (*p && *p != '\r' && *p != '\n') {
        if (*p == '-') {
            has_dash = true;
        } else if (*p != '|' && *p != ':' && !md_is_blank(*p)) {
            return false;
        }
        p++;
    }
    return has_dash;
}

int md_parse_table_cells(const char *line, char cells[MD_TABLE_MAX_COLS][MD_CELL_MAX]) {
    if (!line || !cells) return 0;

    const char *p = line;
    while (md_is_blank(*p)) p++;
    if (*p == '|') p++;

    int col = 0;
    while (*p && *p != '\r' && *p != '\n' && col < MD_TABLE_MAX_COLS) {
        /* Extract single cell up to unescaped | */
        char buf[MD_CELL_MAX];
        size_t idx = 0;
        while (*p && *p != '\r' && *p != '\n') {
            if (*p == '\\' && p[1] == '|') {
                if (idx < sizeof(buf) - 1) buf[idx++] = '|';
                p += 2;
                continue;
            }
            if (*p == '|') {
                p++;
                break;
            }
            if (idx < sizeof(buf) - 1) buf[idx++] = *p;
            p++;
        }
        buf[idx] = '\0';

        /* Trim leading whitespace */
        const char *start = buf;
        while (md_is_blank(*start)) start++;

        /* Trim trailing whitespace */
        size_t len = strlen(start);
        while (len > 0 && md_is_blank(start[len - 1])) len--;

        if (len >= MD_CELL_MAX) len = MD_CELL_MAX - 1;
        memcpy(cells[col], start, len);
        cells[col][len] = '\0';
        col++;
    }
    return col;
}

void md_heading_clean_title(const char *heading_text, char *out_title, size_t out_sz) {
    if (!heading_text || !out_title || out_sz == 0) return;
    size_t o = 0;
    const char *p = heading_text;
    /* Trim leading heading hashes and spaces */
    while (*p == '#' || *p == ' ' || *p == '\t') p++;
    while (*p && *p != '\r' && *p != '\n' && o < out_sz - 1) {
        /* If link: [label](url) -> extract label only */
        if (*p == '[') {
            const char *rb = strchr(p + 1, ']');
            if (rb && rb[1] == '(') {
                const char *rp = strchr(rb + 2, ')');
                if (rp) {
                    p++; /* skip '[' */
                    while (p < rb && o < out_sz - 1) {
                        if (*p != '*' && *p != '_' && *p != '`') {
                            out_title[o++] = *p;
                        }
                        p++;
                    }
                    p = rp + 1; /* skip ')...' */
                    continue;
                }
            }
        }
        /* Skip markdown formatting delimiters **, *, _, ` */
        if (*p == '*' || *p == '_' || *p == '`') {
            p++;
            continue;
        }
        out_title[o++] = *p++;
    }
    /* Trim trailing spaces */
    while (o > 0 && (out_title[o - 1] == ' ' || out_title[o - 1] == '\t')) o--;
    out_title[o] = '\0';
}

/* --- Heading Slug Generator --- */

void md_heading_slug(const char *heading_text, char *out_slug, size_t out_sz) {
    if (!heading_text || !out_slug || out_sz == 0) return;

    char clean[MD_LINE_MAX];
    md_heading_clean_title(heading_text, clean, sizeof(clean));

    size_t o = 0;
    bool last_was_dash = false;
    const char *p = clean;

    /* Trim leading non-alnum */
    while (*p && !md_is_alnum(*p)) p++;

    while (*p && *p != '\r' && *p != '\n' && o < out_sz - 1) {
        char c = *p++;
        if (md_is_alnum(c)) {
            out_slug[o++] = md_tolower(c);
            last_was_dash = false;
        } else if (c == ' ' || c == '\t' || c == '-' || c == '_') {
            if (!last_was_dash && o > 0) {
                out_slug[o++] = '-';
                last_was_dash = true;
            }
        }
    }
    /* Trim trailing dash */
    if (o > 0 && out_slug[o - 1] == '-') o--;
    out_slug[o] = '\0';
}

/* --- URL / Anchor Resolver --- */

bool md_resolve_url(const char *base_url, const char *target, char *out_url, size_t out_sz) {
    if (!target || !out_url || out_sz == 0) return false;

    /* 1. Internal #anchor navigation */
    if (target[0] == '#') {
        if (!base_url || base_url[0] == '\0') {
            strncpy(out_url, target, out_sz - 1);
            out_url[out_sz - 1] = '\0';
            return true;
        }
        /* Copy base_url up to existing # if any */
        const char *hash = strchr(base_url, '#');
        size_t base_len = hash ? (size_t)(hash - base_url) : strlen(base_url);
        size_t targ_len = strlen(target);
        if (base_len + targ_len >= out_sz) return false;
        memcpy(out_url, base_url, base_len);
        memcpy(out_url + base_len, target, targ_len);
        out_url[base_len + targ_len] = '\0';
        return true;
    }

    /* 2. Absolute URL with scheme (e.g. spartan://, gemini://, http://) */
    if (strstr(target, "://") != NULL) {
        strncpy(out_url, target, out_sz - 1);
        out_url[out_sz - 1] = '\0';
        return true;
    }

    /* 3. Absolute path (/...) */
    if (target[0] == '/') {
        if (base_url && strstr(base_url, "://")) {
            const char *scheme_end = strstr(base_url, "://") + 3;
            const char *slash = strchr(scheme_end, '/');
            size_t host_len = slash ? (size_t)(slash - base_url) : strlen(base_url);
            size_t targ_len = strlen(target);
            if (host_len + targ_len >= out_sz) return false;
            memcpy(out_url, base_url, host_len);
            memcpy(out_url + host_len, target, targ_len);
            out_url[host_len + targ_len] = '\0';
            return true;
        }
        strncpy(out_url, target, out_sz - 1);
        out_url[out_sz - 1] = '\0';
        return true;
    }

    /* 4. Relative target */
    if (!base_url || base_url[0] == '\0') {
        strncpy(out_url, target, out_sz - 1);
        out_url[out_sz - 1] = '\0';
        return true;
    }

    /* Strip fragment from base_url before resolving path */
    const char *hash = strchr(base_url, '#');
    const char *last_slash = NULL;
    for (const char *s = base_url; *s && (!hash || s < hash); s++) {
        if (*s == '/') last_slash = s;
    }

    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - base_url) + 1;
        size_t targ_len = strlen(target);
        if (dir_len + targ_len >= out_sz) return false;
        memcpy(out_url, base_url, dir_len);
        memcpy(out_url + dir_len, target, targ_len);
        out_url[dir_len + targ_len] = '\0';
        return true;
    }

    strncpy(out_url, target, out_sz - 1);
    out_url[out_sz - 1] = '\0';
    return true;
}

/* --- Inline Span Iterator --- */

void md_span_init(md_span_iter_t *iter, const char *line) {
    if (!iter) return;
    iter->src = line ? line : "";
    iter->pos = 0;
    iter->len = strlen(iter->src);
    iter->active_style = MD_STYLE_NONE;
    /* Trim trailing \r\n */
    while (iter->len > 0 && (iter->src[iter->len - 1] == '\r' || iter->src[iter->len - 1] == '\n')) {
        iter->len--;
    }
}

bool md_span_next(md_span_iter_t *iter, md_span_t *span) {
    if (!iter || !span || iter->pos >= iter->len) return false;

    memset(span, 0, sizeof(*span));
    span->style = MD_STYLE_NONE;

    const char *s = iter->src;

    /* 1. Check for closing delimiters of currently active styles */
    if ((iter->active_style & (MD_STYLE_BOLD | MD_STYLE_ITALIC)) == (MD_STYLE_BOLD | MD_STYLE_ITALIC)) {
        if (iter->pos + 2 < iter->len &&
            ((s[iter->pos] == '*' && s[iter->pos+1] == '*' && s[iter->pos+2] == '*') ||
             (s[iter->pos] == '_' && s[iter->pos+1] == '_' && s[iter->pos+2] == '_'))) {
            iter->active_style &= ~(MD_STYLE_BOLD | MD_STYLE_ITALIC);
            iter->pos += 3;
            if (iter->pos >= iter->len) return false;
        }
    }
    if (iter->active_style & MD_STYLE_BOLD) {
        if (iter->pos + 1 < iter->len &&
            ((s[iter->pos] == '*' && s[iter->pos+1] == '*') ||
             (s[iter->pos] == '_' && s[iter->pos+1] == '_'))) {
            iter->active_style &= ~MD_STYLE_BOLD;
            iter->pos += 2;
            if (iter->pos >= iter->len) return false;
        }
    }
    if (iter->active_style & MD_STYLE_ITALIC) {
        if (iter->pos < iter->len && (s[iter->pos] == '*' || s[iter->pos] == '_')) {
            iter->active_style &= ~MD_STYLE_ITALIC;
            iter->pos += 1;
            if (iter->pos >= iter->len) return false;
        }
    }

    /* 2. Check for opening delimiters */
    if (!(iter->active_style & (MD_STYLE_BOLD | MD_STYLE_ITALIC)) && iter->pos + 2 < iter->len &&
        ((strncmp(s + iter->pos, "***", 3) == 0) || (strncmp(s + iter->pos, "___", 3) == 0))) {
        char delim[4] = { s[iter->pos], s[iter->pos], s[iter->pos], '\0' };
        const char *end = strstr(s + iter->pos + 3, delim);
        if (end && (size_t)(end - s) < iter->len) {
            iter->active_style |= (MD_STYLE_BOLD | MD_STYLE_ITALIC);
            iter->pos += 3;
        }
    } else if (!(iter->active_style & MD_STYLE_BOLD) && iter->pos + 1 < iter->len &&
        ((strncmp(s + iter->pos, "**", 2) == 0) || (strncmp(s + iter->pos, "__", 2) == 0))) {
        char delim[3] = { s[iter->pos], s[iter->pos], '\0' };
        const char *end = strstr(s + iter->pos + 2, delim);
        if (end && (size_t)(end - s) < iter->len) {
            iter->active_style |= MD_STYLE_BOLD;
            iter->pos += 2;
        }
    } else if (!(iter->active_style & MD_STYLE_ITALIC) && iter->pos < iter->len &&
               (s[iter->pos] == '*' || s[iter->pos] == '_')) {
        char delim = s[iter->pos];
        bool is_identifier_underscore = (delim == '_' && iter->pos > 0 && md_is_alnum(s[iter->pos - 1]));
        bool followed_by_space = (iter->pos + 1 < iter->len && md_is_space(s[iter->pos + 1]));
        if (!is_identifier_underscore && !followed_by_space) {
            const char *end = strchr(s + iter->pos + 1, delim);
            if (end && (size_t)(end - s) < iter->len && !md_is_space(*(end - 1))) {
                iter->active_style |= MD_STYLE_ITALIC;
                iter->pos += 1;
            }
        }
    }

    if (iter->pos >= iter->len) return false;
    size_t start = iter->pos;

    /* 3. Inline Code (`code`) */
    if (s[start] == '`') {
        const char *end = strchr(s + start + 1, '`');
        if (end && (size_t)(end - s) < iter->len) {
            span->style = iter->active_style | MD_STYLE_CODE;
            span->text = s + start + 1;
            span->len = (size_t)(end - (s + start + 1));
            iter->pos = (size_t)(end - s) + 1;
            return true;
        }
    }

    /* 4. Inline LaTeX Math ($math$) */
    if (s[start] == '$') {
        /* Check if not escaped */
        const char *end = strchr(s + start + 1, '$');
        if (end && (size_t)(end - s) < iter->len) {
            span->style = iter->active_style | MD_STYLE_MATH;
            span->text = s + start + 1;
            span->len = (size_t)(end - (s + start + 1));
            iter->pos = (size_t)(end - s) + 1;
            return true;
        }
    }

    /* 5. Inline Image (![alt](url)) */
    if (s[start] == '!' && start + 1 < iter->len && s[start + 1] == '[') {
        const char *rb = strchr(s + start + 2, ']');
        if (rb && rb[1] == '(') {
            const char *rp = strchr(rb + 2, ')');
            if (rp && (size_t)(rp - s) < iter->len) {
                span->style = iter->active_style | MD_STYLE_IMAGE;
                size_t alt_len = (size_t)(rb - (s + start + 2));
                if (alt_len >= sizeof(span->link_text)) alt_len = sizeof(span->link_text) - 1;
                memcpy(span->link_text, s + start + 2, alt_len);
                span->link_text[alt_len] = '\0';

                size_t url_len = (size_t)(rp - (rb + 2));
                if (url_len >= sizeof(span->link_url)) url_len = sizeof(span->link_url) - 1;
                memcpy(span->link_url, rb + 2, url_len);
                span->link_url[url_len] = '\0';

                span->text = span->link_text;
                span->len = alt_len;
                iter->pos = (size_t)(rp - s) + 1;
                return true;
            }
        }
    }

    /* 6. Footnote Reference ([^id]) */
    if (s[start] == '[' && start + 1 < iter->len && s[start + 1] == '^') {
        const char *rb = strchr(s + start + 2, ']');
        if (rb && (size_t)(rb - s) < iter->len && rb[1] != ':') {
            size_t id_len = (size_t)(rb - (s + start + 2));
            if (id_len > 0 && id_len < sizeof(span->footnote_id)) {
                span->style = iter->active_style | MD_STYLE_FOOTNOTE;
                memcpy(span->footnote_id, s + start + 2, id_len);
                span->footnote_id[id_len] = '\0';
                span->text = span->footnote_id;
                span->len = id_len;
                iter->pos = (size_t)(rb - s) + 1;
                return true;
            }
        }
    }

    /* 7. Inline Hyperlink ([label](url)) */
    if (s[start] == '[') {
        const char *rb = strchr(s + start + 1, ']');
        if (rb && rb[1] == '(') {
            const char *rp = strchr(rb + 2, ')');
            if (rp && (size_t)(rp - s) < iter->len) {
                span->style = iter->active_style | MD_STYLE_LINK;
                size_t lbl_len = (size_t)(rb - (s + start + 1));
                if (lbl_len >= sizeof(span->link_text)) lbl_len = sizeof(span->link_text) - 1;
                memcpy(span->link_text, s + start + 1, lbl_len);
                span->link_text[lbl_len] = '\0';

                /* Strip inner bold or italic from link text and apply to style */
                if (lbl_len >= 4 && span->link_text[0] == '*' && span->link_text[1] == '*' &&
                    span->link_text[lbl_len - 2] == '*' && span->link_text[lbl_len - 1] == '*') {
                    span->style |= MD_STYLE_BOLD;
                    memmove(span->link_text, span->link_text + 2, lbl_len - 4);
                    lbl_len -= 4;
                    span->link_text[lbl_len] = '\0';
                } else if (lbl_len >= 2 && span->link_text[0] == '*' && span->link_text[lbl_len - 1] == '*') {
                    span->style |= MD_STYLE_ITALIC;
                    memmove(span->link_text, span->link_text + 1, lbl_len - 2);
                    lbl_len -= 2;
                    span->link_text[lbl_len] = '\0';
                }

                size_t url_len = (size_t)(rp - (rb + 2));
                if (url_len >= sizeof(span->link_url)) url_len = sizeof(span->link_url) - 1;
                memcpy(span->link_url, rb + 2, url_len);
                span->link_url[url_len] = '\0';

                span->text = span->link_text;
                span->len = lbl_len;
                iter->pos = (size_t)(rp - s) + 1;
                return true;
            }
        }
    }

    /* 8. Plain text up to next special delimiter */
    size_t p = start;
    bool has_entities = false;
    while (p < iter->len) {
        /* Check for backslash escape */
        if (s[p] == '\\' && p + 1 < iter->len) {
            has_entities = true;
            p += 2;
            continue;
        }
        if (s[p] == '&') {
            has_entities = true;
        }
        /* Delimiter check at boundary */
        if (p > start) {
            if (s[p] == '`' || s[p] == '$' || s[p] == '[' ||
                (s[p] == '!' && p + 1 < iter->len && s[p+1] == '[')) {
                break;
            }
            if (s[p] == '*' || s[p] == '_') {
                /* Check if it is a closing delimiter for active style */
                if ((iter->active_style & MD_STYLE_BOLD) && p + 1 < iter->len && s[p] == s[p+1]) {
                    break;
                }
                if (iter->active_style & MD_STYLE_ITALIC) {
                    break;
                }
                /* Or an opening delimiter */
                if (p + 1 < iter->len && s[p] == s[p+1]) {
                    break;
                }
                if (!md_is_space(s[p+1])) {
                    break;
                }
            }
        }
        p++;
    }

    size_t raw_len = p - start;
    span->style = iter->active_style;
    if (has_entities) {
        /* Unescape and decode entities into span->unescaped */
        size_t o = 0;
        for (size_t i = start; i < p && o < sizeof(span->unescaped) - 1; ) {
            if (s[i] == '\\' && i + 1 < p) {
                span->unescaped[o++] = s[i + 1];
                i += 2;
                continue;
            }
            if (s[i] == '&') {
                if (i + 4 <= p && strncmp(s + i, "&amp;", 5) == 0) {
                    span->unescaped[o++] = '&'; i += 5; continue;
                } else if (i + 3 <= p && strncmp(s + i, "&lt;", 4) == 0) {
                    span->unescaped[o++] = '<'; i += 4; continue;
                } else if (i + 3 <= p && strncmp(s + i, "&gt;", 4) == 0) {
                    span->unescaped[o++] = '>'; i += 4; continue;
                } else if (i + 5 <= p && strncmp(s + i, "&quot;", 6) == 0) {
                    span->unescaped[o++] = '"'; i += 6; continue;
                }
            }
            span->unescaped[o++] = s[i++];
        }
        span->unescaped[o] = '\0';
        span->text = span->unescaped;
        span->len = o;
    } else {
        span->text = s + start;
        span->len = raw_len;
    }

    iter->pos = p;
    return true;
}
