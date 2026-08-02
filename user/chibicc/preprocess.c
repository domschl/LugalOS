/*
 * chibicc - C Preprocessor (#include, #define, #ifndef, #ifdef, #endif)
 * Copyright (c) 2026 LugalOS Developers
 * License: MIT License
 * Adapted for LugalOS Freestanding Microkernel Architecture
 */

#include "chibicc.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include <string.h>

#define MAX_MACROS 128
#define MACRO_NAME_LEN 32
#define MACRO_VAL_LEN 128

typedef struct {
    char name[MACRO_NAME_LEN];
    char val[MACRO_VAL_LEN];
} Macro;

static Macro macros[MAX_MACROS];
static int macro_cnt = 0;

static const char *builtin_lugal_h =
    "#ifndef _LUGAL_H\n"
    "#define _LUGAL_H\n"
    "#define SYS_IPC_CALL   1\n"
    "#define SYS_IPC_REPLY  2\n"
    "#define SYS_IPC_SEND   3\n"
    "#define SYS_IPC_RECV   4\n"
    "#define IPC_ANY       -1\n"
    "struct ipc_msg {\n"
    "    long tag;\n"
    "    long d0;\n"
    "    long d1;\n"
    "    long d2;\n"
    "    long d3;\n"
    "    long d4;\n"
    "};\n"
    "#endif\n";

static void define_macro(const char *name, const char *val) {
    for (int i = 0; i < macro_cnt; i++) {
        if (strcmp(macros[i].name, name) == 0) {
            strncpy(macros[i].val, val, MACRO_VAL_LEN - 1);
            macros[i].val[MACRO_VAL_LEN - 1] = '\0';
            return;
        }
    }
    if (macro_cnt < MAX_MACROS) {
        strncpy(macros[macro_cnt].name, name, MACRO_NAME_LEN - 1);
        macros[macro_cnt].name[MACRO_NAME_LEN - 1] = '\0';
        strncpy(macros[macro_cnt].val, val, MACRO_VAL_LEN - 1);
        macros[macro_cnt].val[MACRO_VAL_LEN - 1] = '\0';
        macro_cnt++;
    }
}

static bool is_defined(const char *name) {
    for (int i = 0; i < macro_cnt; i++) {
        if (strcmp(macros[i].name, name) == 0) return true;
    }
    return false;
}

static const char *get_macro(const char *name) {
    for (int i = 0; i < macro_cnt; i++) {
        if (strcmp(macros[i].name, name) == 0) return macros[i].val;
    }
    return NULL;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_body(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

#define PREPROC_BUF_SIZE 16384
static char preproc_buf[PREPROC_BUF_SIZE];
static int preproc_out_idx = 0;

static void emit_str(const char *str) {
    while (*str && preproc_out_idx < PREPROC_BUF_SIZE - 1) {
        preproc_buf[preproc_out_idx++] = *str++;
    }
    preproc_buf[preproc_out_idx] = '\0';
}

static void emit_char(char c) {
    if (preproc_out_idx < PREPROC_BUF_SIZE - 1) {
        preproc_buf[preproc_out_idx++] = c;
        preproc_buf[preproc_out_idx] = '\0';
    }
}

static void preprocess_internal(const char *src, int depth);

static void expand_macros_and_emit(const char *line) {
    const char *p = line;
    while (*p) {
        if (is_ident_start(*p)) {
            char name[MACRO_NAME_LEN];
            int nlen = 0;
            while (*p && is_ident_body(*p) && nlen < MACRO_NAME_LEN - 1) {
                name[nlen++] = *p++;
            }
            name[nlen] = '\0';

            const char *val = get_macro(name);
            if (val) {
                emit_str(val);
            } else {
                emit_str(name);
            }
        } else {
            emit_char(*p);
            p++;
        }
    }
}

static void preprocess_internal(const char *src, int depth) {
    if (depth > 8) {
        printk("[preproc Error] Include depth exceeded 8!\n");
        return;
    }

    const char *p = src;
    bool skipping = false;
    int skip_depth = 0;

    while (*p) {
        /* Read line */
        char line[256];
        int lidx = 0;
        while (*p && *p != '\n' && lidx < 255) {
            line[lidx++] = *p++;
        }
        if (*p == '\n') p++;
        line[lidx] = '\0';

        /* Trim leading whitespace */
        char *ptr = line;
        while (*ptr == ' ' || *ptr == '\t' || *ptr == '\r') ptr++;

        if (*ptr == '#') {
            ptr++;
            while (*ptr == ' ' || *ptr == '\t') ptr++;

            if (strncmp(ptr, "ifndef", 6) == 0 && (ptr[6] == ' ' || ptr[6] == '\t' || ptr[6] == '\0')) {
                ptr += 6;
                while (*ptr == ' ' || *ptr == '\t') ptr++;
                char name[MACRO_NAME_LEN];
                int nlen = 0;
                while (*ptr && is_ident_body(*ptr) && nlen < MACRO_NAME_LEN - 1) {
                    name[nlen++] = *ptr++;
                }
                name[nlen] = '\0';

                if (is_defined(name)) {
                    skipping = true;
                    skip_depth++;
                }
                continue;
            }

            if (strncmp(ptr, "ifdef", 5) == 0 && (ptr[5] == ' ' || ptr[5] == '\t' || ptr[5] == '\0')) {
                ptr += 5;
                while (*ptr == ' ' || *ptr == '\t') ptr++;
                char name[MACRO_NAME_LEN];
                int nlen = 0;
                while (*ptr && is_ident_body(*ptr) && nlen < MACRO_NAME_LEN - 1) {
                    name[nlen++] = *ptr++;
                }
                name[nlen] = '\0';

                if (!is_defined(name)) {
                    skipping = true;
                    skip_depth++;
                }
                continue;
            }

            if (strncmp(ptr, "endif", 5) == 0) {
                if (skipping) {
                    skip_depth--;
                    if (skip_depth == 0) skipping = false;
                }
                continue;
            }

            if (skipping) continue;

            if (strncmp(ptr, "define", 6) == 0 && (ptr[6] == ' ' || ptr[6] == '\t' || ptr[6] == '\0')) {
                ptr += 6;
                while (*ptr == ' ' || *ptr == '\t') ptr++;

                char name[MACRO_NAME_LEN];
                int nlen = 0;
                while (*ptr && is_ident_body(*ptr) && nlen < MACRO_NAME_LEN - 1) {
                    name[nlen++] = *ptr++;
                }
                name[nlen] = '\0';

                while (*ptr == ' ' || *ptr == '\t') ptr++;

                char val[MACRO_VAL_LEN];
                int vlen = 0;
                while (*ptr == ' ' || *ptr == '\t') ptr++;
                while (*ptr && *ptr != '\r' && *ptr != '\n' && vlen < MACRO_VAL_LEN - 1) {
                    val[vlen++] = *ptr++;
                }
                val[vlen] = '\0';
                if (vlen == 0) { val[0] = '1'; val[1] = '\0'; }

                define_macro(name, val);
                continue;
            }

            if (strncmp(ptr, "include", 7) == 0 && (ptr[7] == ' ' || ptr[7] == '\t' || ptr[7] == '<' || ptr[7] == '"')) {
                ptr += 7;
                while (*ptr == ' ' || *ptr == '\t') ptr++;

                char delim = *ptr;
                char close_delim = (delim == '<') ? '>' : '"';
                if (delim == '<' || delim == '"') ptr++;

                char hdr_path[64];
                int hidx = 0;
                while (*ptr && *ptr != close_delim && *ptr != '\r' && *ptr != '\n' && hidx < 63) {
                    hdr_path[hidx++] = *ptr++;
                }
                hdr_path[hidx] = '\0';

                /* Check for built-in or VFS header file */
                if (strcmp(hdr_path, "lugal.h") == 0 || strcmp(hdr_path, "include/lugal.h") == 0) {
                    static char hdr_buf[2048];
                    int r = vfs_read("/ram0/lugal.h", hdr_buf, sizeof(hdr_buf) - 1);
                    if (r > 0) {
                        hdr_buf[r] = '\0';
                        preprocess_internal(hdr_buf, depth + 1);
                    } else {
                        preprocess_internal(builtin_lugal_h, depth + 1);
                    }
                } else {
                    char full_path[128];
                    if (hdr_path[0] == '/') {
                        strncpy(full_path, hdr_path, sizeof(full_path) - 1);
                        full_path[sizeof(full_path) - 1] = '\0';
                    } else {
                        int path_len = strlen(hdr_path);
                        memcpy(full_path, "/ram0/", 6);
                        memcpy(full_path + 6, hdr_path, path_len < 120 ? path_len : 120);
                        full_path[6 + (path_len < 120 ? path_len : 120)] = '\0';
                    }
                    static char hdr_buf[4096];
                    int r = vfs_read(full_path, hdr_buf, sizeof(hdr_buf) - 1);
                    if (r > 0) {
                        hdr_buf[r] = '\0';
                        preprocess_internal(hdr_buf, depth + 1);
                    } else {
                        printk("[preproc Warning] Header file '%s' not found\n", full_path);
                    }
                }
                continue;
            }
            continue;
        }

        if (!skipping) {
            expand_macros_and_emit(line);
            emit_char('\n');
        }
    }
}

char *preprocess(const char *src) {
    macro_cnt = 0;
    preproc_out_idx = 0;
    preproc_buf[0] = '\0';

    preprocess_internal(src, 0);
    return preproc_buf;
}
