#ifndef LUGALOS_KERNEL_LINE_EDITOR_H
#define LUGALOS_KERNEL_LINE_EDITOR_H

void line_editor_init(void);
int readline_interactive(const char *prompt, char *out_buf, int max_len);
int edit_multiline_box(const char *initial_filename, char *out_buf, int max_len);

#endif /* LUGALOS_KERNEL_LINE_EDITOR_H */

