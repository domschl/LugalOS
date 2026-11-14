/* fs/epub.c — Freestanding on-device EPUB (ZIP + DEFLATE + OPF/NCX + XHTML) reader.
 *
 * Parses ZIP central directories, inflates entries via kernel/zlib.c, checks
 * for DRM (encryption.xml), resolves OPF spine + NCX/NAV titles, and converts
 * chapter XHTML streams to UTF-8 restricted Markdown.
 */

#if !defined(LUGALOS_HOST_TEST)
#include "lugalos_config.h"
#endif
#include "fs/epub.h"
#include "kernel/zlib.h"
#include <string.h>

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_PSRAM_BYTES) || defined(CONFIG_BOARD_VISIONFIVE2)

static inline uint16_t le16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static inline uint32_t le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static bool epub_read(const epub_book_t *b, uint32_t off, void *dst, uint32_t len) {
    if (!b || !b->read_fn) return false;
    if (off > b->file_size || len > b->file_size - off) return false;
    if (len == 0) return true;
    return b->read_fn(b->read_ctx, off, dst, len) == 0;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ci_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = ascii_lower(a[i]);
        char cb = ascii_lower(b[i]);
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

static const char *ci_strstr(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    if (hay_len < nlen) return NULL;
    for (size_t i = 0; i <= hay_len - nlen; i++) {
        if (ci_strncmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

static const char *find_xml_tag(const char *xml, size_t len, const char *tag) {
    size_t tlen = strlen(tag);
    size_t pos = 0;
    while (pos < len) {
        const char *p = ci_strstr(xml + pos, len - pos, tag);
        if (!p) return NULL;
        size_t off = (size_t)(p - xml);
        if (off + tlen < len) {
            char next = xml[off + tlen];
            if (next == ' ' || next == '\t' || next == '\n' || next == '\r' ||
                next == '>' || next == '/') {
                return p;
            }
        }
        pos = off + tlen;
    }
    return NULL;
}

typedef struct {
    uint32_t local_off;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t crc32;
    uint16_t method;
} zip_entry_t;

static bool zip_find_eocd(epub_book_t *b) {
    if (b->file_size < 22u) return false;
    uint32_t max_scan = 65536u + 22u;
    if (max_scan > b->file_size) max_scan = b->file_size;
    uint32_t start = b->file_size - max_scan;

    uint8_t buf[512];
    uint32_t pos = b->file_size - 22u;
    for (;;) {
        uint32_t chunk_start = (pos > start + sizeof(buf) - 22u)
                               ? (pos - (uint32_t)(sizeof(buf) - 22u))
                               : start;
        uint32_t chunk_len = pos + 22u - chunk_start;
        if (chunk_len > sizeof(buf)) chunk_len = sizeof(buf);
        if (!epub_read(b, chunk_start, buf, chunk_len)) return false;

        for (int i = (int)chunk_len - 22; i >= 0; i--) {
            if (buf[i] == 0x50u && buf[i + 1] == 0x4Bu &&
                buf[i + 2] == 0x05u && buf[i + 3] == 0x06u) {
                uint16_t clen = le16(buf + i + 20);
                if (chunk_start + (uint32_t)i + 22u + clen == b->file_size) {
                    b->cd_entries = le16(buf + i + 10);
                    b->cd_size = le32(buf + i + 12);
                    b->cd_off = le32(buf + i + 16);
                    if (b->cd_off > b->file_size || b->cd_size > b->file_size - b->cd_off) {
                        return false;
                    }
                    return true;
                }
            }
        }
        if (chunk_start == start) break;
        pos = chunk_start;
    }
    return false;
}

static bool zip_find_entry(const epub_book_t *b, const char *target, zip_entry_t *out) {
    size_t tlen = strlen(target);
    uint32_t pos = b->cd_off;
    uint32_t end = b->cd_off + b->cd_size;
    uint8_t hdr[46];
    char name[256];

    for (uint16_t i = 0; i < b->cd_entries && pos + 46u <= end; i++) {
        if (!epub_read(b, pos, hdr, 46)) return false;
        if (le32(hdr) != 0x02014B50u) return false;

        uint16_t method = le16(hdr + 10);
        uint32_t crc = le32(hdr + 16);
        uint32_t csz = le32(hdr + 20);
        uint32_t usz = le32(hdr + 24);
        uint16_t nlen = le16(hdr + 28);
        uint16_t elen = le16(hdr + 30);
        uint16_t clen = le16(hdr + 32);
        uint32_t loff = le32(hdr + 42);
        pos += 46u;

        if (pos + nlen + elen + clen > end) return false;
        if (nlen == tlen && nlen < sizeof(name)) {
            if (!epub_read(b, pos, name, nlen)) return false;
            name[nlen] = '\0';
            if (strcmp(name, target) == 0) {
                out->local_off = loff;
                out->comp_size = csz;
                out->uncomp_size = usz;
                out->crc32 = crc;
                out->method = method;
                return true;
            }
        }
        pos += (uint32_t)nlen + elen + clen;
    }
    return false;
}

static epub_err_t zip_extract_entry(const epub_book_t *b, const zip_entry_t *e,
                                    uint8_t *scratch, uint32_t scratch_cap,
                                    uint32_t *out_off, uint32_t *out_len) {
    uint8_t lhdr[30];
    if (!epub_read(b, e->local_off, lhdr, 30)) return EPUB_ERR_IO;
    if (le32(lhdr) != 0x04034B50u) return EPUB_ERR_FORMAT;

    uint16_t nlen = le16(lhdr + 26);
    uint16_t elen = le16(lhdr + 28);
    uint32_t payload_off = e->local_off + 30u + nlen + elen;
    if (payload_off > b->file_size || e->comp_size > b->file_size - payload_off) {
        return EPUB_ERR_FORMAT;
    }

    if (e->method == 0) {
        if (e->uncomp_size > scratch_cap) return EPUB_ERR_NOMEM;
        if (!epub_read(b, payload_off, scratch, e->uncomp_size)) return EPUB_ERR_IO;
        if (zlib_crc32(0u, scratch, e->uncomp_size) != e->crc32) return EPUB_ERR_FORMAT;
        *out_off = 0;
        *out_len = e->uncomp_size;
        return EPUB_OK;
    }

    if (e->method == 8) {
        /* Place compressed bytes at the tail of scratch, inflate into the front. */
        if (e->uncomp_size + e->comp_size > scratch_cap) return EPUB_ERR_NOMEM;
        uint32_t comp_off = scratch_cap - e->comp_size;
        if (!epub_read(b, payload_off, scratch + comp_off, e->comp_size)) return EPUB_ERR_IO;

        size_t dec_len = 0, consumed = 0;
        int rc = zlib_inflate_raw(scratch + comp_off, e->comp_size,
                                  scratch, e->uncomp_size, &dec_len, &consumed);
        if (rc != ZLIB_OK || dec_len != e->uncomp_size) return EPUB_ERR_FORMAT;
        if (zlib_crc32(0u, scratch, dec_len) != e->crc32) return EPUB_ERR_FORMAT;
        *out_off = 0;
        *out_len = (uint32_t)dec_len;
        return EPUB_OK;
    }

    return EPUB_ERR_FORMAT;
}

/* Resolve base_dir + rel_href, stripping #fragment and normalizing '..' segments. */
static void resolve_href(const char *base_dir, const char *rel, char *out, size_t out_cap) {
    char tmp[EPUB_MAX_PATH * 2];
    size_t pos = 0;
    if (base_dir && base_dir[0]) {
        size_t bl = strlen(base_dir);
        if (bl >= sizeof(tmp)) bl = sizeof(tmp) - 1;
        memcpy(tmp, base_dir, bl);
        pos = bl;
        if (pos > 0 && tmp[pos - 1] != '/' && pos + 1 < sizeof(tmp)) tmp[pos++] = '/';
    }
    for (size_t i = 0; rel[i] && rel[i] != '#' && pos + 1 < sizeof(tmp); i++) {
        tmp[pos++] = rel[i];
    }
    tmp[pos] = '\0';

    /* Normalize ./ and ../ */
    const char *segs[24];
    size_t seg_lens[24];
    int nsegs = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - s);
        if (sl == 1 && s[0] == '.') continue;
        if (sl == 2 && s[0] == '.' && s[1] == '.') {
            if (nsegs > 0) nsegs--;
            continue;
        }
        if (nsegs < 24) {
            segs[nsegs] = s;
            seg_lens[nsegs] = sl;
            nsegs++;
        }
    }

    size_t op = 0;
    for (int i = 0; i < nsegs; i++) {
        if (i > 0 && op + 1 < out_cap) out[op++] = '/';
        size_t copy = seg_lens[i];
        if (op + copy >= out_cap) copy = out_cap - 1 - op;
        memcpy(out + op, segs[i], copy);
        op += copy;
    }
    out[op] = '\0';
}

static bool extract_attr(const char *tag, size_t tag_len, const char *attr,
                         char *out, size_t out_cap) {
    size_t alen = strlen(attr);
    for (size_t i = 0; i + alen + 2 < tag_len; i++) {
        if (i > 0 && tag[i - 1] != ' ' && tag[i - 1] != '\t' &&
            tag[i - 1] != '\n' && tag[i - 1] != '\r') {
            continue;
        }
        if (ci_strncmp(tag + i, attr, alen) != 0) continue;
        size_t p = i + alen;
        while (p < tag_len && (tag[p] == ' ' || tag[p] == '\t')) p++;
        if (p >= tag_len || tag[p] != '=') continue;
        p++;
        while (p < tag_len && (tag[p] == ' ' || tag[p] == '\t')) p++;
        if (p >= tag_len || (tag[p] != '"' && tag[p] != '\'')) continue;
        char q = tag[p++];
        size_t op = 0;
        while (p < tag_len && tag[p] != q) {
            if (op + 1 < out_cap) out[op++] = tag[p];
            p++;
        }
        out[op] = '\0';
        return true;
    }
    return false;
}

static uint32_t utf8_encode_cp(uint32_t cp, char *out) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static uint32_t decode_entity(const char *s, size_t rem, size_t *consumed, char *out) {
    size_t semi = 0;
    while (semi < rem && semi < 12u && s[semi] != ';') semi++;
    if (semi == 0 || semi >= rem || s[semi] != ';') {
        *consumed = 1;
        out[0] = '&';
        return 1;
    }
    *consumed = semi + 1;
    if (s[1] == '#') {
        uint32_t cp = 0;
        if (semi > 2 && (s[2] == 'x' || s[2] == 'X')) {
            for (size_t i = 3; i < semi; i++) {
                char c = s[i];
                uint32_t d = 0;
                if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
                else { out[0] = ' '; return 1; }
                cp = (cp << 4) | d;
            }
        } else {
            for (size_t i = 2; i < semi; i++) {
                if (s[i] < '0' || s[i] > '9') { out[0] = ' '; return 1; }
                cp = cp * 10u + (uint32_t)(s[i] - '0');
            }
        }
        return utf8_encode_cp(cp ? cp : ' ', out);
    }

    struct { const char *name; uint32_t cp; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
        { "apos", '\'' }, { "nbsp", ' ' }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
        { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
        { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 },
        { "copy", 0x00A9 }, { "reg", 0x00AE }, { "trade", 0x2122 },
        { "auml", 0x00E4 }, { "ouml", 0x00F6 }, { "uuml", 0x00FC },
        { "Auml", 0x00C4 }, { "Ouml", 0x00D6 }, { "Uuml", 0x00DC },
        { "szlig", 0x00DF }, { "eacute", 0x00E9 }, { "egrave", 0x00E8 },
    };
    size_t nlen = semi - 1;
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        if (strlen(ents[i].name) == nlen && memcmp(s + 1, ents[i].name, nlen) == 0) {
            return utf8_encode_cp(ents[i].cp, out);
        }
    }
    out[0] = ' ';
    return 1;
}

static void extract_xml_text(const char *xml, size_t xml_len, const char *open_tag,
                             char *out, size_t out_cap) {
    out[0] = '\0';
    const char *p = find_xml_tag(xml, xml_len, open_tag);
    if (!p) return;
    size_t off = (size_t)(p - xml);
    const char *gt = memchr(p, '>', xml_len - off);
    if (!gt) return;
    const char *start = gt + 1;
    size_t rem = xml_len - (size_t)(start - xml);
    const char *lt = memchr(start, '<', rem);
    if (!lt) return;

    size_t raw_len = (size_t)(lt - start);
    size_t op = 0;
    for (size_t i = 0; i < raw_len && op + 5 < out_cap; ) {
        if (start[i] == '&') {
            size_t c = 0;
            char enc[4];
            uint32_t el = decode_entity(start + i, raw_len - i, &c, enc);
            memcpy(out + op, enc, el);
            op += el;
            i += c;
        } else if (start[i] == '\r' || start[i] == '\n' || start[i] == '\t') {
            if (op > 0 && out[op - 1] != ' ') out[op++] = ' ';
            i++;
        } else {
            out[op++] = start[i++];
        }
    }
    while (op > 0 && out[op - 1] == ' ') op--;
    out[op] = '\0';
}

typedef struct {
    char id[48];
    char href[EPUB_MAX_PATH];
} opf_manifest_item_t;

static void dir_of_path(const char *path, char *dir, size_t dir_cap) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen >= dir_cap) dlen = dir_cap - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
    } else if (dir_cap > 0) {
        dir[0] = '\0';
    }
}

static void parse_ncx_titles(epub_book_t *book, const char *ncx_dir, const char *ncx, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        const char *np = find_xml_tag(ncx + pos, len - pos, "<navPoint");
        if (!np) break;
        size_t np_off = (size_t)(np - ncx);
        const char *np_end = ci_strstr(np, len - np_off, "</navPoint>");
        size_t block_len = np_end ? (size_t)(np_end - np) : (len - np_off);
        if (block_len > 9u) {
            const char *child = find_xml_tag(np + 9u, block_len - 9u, "<navPoint");
            if (child && (size_t)(child - np) < block_len) {
                block_len = (size_t)(child - np);
            }
        }

        char label[EPUB_MAX_TITLE];
        extract_xml_text(np, block_len, "<text", label, sizeof(label));

        const char *cp = find_xml_tag(np, block_len, "<content");
        if (cp && label[0]) {
            const char *gt = memchr(cp, '>', (size_t)((np + block_len) - cp));
            if (gt) {
                char src[EPUB_MAX_PATH];
                if (extract_attr(cp, (size_t)(gt - cp), "src", src, sizeof(src))) {
                    char resolved[EPUB_MAX_PATH];
                    resolve_href(ncx_dir, src, resolved, sizeof(resolved));
                    for (int i = 0; i < book->spine_count; i++) {
                        if (strcmp(book->spine[i].href, resolved) == 0 &&
                            !book->spine[i].title[0]) {
                            strncpy(book->spine[i].title, label, EPUB_MAX_TITLE - 1);
                            book->spine[i].title[EPUB_MAX_TITLE - 1] = '\0';
                        }
                    }
                }
            }
        }
        pos = np_off + 9u;
    }
}

epub_err_t epub_open(epub_book_t *book, epub_read_fn read_fn, void *ctx,
                     uint32_t file_size, uint8_t *scratch, uint32_t scratch_cap) {
    if (!book || !read_fn || !scratch || scratch_cap < 16384u) return EPUB_ERR_NOMEM;
    memset(book, 0, sizeof(*book));
    book->read_fn = read_fn;
    book->read_ctx = ctx;
    book->file_size = file_size;

    if (!zip_find_eocd(book)) return EPUB_ERR_FORMAT;

    /* Check for DRM encryption.xml */
    zip_entry_t enc_e;
    if (zip_find_entry(book, "META-INF/encryption.xml", &enc_e)) {
        uint32_t off = 0, len = 0;
        if (zip_extract_entry(book, &enc_e, scratch, scratch_cap, &off, &len) == EPUB_OK) {
            if (ci_strstr((const char *)(scratch + off), len, "EncryptedData")) {
                return EPUB_ERR_DRM;
            }
        }
    }

    /* Parse META-INF/container.xml */
    zip_entry_t cont_e;
    if (!zip_find_entry(book, "META-INF/container.xml", &cont_e)) return EPUB_ERR_FORMAT;
    uint32_t cont_off = 0, cont_len = 0;
    epub_err_t err = zip_extract_entry(book, &cont_e, scratch, scratch_cap, &cont_off, &cont_len);
    if (err != EPUB_OK) return err;

    const char *cont_xml = (const char *)(scratch + cont_off);
    const char *rf = find_xml_tag(cont_xml, cont_len, "<rootfile");
    if (!rf) return EPUB_ERR_FORMAT;
    const char *rf_gt = memchr(rf, '>', cont_len - (size_t)(rf - cont_xml));
    if (!rf_gt) return EPUB_ERR_FORMAT;

    char opf_path[EPUB_MAX_PATH];
    if (!extract_attr(rf, (size_t)(rf_gt - rf), "full-path", opf_path, sizeof(opf_path))) {
        return EPUB_ERR_FORMAT;
    }

    const char *slash = NULL;
    for (const char *p = opf_path; *p; p++) {
        if (*p == '/') slash = p;
    }
    if (slash) {
        size_t dlen = (size_t)(slash - opf_path);
        if (dlen >= sizeof(book->opf_dir)) dlen = sizeof(book->opf_dir) - 1;
        memcpy(book->opf_dir, opf_path, dlen);
        book->opf_dir[dlen] = '\0';
    }

    /* Extract OPF file into scratch */
    zip_entry_t opf_e;
    if (!zip_find_entry(book, opf_path, &opf_e)) return EPUB_ERR_FORMAT;
    uint32_t opf_off = 0, opf_len = 0;
    err = zip_extract_entry(book, &opf_e, scratch, scratch_cap, &opf_off, &opf_len);
    if (err != EPUB_OK) return err;

    const char *opf = (const char *)(scratch + opf_off);
    extract_xml_text(opf, opf_len, "<dc:title", book->title, sizeof(book->title));
    extract_xml_text(opf, opf_len, "<dc:creator", book->author, sizeof(book->author));

    /* Build temporary manifest table after OPF in scratch */
    uint32_t man_off = (opf_len + 7u) & ~7u;
    if (man_off >= scratch_cap) return EPUB_ERR_NOMEM;
    opf_manifest_item_t *man = (opf_manifest_item_t *)(scratch + man_off);
    int max_man = (int)((scratch_cap - man_off) / sizeof(opf_manifest_item_t));
    int man_count = 0;
    char ncx_href[EPUB_MAX_PATH] = {0};

    size_t pos = 0;
    while (pos < opf_len && man_count < max_man) {
        const char *it = ci_strstr(opf + pos, opf_len - pos, "<item");
        if (!it) break;
        size_t it_off = (size_t)(it - opf);
        if (it_off + 5u < opf_len && (opf[it_off + 5] == ' ' || opf[it_off + 5] == '\t' ||
                                      opf[it_off + 5] == '\n' || opf[it_off + 5] == '\r')) {
            const char *gt = memchr(it, '>', opf_len - it_off);
            if (!gt) break;
            size_t tlen = (size_t)(gt - it);
            char id[48], href[EPUB_MAX_PATH], mtype[64];
            if (extract_attr(it, tlen, "id", id, sizeof(id)) &&
                extract_attr(it, tlen, "href", href, sizeof(href))) {
                strncpy(man[man_count].id, id, sizeof(man[man_count].id) - 1);
                man[man_count].id[sizeof(man[man_count].id) - 1] = '\0';
                resolve_href(book->opf_dir, href, man[man_count].href, EPUB_MAX_PATH);
                if (!ncx_href[0] &&
                    extract_attr(it, tlen, "media-type", mtype, sizeof(mtype)) &&
                    strcmp(mtype, "application/x-dtbncx+xml") == 0) {
                    strncpy(ncx_href, man[man_count].href, sizeof(ncx_href) - 1);
                }
                man_count++;
            }
            pos = (size_t)(gt - opf) + 1u;
        } else {
            pos = it_off + 5u;
        }
    }

    /* Parse <spine> <itemref idref="..."> */
    const char *sp = find_xml_tag(opf, opf_len, "<spine");
    if (!sp) return EPUB_ERR_FORMAT;
    pos = (size_t)(sp - opf);
    while (pos < opf_len && book->spine_count < EPUB_MAX_SPINE) {
        const char *ir = find_xml_tag(opf + pos, opf_len - pos, "<itemref");
        if (!ir) break;
        size_t ir_off = (size_t)(ir - opf);
        const char *gt = memchr(ir, '>', opf_len - ir_off);
        if (!gt) break;
        char idref[48];
        if (extract_attr(ir, (size_t)(gt - ir), "idref", idref, sizeof(idref))) {
            for (int m = 0; m < man_count; m++) {
                if (strcmp(man[m].id, idref) == 0) {
                    zip_entry_t ze;
                    if (zip_find_entry(book, man[m].href, &ze)) {
                        epub_spine_item_t *si = &book->spine[book->spine_count++];
                        strncpy(si->href, man[m].href, EPUB_MAX_PATH - 1);
                        si->zip_local_off = ze.local_off;
                        si->comp_size = ze.comp_size;
                        si->uncomp_size = ze.uncomp_size;
                        si->crc32 = ze.crc32;
                        si->method = ze.method;
                    }
                    break;
                }
            }
        }
        pos = (size_t)(gt - opf) + 1u;
    }

    if (book->spine_count == 0) return EPUB_ERR_FORMAT;

    /* Optional NCX chapter titles */
    if (ncx_href[0]) {
        zip_entry_t ncx_e;
        if (zip_find_entry(book, ncx_href, &ncx_e)) {
            uint32_t noff = 0, nlen = 0;
            if (zip_extract_entry(book, &ncx_e, scratch, scratch_cap, &noff, &nlen) == EPUB_OK) {
                char ncx_dir[EPUB_MAX_PATH];
                dir_of_path(ncx_href, ncx_dir, sizeof(ncx_dir));
                parse_ncx_titles(book, ncx_dir, (const char *)(scratch + noff), nlen);
            }
        }
    }

    return EPUB_OK;
}

typedef struct {
    char    *out;
    uint32_t cap;
    uint32_t len;
    int      nl_run;
    bool     need_space;
} md_writer_t;

static void md_putc(md_writer_t *w, char c) {
    if (w->len + 1u >= w->cap) return;
    if (c == '\n') {
        if (w->nl_run >= 2) return;
        w->out[w->len++] = '\n';
        w->nl_run++;
        w->need_space = false;
        return;
    }
    w->nl_run = 0;
    w->out[w->len++] = c;
}

static void md_puts(md_writer_t *w, const char *s) {
    for (size_t i = 0; s[i]; i++) md_putc(w, s[i]);
}

static void md_flush_space(md_writer_t *w) {
    if (w->need_space && w->nl_run == 0 && w->len > 0) {
        md_putc(w, ' ');
    }
    w->need_space = false;
}

static void md_ensure_nl(md_writer_t *w, int count) {
    w->need_space = false;
    while (w->nl_run < count && w->len > 0 && w->len + 1u < w->cap) {
        md_putc(w, '\n');
    }
}

uint32_t epub_xhtml_to_md(const uint8_t *xhtml, uint32_t xhtml_len,
                          char *md_out, uint32_t md_cap) {
    if (!xhtml || !md_out || md_cap == 0) return 0;
    md_writer_t w = { .out = md_out, .cap = md_cap, .nl_run = 2 };

    const char *s = (const char *)xhtml;
    uint32_t i = 0;
    bool skip_tag_content = false;
    char skip_end_tag[16] = {0};
    bool in_pre = false;
    bool in_quote = false;
    char link_href[EPUB_MAX_PATH] = {0};
    bool in_link = false;

    while (i < xhtml_len) {
        if (s[i] == '<') {
            uint32_t tstart = i + 1u;
            uint32_t tend = tstart;
            while (tend < xhtml_len && s[tend] != '>') tend++;
            if (tend >= xhtml_len) break;

            const char *tag = s + tstart;
            size_t tlen = tend - tstart;
            i = tend + 1u;

            if (tlen == 0 || tag[0] == '!' || tag[0] == '?') continue;

            bool closing = (tag[0] == '/');
            if (closing) { tag++; tlen--; }

            size_t nlen = 0;
            while (nlen < tlen && tag[nlen] != ' ' && tag[nlen] != '\t' &&
                   tag[nlen] != '\n' && tag[nlen] != '\r' && tag[nlen] != '/') {
                nlen++;
            }
            if (nlen == 0 || nlen >= 16) continue;
            char tname[16];
            for (size_t k = 0; k < nlen; k++) tname[k] = ascii_lower(tag[k]);
            tname[nlen] = '\0';

            if (skip_tag_content) {
                if (closing && strcmp(tname, skip_end_tag) == 0) {
                    skip_tag_content = false;
                }
                continue;
            }

            if (!closing && (strcmp(tname, "head") == 0 || strcmp(tname, "style") == 0 ||
                             strcmp(tname, "script") == 0)) {
                skip_tag_content = true;
                strncpy(skip_end_tag, tname, sizeof(skip_end_tag) - 1);
                continue;
            }

            if (strcmp(tname, "h1") == 0 || strcmp(tname, "h2") == 0 ||
                strcmp(tname, "h3") == 0 || strcmp(tname, "h4") == 0) {
                if (!closing) {
                    md_ensure_nl(&w, 2);
                    int lvl = tname[1] - '0';
                    for (int h = 0; h < lvl; h++) md_putc(&w, '#');
                    md_putc(&w, ' ');
                } else {
                    md_ensure_nl(&w, 2);
                }
            } else if (strcmp(tname, "p") == 0 || strcmp(tname, "div") == 0) {
                md_ensure_nl(&w, 2);
            } else if (strcmp(tname, "br") == 0) {
                md_putc(&w, '\n');
            } else if (strcmp(tname, "hr") == 0) {
                md_ensure_nl(&w, 2);
                md_puts(&w, "---");
                md_ensure_nl(&w, 2);
            } else if (strcmp(tname, "blockquote") == 0) {
                in_quote = !closing;
                md_ensure_nl(&w, 2);
                if (in_quote) md_puts(&w, "> ");
            } else if (strcmp(tname, "li") == 0) {
                if (!closing) {
                    md_ensure_nl(&w, 1);
                    md_puts(&w, "- ");
                } else {
                    md_ensure_nl(&w, 1);
                }
            } else if (strcmp(tname, "pre") == 0) {
                md_ensure_nl(&w, 2);
                md_puts(&w, "```");
                md_putc(&w, '\n');
                in_pre = !closing;
            } else if (strcmp(tname, "em") == 0 || strcmp(tname, "i") == 0) {
                md_flush_space(&w);
                md_putc(&w, '*');
            } else if (strcmp(tname, "strong") == 0 || strcmp(tname, "b") == 0) {
                md_flush_space(&w);
                md_puts(&w, "**");
            } else if (strcmp(tname, "code") == 0 && !in_pre) {
                md_flush_space(&w);
                md_putc(&w, '`');
            } else if (strcmp(tname, "a") == 0) {
                if (!closing) {
                    if (extract_attr(tag, tlen, "href", link_href, sizeof(link_href)) &&
                        link_href[0] && link_href[0] != '#') {
                        md_flush_space(&w);
                        md_putc(&w, '[');
                        in_link = true;
                    }
                } else if (in_link) {
                    md_puts(&w, "](");
                    md_puts(&w, link_href);
                    md_putc(&w, ')');
                    in_link = false;
                }
            } else if (!closing && strcmp(tname, "img") == 0) {
                char alt[96];
                if (extract_attr(tag, tlen, "alt", alt, sizeof(alt)) && alt[0]) {
                    md_flush_space(&w);
                    md_puts(&w, "*[Image: ");
                    md_puts(&w, alt);
                    md_puts(&w, "]*");
                }
            }
            continue;
        }

        if (skip_tag_content) {
            i++;
            continue;
        }

        if (s[i] == '&') {
            size_t c = 0;
            char enc[4];
            uint32_t el = decode_entity(s + i, xhtml_len - i, &c, enc);
            md_flush_space(&w);
            for (uint32_t k = 0; k < el; k++) md_putc(&w, enc[k]);
            i += (uint32_t)c;
            continue;
        }

        char ch = s[i++];
        if (in_pre) {
            if (ch != '\r') md_putc(&w, ch);
            continue;
        }

        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (w.nl_run == 0 && w.len > 0) w.need_space = true;
        } else {
            md_flush_space(&w);
            if (in_quote && w.nl_run > 0) md_puts(&w, "> ");
            md_putc(&w, ch);
        }
    }

    while (w.len > 0 && w.out[w.len - 1] == '\n') w.len--;
    if (w.len + 1u < w.cap) w.out[w.len++] = '\n';
    w.out[w.len] = '\0';
    return w.len;
}

epub_err_t epub_load_chapter_md(const epub_book_t *book, int chapter_idx,
                                uint8_t *scratch, uint32_t scratch_cap,
                                char *md_out, uint32_t md_cap, uint32_t *out_len) {
    if (!book || chapter_idx < 0 || chapter_idx >= book->spine_count) {
        return EPUB_ERR_NOTFOUND;
    }
    const epub_spine_item_t *si = &book->spine[chapter_idx];
    zip_entry_t ze = {
        .local_off = si->zip_local_off,
        .comp_size = si->comp_size,
        .uncomp_size = si->uncomp_size,
        .crc32 = si->crc32,
        .method = si->method,
    };
    uint32_t off = 0, len = 0;
    epub_err_t err = zip_extract_entry(book, &ze, scratch, scratch_cap, &off, &len);
    if (err != EPUB_OK) return err;

    uint32_t mlen = epub_xhtml_to_md(scratch + off, len, md_out, md_cap);
    if (out_len) *out_len = mlen;
    return EPUB_OK;
}

static void percent_decode(const char *src, char *dst, size_t dst_cap) {
    size_t op = 0;
    for (size_t i = 0; src[i] && op + 1 < dst_cap; ) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char h1 = src[i + 1], h2 = src[i + 2];
            int d1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') :
                     (h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) :
                     (h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1;
            int d2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') :
                     (h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) :
                     (h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1;
            if (d1 >= 0 && d2 >= 0) {
                dst[op++] = (char)((d1 << 4) | d2);
                i += 3;
                continue;
            }
        }
        dst[op++] = src[i++];
    }
    dst[op] = '\0';
}

int epub_find_spine_href(const epub_book_t *book, int from_chapter, const char *rel_href) {
    if (!book || !rel_href || !rel_href[0]) return -1;
    char base_dir[EPUB_MAX_PATH];
    if (from_chapter >= 0 && from_chapter < book->spine_count) {
        dir_of_path(book->spine[from_chapter].href, base_dir, sizeof(base_dir));
    } else {
        strncpy(base_dir, book->opf_dir, sizeof(base_dir) - 1);
        base_dir[sizeof(base_dir) - 1] = '\0';
    }
    char resolved[EPUB_MAX_PATH];
    resolve_href(base_dir, rel_href, resolved, sizeof(resolved));
    if (!resolved[0]) return -1;

    for (int i = 0; i < book->spine_count; i++) {
        if (strcmp(book->spine[i].href, resolved) == 0) return i;
    }

    char decoded[EPUB_MAX_PATH];
    percent_decode(resolved, decoded, sizeof(decoded));
    if (strcmp(decoded, resolved) != 0) {
        for (int i = 0; i < book->spine_count; i++) {
            if (strcmp(book->spine[i].href, decoded) == 0) return i;
        }
    }
    return -1;
}

#endif /* LUGALOS_HOST_TEST || CONFIG_PSRAM_BYTES || CONFIG_BOARD_VISIONFIVE2 */
