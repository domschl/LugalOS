#include "ed.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "drivers/uart.h"
#include <string.h>

#define MAX_ED_LINES 32
#define MAX_LINE_LEN 80

static char ed_buf[MAX_ED_LINES][MAX_LINE_LEN];
static int line_count = 0;
static char current_file[32];

static void ed_read_line(char *out_buf, int max_len) {
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
            }
        }
    }
}

static void ed_load_file(const char *filename) {
    char raw_buf[512];
    int bytes = vfs_read(filename, raw_buf, 512);
    if (bytes < 0) {
        printk("'%s': [New File]\n", filename);
        line_count = 0;
        return;
    }

    line_count = 0;
    int col = 0;
    for (int i = 0; i < bytes; i++) {
        if (raw_buf[i] == '\n' || raw_buf[i] == '\r') {
            if (col > 0) {
                ed_buf[line_count][col] = '\0';
                line_count++;
                col = 0;
            }
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
    printk("'%s': %d bytes read (%d lines)\n", filename, bytes, line_count);
}

static void ed_save_file(const char *filename) {
    char out_buf[1024];
    out_buf[0] = '\0';
    int total_len = 0;

    for (int i = 0; i < line_count; i++) {
        int len = strlen(ed_buf[i]);
        memcpy(&out_buf[total_len], ed_buf[i], len);
        total_len += len;
        out_buf[total_len++] = '\n';
    }
    out_buf[total_len] = '\0';

    if (vfs_write(filename, out_buf, total_len) == 0) {
        printk("'%s': %d bytes written\n", filename, total_len);
    } else {
        printk("[ed Error] Failed to write '%s'\n", filename);
    }
}

void ed_main(const char *filename) {
    if (filename && filename[0] != '\0') {
        strncpy(current_file, filename, 31);
        current_file[31] = '\0';
        ed_load_file(current_file);
    } else {
        current_file[0] = '\0';
        line_count = 0;
        printk("[New Buffer]\n");
    }

    char cmd[128];
    while (1) {
        printk(":");
        ed_read_line(cmd, 128);

        if (cmd[0] == '\0') continue;

        if (strcmp(cmd, "q") == 0) {
            break;
        } else if (strcmp(cmd, "a") == 0) {
            /* Append Mode */
            while (line_count < MAX_ED_LINES) {
                ed_read_line(ed_buf[line_count], MAX_LINE_LEN);
                if (strcmp(ed_buf[line_count], ".") == 0) {
                    break;
                }
                line_count++;
            }
        } else if (strcmp(cmd, "p") == 0 || strcmp(cmd, "1,$p") == 0) {
            for (int i = 0; i < line_count; i++) {
                printk("%s\n", ed_buf[i]);
            }
        } else if (strcmp(cmd, "n") == 0 || strcmp(cmd, "1,$n") == 0) {
            for (int i = 0; i < line_count; i++) {
                printk("%d: %s\n", i + 1, ed_buf[i]);
            }
        } else if (strcmp(cmd, "w") == 0) {
            if (current_file[0] != '\0') {
                ed_save_file(current_file);
            } else {
                printk("No file name specified\n");
            }
        } else if (cmd[0] == 'w' && cmd[1] == ' ') {
            strncpy(current_file, &cmd[2], 31);
            ed_save_file(current_file);
        } else if (cmd[0] == 'd') {
            if (line_count > 0) {
                line_count--;
                printk("Line deleted (%d lines remaining)\n", line_count);
            }
        } else {
            printk("?\n");
        }
    }
}
