/* drivers/reader_view.c — Ribbon Reader Window Engine with Proportional Typography
 *
 * Implements a dedicated long-form reading window for the horizontal window ribbon:
 * - Proportional serif typography (font_serif) with word-wrapping.
 * - Monospace fallback (font8x16) for preformatted blocks (``` ... ```).
 * - Touch hit-testing: tapping links navigates to targets.
 * - Smooth vertical scrolling & page panning with GT911 gesture support.
 * - Freestanding C, zero heap allocations, zero FPU in M-mode.
 */

#include "drivers/reader_view.h"
#include "drivers/font_serif.h"
#include "drivers/font8x16.h"
#include "drivers/canvas1.h"
#include "drivers/lcdterm_attr.h"
#include "drivers/lcdterm.h"
#include "drivers/ramscreen.h"
#include "drivers/screen.h"
#include "kernel/console.h"
#include "fs/gemtext.h"
#include "fs/vfs.h"
#include "net/spartan.h"
#include "kernel/vterm.h"
#include <string.h>

#define READER_LINKS_MAX   16
#define READER_STACK_MAX   8
#define READER_MARGIN_X    24
#define READER_LINE_H      18

typedef struct {
    int  y0, y1;         /* client area y bounds */
    int  num;            /* 1-based index (e.g. 1..9) */
    char url[128];
} reader_link_t;

typedef struct {
    bool          active;
    uint8_t       target_cid;
    int           win_w, win_h;
    char          path[128];
    char          title[64];
    int           scroll_y;
    int           total_h;
    reader_link_t links[READER_LINKS_MAX];
    int           link_count;
    char          history[READER_STACK_MAX][128];
    int           history_depth;
} reader_state_t;

static reader_state_t s_reader;

static bool resolve_doc_path(const char *topic, char *out_path, size_t out_sz) {
    if (!topic || !*topic) return false;

    if (strncmp(topic, "spartan://", 10) == 0 || strncmp(topic, "SPARTAN://", 10) == 0) {
        strncpy(out_path, topic, out_sz - 1);
        out_path[out_sz - 1] = '\0';
        return true;
    }

    if (topic[0] == '/' || topic[0] == '.') {
        int fd = vfs_open(topic, VFS_O_READ);
        if (fd >= 0) {
            vfs_close(fd);
            strncpy(out_path, topic, out_sz - 1);
            out_path[out_sz - 1] = '\0';
            return true;
        }
    }

    char clean[64];
    strncpy(clean, topic, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char *dot = NULL;
    for (char *p = clean; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (dot && (strcmp(dot, ".gmi") == 0 || strcmp(dot, ".txt") == 0)) {
        *dot = '\0';
    }

    /* 1. Try .gmi on /sd0/help */
    ksnprintf(out_path, out_sz, "/sd0/help/%s.gmi", clean);
    int fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    /* 2. Try .gmi on /flash0/help */
    ksnprintf(out_path, out_sz, "/flash0/help/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    /* 3. Try /sd0/pub/ */
    ksnprintf(out_path, out_sz, "/sd0/pub/%s.gmi", clean);
    fd = vfs_open(out_path, VFS_O_READ);
    if (fd >= 0) { vfs_close(fd); return true; }

    return false;
}

static bool reader_open_internal(const char *resource, bool push_history) {
    if (!resource || !*resource) return false;

    char resolved[128];
    if (!resolve_doc_path(resource, resolved, sizeof(resolved))) {
        strncpy(resolved, resource, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    if (push_history && s_reader.path[0] && s_reader.history_depth < READER_STACK_MAX) {
        strncpy(s_reader.history[s_reader.history_depth++], s_reader.path, sizeof(s_reader.history[0]) - 1);
    }

    bool is_spartan = (strncmp(resolved, "spartan://", 10) == 0 || strncmp(resolved, "SPARTAN://", 10) == 0);
    if (is_spartan) {
        char redir[128] = {0};
        char err[128] = {0};
        int rc = spartan_fetch(resolved, "/ram0/spartan.gmi", redir, sizeof(redir), err, sizeof(err));
        if (rc == SPARTAN_REDIRECT && redir[0]) {
            rc = spartan_fetch(redir, "/ram0/spartan.gmi", NULL, 0, err, sizeof(err));
        }
        if (rc != SPARTAN_OK) {
            return false;
        }
    }

    strncpy(s_reader.path, resolved, sizeof(s_reader.path) - 1);
    s_reader.path[sizeof(s_reader.path) - 1] = '\0';

    /* Set default title from filename */
    const char *slash = NULL;
    for (const char *sp = resolved; *sp; sp++) {
        if (*sp == '/') slash = sp;
    }
    const char *name = slash ? slash + 1 : resolved;
    strncpy(s_reader.title, name, sizeof(s_reader.title) - 1);
    s_reader.title[sizeof(s_reader.title) - 1] = '\0';

    s_reader.scroll_y = 0;
    s_reader.total_h = 0;
    s_reader.link_count = 0;
    s_reader.active = true;
    return true;
}

void reader_init(void) {
    memset(&s_reader, 0, sizeof(s_reader));
}

bool reader_open(const char *resource) {
    return reader_open_internal(resource, true);
}

void reader_close(void) {
    s_reader.active = false;
    s_reader.path[0] = '\0';
}

static canvas1_t *get_reader_canvas(void) {
#if defined(LCDTERM_PRESENT)
    canvas1_t *c = lcdterm_get_canvas_backing(s_reader.target_cid);
    if (c) return c;
#endif
    return ramscreen_get_canvas_backing(s_reader.target_cid);
}

void reader_set_target(uint8_t cid, int w, int h) {
    s_reader.target_cid = cid;
    if (w > 0) s_reader.win_w = w;
    if (h > 0) s_reader.win_h = h;
}

void reader_update(void) {
    if (!s_reader.active) return;
    canvas1_t *c = get_reader_canvas();
    if (!c) return;
    int w = s_reader.win_w > 0 ? s_reader.win_w : (int)c->w;
    int h = s_reader.win_h > 0 ? s_reader.win_h : (int)c->h;
    reader_draw(c, w, h);

    /* Update title on ribbon canvas window */
    const char *title = reader_get_title();
    uint8_t req[SCREEN_CTITLE_MAX + 2];
    req[0] = 'T';
    req[1] = s_reader.target_cid;
    uint32_t tl = (uint32_t)strlen(title);
    if (tl > sizeof(req) - 3) tl = sizeof(req) - 3;
    memcpy(req + 2, title, tl);
    req[2 + tl] = '\0';
    uint8_t reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 3 + tl, reply);
#if defined(LCDTERM_PRESENT)
    lcdterm_repaint();
#endif
}

void reader_scroll(int delta_y) {
    if (!s_reader.active) return;
    s_reader.scroll_y += delta_y;
    if (s_reader.scroll_y < 0) s_reader.scroll_y = 0;
    int max_scroll = s_reader.total_h - 120;
    if (max_scroll < 0) max_scroll = 0;
    if (s_reader.scroll_y > max_scroll) s_reader.scroll_y = max_scroll;
    reader_update();
}

bool reader_back(void) {
    if (s_reader.history_depth > 0) {
        s_reader.history_depth--;
        char prev[128];
        strncpy(prev, s_reader.history[s_reader.history_depth], sizeof(prev) - 1);
        prev[sizeof(prev) - 1] = '\0';
        bool ok = reader_open_internal(prev, false);
        if (ok) reader_update();
        return ok;
    }
    return false;
}

bool reader_tap(int client_x, int client_y) {
    (void)client_x;
    if (!s_reader.active) return false;

    for (int i = 0; i < s_reader.link_count; i++) {
        if (client_y >= s_reader.links[i].y0 && client_y <= s_reader.links[i].y1) {
            char target[128];
            strncpy(target, s_reader.links[i].url, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';

            char resolved[128];
            bool ok = false;
            if (gemtext_resolve_url(s_reader.path, target, resolved, sizeof(resolved)) ||
                resolve_doc_path(target, resolved, sizeof(resolved))) {
                ok = reader_open(resolved);
            } else {
                ok = reader_open(target);
            }
            if (ok) reader_update();
            return ok;
        }
    }
    return false;
}

bool reader_follow_link_num(int num) {
    if (!s_reader.active) return false;
    for (int i = 0; i < s_reader.link_count; i++) {
        if (s_reader.links[i].num == num) {
            char target[128];
            strncpy(target, s_reader.links[i].url, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';

            char resolved[128];
            bool ok = false;
            if (gemtext_resolve_url(s_reader.path, target, resolved, sizeof(resolved)) ||
                resolve_doc_path(target, resolved, sizeof(resolved))) {
                ok = reader_open(resolved);
            } else {
                ok = reader_open(target);
            }
            if (ok) reader_update();
            return ok;
        }
    }
    return false;
}

void reader_feed_key(int c) {
    if (!s_reader.active) return;
    static int esc_state = 0;
    static int esc_arg = 0;

    int win_h = s_reader.win_h > 0 ? s_reader.win_h : 480;
    int page_dy = win_h - 40;
    if (page_dy < 40) page_dy = 40;

    if (esc_state == 0) {
        if (c == 0x1b) {
            esc_state = 1;
            return;
        }
        if (c == 0x08 || c == '?' || c == 'h') {
            /* Emacs Ctrl-H or '?' / 'h' -> open Reader Help */
            reader_open("/sd0/help/reader.gmi");
            reader_update();
            return;
        }
        if (c == 'q' || c == 0x03) {
            /* Close reader window */
            uint8_t req[1] = { 'C' }, reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 1, reply) && reply[0] == 0) {
                vterm_set_active(reply[1]);
            }
            reader_close();
            console_set_active_win_type(1 /* RIBBON_WIN_TERM */);
            return;
        }
        if (c == 'b' || c == 0x7f) {
            /* Back in history */
            reader_back();
            return;
        }
        if (c == ' ' || c == 0x16 /* Ctrl-V */) {
            /* Page down */
            reader_scroll(page_dy);
            return;
        }
        if (c == 'j' || c == 0x0e /* Ctrl-N */ || c == '\r' || c == '\n') {
            /* Line down */
            reader_scroll(32);
            return;
        }
        if (c == 'k' || c == 0x10 /* Ctrl-P */) {
            /* Line up */
            reader_scroll(-32);
            return;
        }
        if (c == 'd' || c == 0x04 /* Ctrl-D */) {
            /* Half page down */
            reader_scroll(page_dy / 2);
            return;
        }
        if (c == 'u' || c == 0x15 /* Ctrl-U */) {
            /* Half page up */
            reader_scroll(-page_dy / 2);
            return;
        }
        if (c == 'g') {
            /* Top of document */
            s_reader.scroll_y = 0;
            reader_update();
            return;
        }
        if (c == 'G') {
            /* Bottom of document */
            int max_scroll = s_reader.total_h - 120;
            s_reader.scroll_y = max_scroll > 0 ? max_scroll : 0;
            reader_update();
            return;
        }
        if (c == 'r' || c == 0x12 /* Ctrl-R */) {
            /* Reload / Redraw */
            reader_update();
            return;
        }
        if (c >= '1' && c <= '9') {
            /* Follow visible link */
            reader_follow_link_num(c - '0');
            return;
        }
        return;
    } else if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_arg = 0;
            return;
        }
        if (c == 'v' || c == 'V') {
            /* Alt-V: Page up (Emacs) */
            reader_scroll(-page_dy);
            esc_state = 0;
            return;
        }
        esc_state = 0;
        return;
    } else if (esc_state == 2) {
        if (c == 'A') {
            /* Up Arrow */
            reader_scroll(-32);
            esc_state = 0;
            return;
        }
        if (c == 'B') {
            /* Down Arrow */
            reader_scroll(32);
            esc_state = 0;
            return;
        }
        if (c == 'C') {
            /* Right Arrow */
            reader_scroll(page_dy);
            esc_state = 0;
            return;
        }
        if (c == 'D') {
            /* Left Arrow */
            reader_back();
            esc_state = 0;
            return;
        }
        if (c >= '0' && c <= '9') {
            esc_arg = esc_arg * 10 + (c - '0');
            esc_state = 3;
            return;
        }
        esc_state = 0;
        return;
    } else if (esc_state == 3) {
        if (c == '~') {
            if (esc_arg == 5) {
                /* Page Up */
                reader_scroll(-page_dy);
            } else if (esc_arg == 6) {
                /* Page Down */
                reader_scroll(page_dy);
            } else if (esc_arg == 1 || esc_arg == 7) {
                /* Home */
                s_reader.scroll_y = 0;
                reader_update();
            } else if (esc_arg == 4 || esc_arg == 8) {
                /* End */
                int max_scroll = s_reader.total_h - 120;
                s_reader.scroll_y = max_scroll > 0 ? max_scroll : 0;
                reader_update();
            }
            esc_state = 0;
            return;
        }
        if (c >= '0' && c <= '9') {
            esc_arg = esc_arg * 10 + (c - '0');
            return;
        }
        esc_state = 0;
        return;
    }
}

const char *reader_get_title(void) {
    return s_reader.title[0] ? s_reader.title : "Reader";
}

const char *reader_get_path(void) {
    return s_reader.path;
}

bool reader_is_active(void) {
    return s_reader.active;
}

/* Word wrapping and proportional text rendering */
static int render_wrapped_serif_line(const canvas1_t *c, int x0, int max_w,
                                    const char *text, bool bold, int line_h,
                                    int scroll_y, int win_h, int *out_doc_y) {
    int cur_x = x0;
    int cur_y = *out_doc_y;
    const char *p = text;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *word_start = p;
        while (*p && *p != ' ') p++;
        uint32_t word_len = (uint32_t)(p - word_start);
        unsigned word_w = font_serif_text_width(word_start, word_len);

        if (cur_x + (int)word_w > x0 + max_w && cur_x > x0) {
            cur_x = x0;
            cur_y += line_h;
        }

        int screen_y = cur_y - scroll_y;
        if (screen_y + line_h >= 0 && screen_y < win_h) {
            canvas1_serif_text(c, cur_x, screen_y, word_start, word_len, bold);
        }
        cur_x += (int)word_w + (int)font_serif_char_width(' ');
    }
    cur_y += line_h;
    *out_doc_y = cur_y;
    return cur_y;
}

void reader_draw(const canvas1_t *c, int w, int h) {
    if (!c || w <= 0 || h <= 0) return;

    /* Fill background */
    canvas1_fill(c, 0, 0, w - 1, h - 1, CANVAS1_WHITE);

    if (!s_reader.active || !s_reader.path[0]) {
        canvas1_serif_text(c, READER_MARGIN_X, 32, "No document loaded. Use 'reader <file.gmi>' to open.", 52, false);
        return;
    }

    const char *file_to_open = s_reader.path;
    if (strncmp(s_reader.path, "spartan://", 10) == 0 || strncmp(s_reader.path, "SPARTAN://", 10) == 0) {
        file_to_open = "/ram0/spartan.gmi";
    }

    int fd = vfs_open(file_to_open, VFS_O_READ);
    if (fd < 0) {
        char err[128];
        ksnprintf(err, sizeof(err), "Cannot open document: %s", file_to_open);
        canvas1_serif_text(c, READER_MARGIN_X, 32, err, strlen(err), false);
        return;
    }

    int margin_x = READER_MARGIN_X;
    int max_w = w - 2 * margin_x;
    if (max_w < 60) max_w = 60;

    int doc_y = 16;
    int scroll_y = s_reader.scroll_y;
    int link_idx = 0;
    s_reader.link_count = 0;

    uint64_t file_offset = 0;
    char read_buf[512];
    char line_buf[GEM_LINE_MAX];
    int line_pos = 0;
    bool in_pre = false;

    for (;;) {
        int n = vfs_pread(fd, read_buf, sizeof(read_buf), file_offset);
        if (n <= 0) {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                line_pos = 0;
                gem_line_t gem;
                if (gemtext_parse_line(line_buf, &in_pre, &gem)) {
                    /* Process line */
                    if (gem.type == GEM_LINE_TEXT) {
                        render_wrapped_serif_line(c, margin_x, max_w, gem.text, false, READER_LINE_H, scroll_y, h, &doc_y);
                        doc_y += 4;
                    }
                }
            }
            break;
        }

        for (int i = 0; i < n; i++) {
            char ch = read_buf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                line_buf[line_pos] = '\0';
                line_pos = 0;

                gem_line_t gem;
                if (!gemtext_parse_line(line_buf, &in_pre, &gem)) continue;

                int screen_y = doc_y - scroll_y;

                switch (gem.type) {
                case GEM_LINE_H1:
                    if (screen_y + 26 >= 0 && screen_y < h) {
                        canvas1_serif_text(c, margin_x, screen_y, gem.text, strlen(gem.text), true);
                        canvas1_hline(c, margin_x, margin_x + max_w, screen_y + 19, CANVAS1_BLACK);
                    }
                    doc_y += 30;
                    break;

                case GEM_LINE_H2:
                    if (screen_y + 22 >= 0 && screen_y < h) {
                        canvas1_serif_text(c, margin_x, screen_y, gem.text, strlen(gem.text), true);
                    }
                    doc_y += 24;
                    break;

                case GEM_LINE_H3:
                    if (screen_y + 20 >= 0 && screen_y < h) {
                        canvas1_serif_text(c, margin_x, screen_y, gem.text, strlen(gem.text), false);
                    }
                    doc_y += 22;
                    break;

                case GEM_LINE_BLANK:
                    doc_y += 12;
                    break;

                case GEM_LINE_LIST:
                    if (screen_y + 16 >= 0 && screen_y < h) {
                        canvas1_fill(c, margin_x + 6, screen_y + 6, margin_x + 9, screen_y + 9, CANVAS1_BLACK);
                    }
                    render_wrapped_serif_line(c, margin_x + 18, max_w - 18, gem.text, false, READER_LINE_H, scroll_y, h, &doc_y);
                    doc_y += 4;
                    break;

                case GEM_LINE_QUOTE:
                    {
                        int q_start = doc_y;
                        render_wrapped_serif_line(c, margin_x + 16, max_w - 16, gem.text, false, READER_LINE_H, scroll_y, h, &doc_y);
                        int q_h = doc_y - q_start;
                        int q_sy = q_start - scroll_y;
                        if (q_sy + q_h >= 0 && q_sy < h) {
                            canvas1_fill(c, margin_x + 4, q_sy, margin_x + 6, q_sy + q_h - 2, CANVAS1_GREY);
                        }
                        doc_y += 6;
                    }
                    break;

                case GEM_LINE_PRE_TOGGLE:
                    doc_y += 8;
                    break;

                case GEM_LINE_PRE_TEXT:
                    if (screen_y + 16 >= 0 && screen_y < h) {
                        /* Monospace 8x16 Spleen text */
                        canvas1_text(c, margin_x + 12, screen_y, gem.text, strlen(gem.text), (max_w - 12) / 8, 0, 16, false);
                    }
                    doc_y += 16;
                    break;

                case GEM_LINE_LINK:
                    link_idx++;
                    if (screen_y + 20 >= 0 && screen_y < h) {
                        if (s_reader.link_count < READER_LINKS_MAX) {
                            s_reader.links[s_reader.link_count].y0 = screen_y;
                            s_reader.links[s_reader.link_count].y1 = screen_y + 18;
                            s_reader.links[s_reader.link_count].num = link_idx;
                            strncpy(s_reader.links[s_reader.link_count].url, gem.url, sizeof(s_reader.links[0].url) - 1);
                            s_reader.links[s_reader.link_count].url[sizeof(s_reader.links[0].url) - 1] = '\0';
                            s_reader.link_count++;
                        }

                        char prefix[16];
                        ksnprintf(prefix, sizeof(prefix), "[%d] ", link_idx);
                        int px_w = canvas1_serif_text(c, margin_x, screen_y, prefix, strlen(prefix), true);
                        const char *lbl = gem.label[0] ? gem.label : gem.url;
                        int lbl_w = canvas1_serif_text(c, margin_x + px_w, screen_y, lbl, strlen(lbl), true);
                        /* Underline the link */
                        canvas1_hline(c, margin_x + px_w, margin_x + px_w + lbl_w, screen_y + 16, CANVAS1_BLACK);
                    }
                    doc_y += 22;
                    break;

                case GEM_LINE_TEXT:
                default:
                    render_wrapped_serif_line(c, margin_x, max_w, gem.text, false, READER_LINE_H, scroll_y, h, &doc_y);
                    doc_y += 4;
                    break;
                }
            } else if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = ch;
            }
        }
        file_offset += (uint64_t)n;
    }

    vfs_close(fd);
    s_reader.total_h = doc_y + 32;

    /* Render right scrollbar if document exceeds window height */
    if (s_reader.total_h > h) {
        int sb_w = 4;
        int sb_x = w - sb_w - 2;
        int thumb_h = (h * h) / s_reader.total_h;
        if (thumb_h < 16) thumb_h = 16;
        int thumb_y = (s_reader.scroll_y * (h - thumb_h)) / (s_reader.total_h - h);
        canvas1_fill(c, sb_x, 0, sb_x + sb_w, h - 1, CANVAS1_GREY);
        canvas1_fill(c, sb_x, thumb_y, sb_x + sb_w, thumb_y + thumb_h, CANVAS1_BLACK);
    }
}
