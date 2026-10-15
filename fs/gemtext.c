/* fs/gemtext.c — LugalOS Gemtext (text/gemini) Line Parser & URL Resolver.
 *
 * Implements an allocation-free, streaming Gemtext line-oriented parser and
 * standard URL resolver for both local (file://, /sd0/...) and network
 * (spartan://, gemini://) navigation.
 *
 * Freestanding C, zero FPU, bounded string buffers.
 */

#include "fs/gemtext.h"
#include <string.h>

static inline bool gem_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool gem_is_blank(char c) {
    return c == ' ' || c == '\t';
}

bool gemtext_parse_line(const char *raw_line, bool *in_preformatted, gem_line_t *out) {
    if (!raw_line || !in_preformatted || !out) return false;

    out->type = GEM_LINE_TEXT;
    out->text = "";
    out->url[0] = '\0';
    out->label[0] = '\0';
    out->alt[0] = '\0';

    /* If we are inside a preformatted block */
    if (*in_preformatted) {
        if (raw_line[0] == '`' && raw_line[1] == '`' && raw_line[2] == '`') {
            *in_preformatted = false;
            out->type = GEM_LINE_PRE_TOGGLE;
            out->text = "";
            return true;
        }
        out->type = GEM_LINE_PRE_TEXT;
        out->text = raw_line;
        return true;
    }

    /* Check for preformatted block start */
    if (raw_line[0] == '`' && raw_line[1] == '`' && raw_line[2] == '`') {
        *in_preformatted = true;
        out->type = GEM_LINE_PRE_TOGGLE;

        const char *p = raw_line + 3;
        while (gem_is_blank(*p)) p++;

        size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && i < GEM_LABEL_MAX - 1) {
            out->alt[i++] = *p++;
        }
        while (i > 0 && gem_is_space(out->alt[i - 1])) {
            i--;
        }
        out->alt[i] = '\0';
        out->text = out->alt;
        return true;
    }

    /* Check for hyperlink line (=>) */
    if (raw_line[0] == '=' && raw_line[1] == '>') {
        out->type = GEM_LINE_LINK;
        const char *p = raw_line + 2;
        while (gem_is_blank(*p)) p++;

        /* Extract URL (first whitespace-delimited token) */
        size_t u = 0;
        while (*p && !gem_is_space(*p) && u < GEM_URL_MAX - 1) {
            out->url[u++] = *p++;
        }
        out->url[u] = '\0';

        /* Extract optional human-readable label */
        while (gem_is_blank(*p)) p++;
        size_t l = 0;
        while (*p && *p != '\r' && *p != '\n' && l < GEM_LABEL_MAX - 1) {
            out->label[l++] = *p++;
        }
        while (l > 0 && gem_is_space(out->label[l - 1])) {
            l--;
        }
        out->label[l] = '\0';

        /* If label was omitted, default to URL */
        if (out->label[0] == '\0') {
            strncpy(out->label, out->url, GEM_LABEL_MAX - 1);
            out->label[GEM_LABEL_MAX - 1] = '\0';
        }
        out->text = out->label;
        return true;
    }

    /* Check for headings (#, ##, ###).
     * Gemtext specifies 1 to 3 '#' characters followed by a space.
     * Four or more '#' characters are treated as regular text. */
    if (raw_line[0] == '#') {
        int hashes = 0;
        while (raw_line[hashes] == '#') hashes++;
        if (hashes <= 3 && (raw_line[hashes] == ' ' || raw_line[hashes] == '\t' ||
                            raw_line[hashes] == '\0' || raw_line[hashes] == '\r' || raw_line[hashes] == '\n')) {
            if (hashes == 3) out->type = GEM_LINE_H3;
            else if (hashes == 2) out->type = GEM_LINE_H2;
            else out->type = GEM_LINE_H1;

            const char *p = raw_line + hashes;
            while (gem_is_blank(*p)) p++;
            out->text = p;
            return true;
        }
    }

    /* Check for unordered list item (*) */
    if (raw_line[0] == '*' && (raw_line[1] == ' ' || raw_line[1] == '\t')) {
        out->type = GEM_LINE_LIST;
        const char *p = raw_line + 1;
        while (gem_is_blank(*p)) p++;
        out->text = p;
        return true;
    }

    /* Check for blockquote (>) */
    if (raw_line[0] == '>') {
        out->type = GEM_LINE_QUOTE;
        const char *p = raw_line + 1;
        while (gem_is_blank(*p)) p++;
        out->text = p;
        return true;
    }

    /* Check for empty or whitespace-only line */
    const char *p = raw_line;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '\0') {
        out->type = GEM_LINE_BLANK;
        out->text = "";
        return true;
    }

    /* Regular text paragraph */
    out->type = GEM_LINE_TEXT;
    out->text = raw_line;
    return true;
}

bool gemtext_resolve_url(const char *base_url, const char *target, char *out_url, size_t out_sz) {
    if (!target || !out_url || out_sz == 0) return false;
    out_url[0] = '\0';

    /* Fully qualified URL with scheme (e.g. "spartan://...", "file://...") */
    if (strstr(target, "://") != NULL) {
        if (strlen(target) >= out_sz) return false;
        strncpy(out_url, target, out_sz - 1);
        out_url[out_sz - 1] = '\0';
        return true;
    }

    /* Absolute path on the host or local volume */
    if (target[0] == '/') {
        const char *scheme_sep = base_url ? strstr(base_url, "://") : NULL;
        if (scheme_sep) {
            /* Find end of host[:port] component in base URL */
            const char *host_start = scheme_sep + 3;
            const char *slash = strchr(host_start, '/');
            size_t prefix_len = slash ? (size_t)(slash - base_url) : strlen(base_url);
            if (prefix_len + strlen(target) >= out_sz) return false;
            memcpy(out_url, base_url, prefix_len);
            out_url[prefix_len] = '\0';
            strncat(out_url, target, out_sz - prefix_len - 1);
            return true;
        } else {
            /* Local filesystem absolute path */
            if (strlen(target) >= out_sz) return false;
            strncpy(out_url, target, out_sz - 1);
            out_url[out_sz - 1] = '\0';
            return true;
        }
    }

    /* Relative target: resolve against base_url directory */
    if (!base_url || base_url[0] == '\0') {
        if (strlen(target) >= out_sz) return false;
        strncpy(out_url, target, out_sz - 1);
        out_url[out_sz - 1] = '\0';
        return true;
    }

    const char *last_slash = NULL;
    for (const char *s = base_url; *s; s++) {
        if (*s == '/') last_slash = s;
    }

    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - base_url + 1);
        if (dir_len + strlen(target) >= out_sz) return false;
        memcpy(out_url, base_url, dir_len);
        out_url[dir_len] = '\0';
        strncat(out_url, target, out_sz - dir_len - 1);
    } else {
        if (strlen(target) >= out_sz) return false;
        strncpy(out_url, target, out_sz - 1);
        out_url[out_sz - 1] = '\0';
    }

    return true;
}
