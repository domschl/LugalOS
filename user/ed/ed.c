#include "kernel/console.h"
#include "ed.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/palloc.h"
#include "drivers/uart.h"
#include <string.h>

static void ed_main_inner(const char *filename);

#define MAX_ED_LINES 256
#define MAX_LINE_LEN 128

/* Heap-on-demand (C6/C7, plan/phase6_memory_and_processes.md): the editor's
 * buffers are taken while it is running and returned when it exits, so an
 * editor nobody has opened costs nothing. 44 KB on a 512 KB board is most of
 * a user program's worth of memory to reserve for a text editor. */
static char (*ed_buf)[MAX_LINE_LEN];
static int line_count = 0;
static int dot = 0; // 1-based current line index (0 if buffer empty)
static char current_file[64];

#define ED_RAW_BUF_SIZE 4096
#define ED_OUT_BUF_SIZE 8192
static char *raw_buf;   /* both were function-scope statics: the same memory */
static char *out_buf;   /* under a narrower name */
static uint8_t *g_ed_arena;
static uint32_t g_ed_pages;

/* One arena for all three, for the same reason chibicc has one: three
 * allocations would each round up to a page and give three failure points to
 * unwind instead of one. */
static bool ed_buffers_acquire(void) {
    if (g_ed_arena) return true;
    uint32_t want = (uint32_t)(MAX_ED_LINES * MAX_LINE_LEN)
                  + ED_RAW_BUF_SIZE + ED_OUT_BUF_SIZE;
    g_ed_pages = (want + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    g_ed_arena = (uint8_t *)palloc_pages(g_ed_pages);
    if (!g_ed_arena) {
        cprintf("ed: no memory for a %u KB editor buffer\n",
                g_ed_pages * (uint32_t)PAGE_SIZE / 1024);
        return false;
    }
    ed_buf = (char (*)[MAX_LINE_LEN])g_ed_arena;
    raw_buf = (char *)(g_ed_arena + MAX_ED_LINES * MAX_LINE_LEN);
    out_buf = raw_buf + ED_RAW_BUF_SIZE;
    return true;
}

static void ed_buffers_release(void) {
    if (!g_ed_arena) return;
    palloc_free(g_ed_arena, g_ed_pages);
    g_ed_arena = NULL;
    g_ed_pages = 0;
    ed_buf = NULL;
    raw_buf = NULL;
    out_buf = NULL;
}

static void ed_read_line(char *out_buf, int max_len) {
    if (!out_buf || max_len <= 0) return;
    int idx = 0;
    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            out_buf[idx] = '\0';
            break;
        } else if (c == 0x08 || c == 0x7F) {
            if (idx > 0) {
                idx--;
                uart_puts("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (idx < max_len - 1) {
                out_buf[idx++] = c;
                uart_putc(c);
                /* M4: uart_putc() batches now -- without this, this
                 * keystroke's echo only becomes visible whenever the
                 * *next* uart_getc() call happens to flush it, a
                 * one-character lag a real user would notice. */
                uart_flush();
            }
        }
    }
}

static void ed_load_file(const char *filename) {
    if (!filename || filename[0] == '\0') {
        line_count = 0;
        dot = 0;
        cprintf("?\n");
        return;
    }


    int bytes = vfs_read(filename, raw_buf, sizeof(raw_buf) - 1);
    if (bytes < 0) {
        cprintf("'%s': [New File]\n", filename);
        line_count = 0;
        dot = 0;
        return;
    }
    raw_buf[bytes] = '\0';

    line_count = 0;
    int col = 0;
    for (int i = 0; i < bytes && line_count < MAX_ED_LINES; i++) {
        if (raw_buf[i] == '\n' || raw_buf[i] == '\r') {
            if (raw_buf[i] == '\r' && i + 1 < bytes && raw_buf[i + 1] == '\n') {
                i++;
            }
            ed_buf[line_count][col] = '\0';
            line_count++;
            col = 0;
        } else {
            if (col < MAX_LINE_LEN - 1) {
                ed_buf[line_count][col++] = raw_buf[i];
            }
        }
    }
    if (col > 0 && line_count < MAX_ED_LINES) {
        ed_buf[line_count][col] = '\0';
        line_count++;
    }

    dot = (line_count > 0) ? line_count : 0;
    cprintf("'%s': %d bytes (%d lines)\n", filename, bytes, line_count);
}

static void ed_save_file(const char *filename) {
    if (!filename || filename[0] == '\0') {
        cprintf("?\n");
        return;
    }


    int total_len = 0;

    for (int i = 0; i < line_count; i++) {
        int len = strlen(ed_buf[i]);
        if (total_len + len + 1 < (int)sizeof(out_buf) - 1) {
            memcpy(&out_buf[total_len], ed_buf[i], len);
            total_len += len;
            out_buf[total_len++] = '\n';
        }
    }
    out_buf[total_len] = '\0';

    if (vfs_write(filename, out_buf, total_len) == 0) {
        cprintf("'%s': %d bytes written\n", filename, total_len);
    } else {
        cprintf("?\n");
    }
}

static int ed_insert_line(int index, const char *text) {
    if (line_count >= MAX_ED_LINES || index < 1 || index > line_count + 1) return -1;
    for (int i = line_count; i >= index; i--) {
        strncpy(ed_buf[i], ed_buf[i - 1], MAX_LINE_LEN - 1);
        ed_buf[i][MAX_LINE_LEN - 1] = '\0';
    }
    strncpy(ed_buf[index - 1], text, MAX_LINE_LEN - 1);
    ed_buf[index - 1][MAX_LINE_LEN - 1] = '\0';
    line_count++;
    dot = index;
    return 0;
}

static int ed_delete_line(int index) {
    if (line_count <= 0 || index < 1 || index > line_count) return -1;
    for (int i = index - 1; i < line_count - 1; i++) {
        strncpy(ed_buf[i], ed_buf[i + 1], MAX_LINE_LEN - 1);
        ed_buf[i][MAX_LINE_LEN - 1] = '\0';
    }
    line_count--;
    if (dot > line_count) dot = line_count;
    if (dot < 1 && line_count > 0) dot = 1;
    return 0;
}

static const char *parse_single_addr(const char *p, int *addr) {
    while (*p == ' ') p++;
    if (*p == '.') {
        *addr = (dot > 0) ? dot : 1;
        return p + 1;
    } else if (*p == '$') {
        *addr = (line_count > 0) ? line_count : 1;
        return p + 1;
    } else if (*p >= '0' && *p <= '9') {
        int val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        *addr = val;
        return p;
    }
    return p;
}

static const char *parse_range(const char *p, int *start, int *end) {
    *start = (dot > 0) ? dot : 1;
    *end = *start;

    while (*p == ' ') p++;
    if (*p == ',' || *p == '%') {
        *start = 1;
        *end = (line_count > 0) ? line_count : 1;
        return p + 1;
    }

    const char *next = parse_single_addr(p, start);
    if (next == p) {
        return p;
    }

    p = next;
    while (*p == ' ') p++;
    if (*p == ',') {
        p++;
        next = parse_single_addr(p, end);
        if (next == p) {
            *end = (line_count > 0) ? line_count : 1;
        } else {
            p = next;
        }
    } else {
        *end = *start;
    }

    if (*start < 1) *start = 1;
    if (*end < *start) *end = *start;
    if (*end > line_count && line_count > 0) *end = line_count;

    return p;
}

static void ed_substitute(int start, int end, const char *pattern) {
    if (start < 1 || end > line_count || start > end) {
        cprintf("?\n");
        return;
    }

    char sep = *pattern++;
    if (!sep) { cprintf("?\n"); return; }

    char old_str[64], new_str[64];
    int idx = 0;
    while (*pattern && *pattern != sep && idx < 63) {
        old_str[idx++] = *pattern++;
    }
    old_str[idx] = '\0';

    if (*pattern == sep) pattern++;

    idx = 0;
    while (*pattern && *pattern != sep && idx < 63) {
        new_str[idx++] = *pattern++;
    }
    new_str[idx] = '\0';

    if (old_str[0] == '\0') {
        cprintf("?\n");
        return;
    }

    int modified = 0;
    for (int l = start; l <= end; l++) {
        char *line = ed_buf[l - 1];
        char *pos = strstr(line, old_str);
        if (pos) {
            char temp[MAX_LINE_LEN];
            int prefix_len = (int)(pos - line);
            strncpy(temp, line, prefix_len);
            temp[prefix_len] = '\0';
            strncat(temp, new_str, MAX_LINE_LEN - strlen(temp) - 1);
            strncat(temp, pos + strlen(old_str), MAX_LINE_LEN - strlen(temp) - 1);
            strncpy(line, temp, MAX_LINE_LEN - 1);
            line[MAX_LINE_LEN - 1] = '\0';
            dot = l;
            modified++;
        }
    }

    if (modified == 0) {
        cprintf("?\n");
    } else {
        cprintf("%s\n", ed_buf[dot - 1]);
    }
}

static void ed_search(const char *pattern) {
    if (line_count <= 0) { cprintf("?\n"); return; }
    char search_str[64];
    int idx = 0;
    while (*pattern && *pattern != '/' && idx < 63) {
        search_str[idx++] = *pattern++;
    }
    search_str[idx] = '\0';

    int start_line = (dot > 0) ? dot + 1 : 1;
    for (int i = 0; i < line_count; i++) {
        int l = ((start_line - 1 + i) % line_count) + 1;
        if (strstr(ed_buf[l - 1], search_str) != NULL) {
            dot = l;
            cprintf("%d: %s\n", dot, ed_buf[dot - 1]);
            return;
        }
    }
    cprintf("?\n");
}

void ed_main(const char *filename) {
    /* Acquired for the editor's lifetime and released on every exit path --
     * ed_main() returns from several places, and one of them forgetting would
     * leak 44 KB on a 160 KB heap. */
    if (!ed_buffers_acquire()) return;
    ed_main_inner(filename);
    ed_buffers_release();
}

static void ed_main_inner(const char *filename) {
    if (filename && filename[0] != '\0') {
        strncpy(current_file, filename, 63);
        current_file[63] = '\0';
        ed_load_file(current_file);
    } else {
        current_file[0] = '\0';
        line_count = 0;
        dot = 0;
        printk("[New Buffer]\n");
    }

    char line_in[128];
    while (1) {
        cprintf(":");
        ed_read_line(line_in, 128);
        if (line_in[0] == '\0') continue;

        if (line_in[0] == '/') {
            ed_search(&line_in[1]);
            continue;
        }

        int start_addr = 0, end_addr = 0;
        const char *cmd_p = parse_range(line_in, &start_addr, &end_addr);
        while (*cmd_p == ' ') cmd_p++;

        if (*cmd_p == '\0') {
            if (start_addr >= 1 && start_addr <= line_count) {
                dot = start_addr;
                cprintf("%s\n", ed_buf[dot - 1]);
            } else {
                cprintf("?\n");
            }
            continue;
        }

        char cmd = *cmd_p++;

        if (cmd == 'q') {
            break;
        } else if (cmd == 'f') {
            while (*cmd_p == ' ') cmd_p++;
            if (*cmd_p != '\0') {
                strncpy(current_file, cmd_p, 63);
                current_file[63] = '\0';
            }
            cprintf("%s\n", current_file[0] != '\0' ? current_file : "[No name]");
        } else if (cmd == 'e') {
            while (*cmd_p == ' ') cmd_p++;
            if (*cmd_p != '\0') {
                strncpy(current_file, cmd_p, 63);
                current_file[63] = '\0';
            }
            ed_load_file(current_file);
        } else if (cmd == 'w') {
            while (*cmd_p == ' ') cmd_p++;
            if (*cmd_p != '\0') {
                ed_save_file(cmd_p);
            } else if (current_file[0] != '\0') {
                ed_save_file(current_file);
            } else {
                cprintf("?\n");
            }
        } else if (cmd == 'a') {
            int insert_pos = (end_addr >= 0 && end_addr <= line_count) ? end_addr : dot;
            while (line_count < MAX_ED_LINES) {
                char input_str[MAX_LINE_LEN];
                ed_read_line(input_str, MAX_LINE_LEN);
                if (strcmp(input_str, ".") == 0) break;
                insert_pos++;
                ed_insert_line(insert_pos, input_str);
            }
        } else if (cmd == 'i') {
            int insert_pos = (start_addr >= 1 && start_addr <= line_count) ? start_addr : (dot > 0 ? dot : 1);
            while (line_count < MAX_ED_LINES) {
                char input_str[MAX_LINE_LEN];
                ed_read_line(input_str, MAX_LINE_LEN);
                if (strcmp(input_str, ".") == 0) break;
                ed_insert_line(insert_pos, input_str);
                insert_pos++;
            }
        } else if (cmd == 'c') {
            if (line_count > 0 && start_addr >= 1 && end_addr <= line_count && start_addr <= end_addr) {
                int count_to_del = end_addr - start_addr + 1;
                for (int d = 0; d < count_to_del; d++) {
                    ed_delete_line(start_addr);
                }
                int insert_pos = start_addr;
                while (line_count < MAX_ED_LINES) {
                    char input_str[MAX_LINE_LEN];
                    ed_read_line(input_str, MAX_LINE_LEN);
                    if (strcmp(input_str, ".") == 0) break;
                    ed_insert_line(insert_pos, input_str);
                    insert_pos++;
                }
            } else {
                cprintf("?\n");
            }
        } else if (cmd == 'd') {
            if (line_count > 0 && start_addr >= 1 && end_addr <= line_count && start_addr <= end_addr) {
                int count_to_del = end_addr - start_addr + 1;
                for (int d = 0; d < count_to_del; d++) {
                    ed_delete_line(start_addr);
                }
            } else {
                cprintf("?\n");
            }
        } else if (cmd == 'p') {
            if (line_count > 0 && start_addr >= 1 && end_addr <= line_count && start_addr <= end_addr) {
                for (int i = start_addr; i <= end_addr; i++) {
                    cprintf("%s\n", ed_buf[i - 1]);
                }
                dot = end_addr;
            } else {
                cprintf("?\n");
            }
        } else if (cmd == 'n') {
            if (line_count > 0 && start_addr >= 1 && end_addr <= line_count && start_addr <= end_addr) {
                for (int i = start_addr; i <= end_addr; i++) {
                    cprintf("%d: %s\n", i, ed_buf[i - 1]);
                }
                dot = end_addr;
            } else {
                cprintf("?\n");
            }
        } else if (cmd == 's') {
            ed_substitute(start_addr, end_addr, cmd_p);
        } else {
            cprintf("?\n");
        }
    }
}
