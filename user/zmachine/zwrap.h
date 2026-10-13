#ifndef ZWRAP_H
#define ZWRAP_H

/* user/zmachine/zwrap.h — Word-wrapping text formatter for Z-machine
 *
 * Provides clean word wrapping at whitespace boundaries for terminal
 * output streams. Preserves whole words, indentation, paragraphs,
 * and prompt formatting.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ZWRAP_BUF_MAX 128

typedef void (*zwrap_out_fn)(void *user, const char *s, size_t len);
typedef int  (*zwrap_width_fn)(void *user);

struct z_wrap {
    zwrap_out_fn   out_fn;
    zwrap_width_fn width_fn;
    void          *user;

    int            col;        /* Current column on line (0 .. width-1) */
    int            spaces;     /* Pending whitespace count before current word */
    int            word_len;   /* Current buffered word length */
    char           word_buf[ZWRAP_BUF_MAX];
    bool           in_input;   /* True during user input echoing */
};

void zwrap_init(struct z_wrap *w, zwrap_out_fn out_fn, zwrap_width_fn width_fn, void *user);
void zwrap_reset(struct z_wrap *w);
void zwrap_flush(struct z_wrap *w);
void zwrap_putc(struct z_wrap *w, uint8_t ch);

#endif /* ZWRAP_H */
