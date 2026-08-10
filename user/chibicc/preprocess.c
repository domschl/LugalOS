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

static Macro *macros;
static int macro_cnt = 0;

static const char *builtin_lugal_h =
    "#ifndef _LUGAL_H\n"
    "#define _LUGAL_H\n"
    "#define SYS_IPC_CALL   1\n"
    "#define SYS_IPC_REPLY  2\n"
    "#define SYS_IPC_SEND   3\n"
    "#define SYS_IPC_RECV   4\n"
    "#define SYS_PRINT      10\n"
    "#define SYS_PUTNUM     11\n"
    "#define SYS_PUTCHAR    12\n"
    "#define SYS_READ_FILE  13\n"
    "#define SYS_WRITE_FILE 14\n"
    "#define IPC_ANY       -1\n"
    "struct ipc_msg {\n"
    "    long tag;\n"
    "    long d0;\n"
    "    long d1;\n"
    "    long d2;\n"
    "    long d3;\n"
    "    long d4;\n"
    "};\n"
    "long lugal_syscall(long sys_nr, long a1, long a2, long a3);\n"
    "int print(char *s);\n"
    "int puts(char *s);\n"
    "int printf(char *s);\n"
    "int putnum(long n);\n"
    "int putchar(char c);\n"
    "int read_file(char *path, void *buf, int max_len);\n"
    "int write_file(char *path, void *buf, int len);\n"
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
static char *preproc_buf;
#define HDR_BUF_SIZE 2048
static char *hdr_buf;
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

                char vfs_path[64];

                if (hdr_path[0] == '/') {
                    strncpy(vfs_path, hdr_path, 63);
                    vfs_path[63] = '\0';
                } else {
                    int vlen = 0;
                    const char *prefix = "/ram0/";
                    while (*prefix && vlen < 63) vfs_path[vlen++] = *prefix++;
                    const char *hp = hdr_path;
                    while (*hp && vlen < 63) vfs_path[vlen++] = *hp++;
                    vfs_path[vlen] = '\0';
                }

                int r = vfs_read(vfs_path, hdr_buf, sizeof(hdr_buf) - 1);
                if (r <= 0 && hdr_path[0] != '/') {
                    int vlen = 0;
                    const char *prefix = "/ram0/include/";
                    while (*prefix && vlen < 63) vfs_path[vlen++] = *prefix++;
                    const char *hp = hdr_path;
                    while (*hp && vlen < 63) vfs_path[vlen++] = *hp++;
                    vfs_path[vlen] = '\0';
                    r = vfs_read(vfs_path, hdr_buf, sizeof(hdr_buf) - 1);
                }

                if (r > 0) {
                    hdr_buf[r] = '\0';
                    preprocess_internal(hdr_buf, depth + 1);
                } else if (strcmp(hdr_path, "lugal.h") == 0 || strcmp(hdr_path, "include/lugal.h") == 0) {
                    preprocess_internal(builtin_lugal_h, depth + 1);
                } else {
                    printk("[preproc Error] Header file '%s' not found on VFS!\n", hdr_path);
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

/* Arena-backed (C6): see user/chibicc/pools.c. hdr_buf was a function-scope
 * static, which is the same memory with a narrower name -- hoisted here so the
 * arena owns every allocation this file makes rather than most of them. */
bool preprocess_pools_init(void) {
    macros = (Macro *)chibicc_pool_alloc(sizeof(Macro) * MAX_MACROS);
    preproc_buf = (char *)chibicc_pool_alloc(PREPROC_BUF_SIZE);
    hdr_buf = (char *)chibicc_pool_alloc(HDR_BUF_SIZE);
    return macros && preproc_buf && hdr_buf;
}

void preprocess_pools_clear(void) {
    macros = NULL;
    preproc_buf = NULL;
    hdr_buf = NULL;
}

uint32_t preprocess_pools_bytes(void) {
    return (uint32_t)(sizeof(Macro) * MAX_MACROS) + PREPROC_BUF_SIZE + HDR_BUF_SIZE;
}
