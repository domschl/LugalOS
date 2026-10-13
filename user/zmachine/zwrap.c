/* user/zmachine/zwrap.c — Word-wrapping text formatter for Z-machine
 *
 * Freestanding: breaks lines strictly at whitespace, preserving whole words.
 */
#include "zwrap.h"

#include <string.h>

static int get_width(const struct z_wrap *w)
{
    int width = (w->width_fn != NULL) ? w->width_fn(w->user) : 80;
    if (width < 10) {
        width = 10;
    }
    return width;
}

void zwrap_init(struct z_wrap *w, zwrap_out_fn out_fn, zwrap_width_fn width_fn, void *user)
{
    w->out_fn = out_fn;
    w->width_fn = width_fn;
    w->user = user;
    zwrap_reset(w);
}

void zwrap_reset(struct z_wrap *w)
{
    w->col = 0;
    w->spaces = 0;
    w->word_len = 0;
    w->in_input = false;
}

void zwrap_flush(struct z_wrap *w)
{
    if (w->in_input || w->out_fn == NULL) {
        return;
    }
    int width = get_width(w);

    if (w->word_len > 0) {
        if (w->col > 0 && (w->col + w->spaces + w->word_len > width)) {
            w->out_fn(w->user, "\n", 1);
            w->col = 0;
            w->spaces = 0;
        } else if (w->col == 0 && w->spaces >= width) {
            w->spaces = 0;
        }
        while (w->spaces > 0) {
            w->out_fn(w->user, " ", 1);
            w->col++;
            w->spaces--;
        }
        w->out_fn(w->user, w->word_buf, (size_t)w->word_len);
        w->col += w->word_len;
        w->word_len = 0;
    } else if (w->spaces > 0) {
        if (w->col > 0 && (w->col + w->spaces > width)) {
            w->spaces = 0;
        }
        while (w->spaces > 0) {
            w->out_fn(w->user, " ", 1);
            w->col++;
            w->spaces--;
        }
    }
}

void zwrap_putc(struct z_wrap *w, uint8_t ch)
{
    if (w->out_fn == NULL) {
        return;
    }

    if (w->in_input) {
        if (ch == '\n' || ch == '\r') {
            w->out_fn(w->user, "\n", 1);
            w->col = 0;
            w->spaces = 0;
            w->word_len = 0;
            w->in_input = false;
        } else if (ch == '\b' || ch == 127) {
            char b[1] = { (char)ch };
            w->out_fn(w->user, b, 1);
            if (w->col > 0) {
                w->col--;
            }
        } else {
            char b[1] = { (char)ch };
            w->out_fn(w->user, b, 1);
            w->col++;
            int width = get_width(w);
            if (w->col >= width) {
                w->col = 0;
            }
        }
        return;
    }

    if (ch == '\n' || ch == '\r') {
        zwrap_flush(w);
        w->out_fn(w->user, "\n", 1);
        w->col = 0;
        w->spaces = 0;
        w->word_len = 0;
        return;
    }

    if (ch == ' ' || ch == '\t') {
        if (w->word_len > 0) {
            zwrap_flush(w);
        }
        w->spaces++;
        return;
    }

    /* Normal character */
    int width = get_width(w);

    /* If we are not at column 0, and current word cannot possibly fit
     * on the rest of this line, wrap to next line now! */
    if (w->col > 0 && (w->col + w->spaces + w->word_len + 1 > width)) {
        w->out_fn(w->user, "\n", 1);
        w->col = 0;
        w->spaces = 0;
    }

    /* If word buffer fills up or exceeds line width, emit chunk */
    if (w->word_len >= width || w->word_len >= (int)sizeof(w->word_buf) - 1) {
        while (w->spaces > 0) {
            w->out_fn(w->user, " ", 1);
            w->col++;
            w->spaces--;
        }
        w->out_fn(w->user, w->word_buf, (size_t)w->word_len);
        w->out_fn(w->user, "\n", 1);
        w->col = 0;
        w->word_len = 0;
    }

    w->word_buf[w->word_len++] = (char)ch;
}
