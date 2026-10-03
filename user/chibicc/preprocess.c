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
/* Headers being read, as a stack (phase 40, item 5): a header is read in
 * above the ones that include it and popped when it is done. This used to be
 * one 2 KB buffer for every level, so a header that included another had the
 * rest of its own text overwritten by the inner one mid-parse. The region is
 * shared by everything open at once; a header that does not fit is an error
 * rather than a truncated read. */
#define HDR_BUF_SIZE 4096
static char *hdr_buf;
static uint32_t hdr_used;
/* The directory of the file at each include depth -- where a quoted
 * #include is looked for first. Depth 0 is the file `cc` was given. */
#define MAX_INCLUDE_DEPTH 8
#define INC_DIR_LEN 64
static char (*inc_dir)[INC_DIR_LEN];
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

/* `dir` + `name` into `out` (INC_DIR_LEN). False if it does not fit: a cut-off
 * path names some other file. */
static bool join_path(char *out, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + nl >= INC_DIR_LEN) return false;
    memcpy(out, dir, dl);
    memcpy(out + dl, name, nl + 1);
    return true;
}

/* The directory part of `path`, with its trailing '/', into `out`; "" when
 * the path has none. */
static void set_dir_of(char *out, const char *path) {
    size_t n = 0;
    for (size_t i = 0; path[i]; i++)
        if (path[i] == '/') n = i + 1;
    if (n >= INC_DIR_LEN) n = 0;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* Reads header `path` onto the stack. Returns its text, NULL if it is not
 * there; *too_big is set when it is there but does not fit. */
static char *read_header(const char *path, bool *too_big) {
    uint32_t room = HDR_BUF_SIZE - hdr_used;
    if (room < 2) { *too_big = true; return NULL; }
    char *buf = hdr_buf + hdr_used;
    int r = vfs_read(path, buf, room);
    if (r <= 0) return NULL;
    /* vfs_read() stops at room - 1 bytes; a header that filled them may have
     * been cut short, and compiling half a header is worse than refusing. */
    if ((uint32_t)r >= room - 1) { *too_big = true; return NULL; }
    buf[r] = '\0';
    hdr_used += (uint32_t)r + 1;
    return buf;
}

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

    const char *p = src;
    /* Conditionals (phase 40 review): `cond_depth` counts the open #if*
     * blocks of this file, `skip_level` is the depth at which skipping
     * started (0 = not skipping). A skip used to be a counter that only
     * conditions *false* at the time pushed, so a true #ifdef nested in a
     * skipped region closed the skip at its own #endif, and the rest of the
     * outer block was compiled. #else was not understood at all: both
     * branches were compiled. */
    int cond_depth = 0;
    int skip_level = 0;

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

            bool is_ifndef = strncmp(ptr, "ifndef", 6) == 0 && (ptr[6] == ' ' || ptr[6] == '\t' || ptr[6] == '\0');
            bool is_ifdef = strncmp(ptr, "ifdef", 5) == 0 && (ptr[5] == ' ' || ptr[5] == '\t' || ptr[5] == '\0');
            if (is_ifndef || is_ifdef) {
                ptr += is_ifndef ? 6 : 5;
                while (*ptr == ' ' || *ptr == '\t') ptr++;
                char name[MACRO_NAME_LEN];
                int nlen = 0;
                while (*ptr && is_ident_body(*ptr) && nlen < MACRO_NAME_LEN - 1) {
                    name[nlen++] = *ptr++;
                }
                name[nlen] = '\0';

                cond_depth++;
                if (skip_level == 0 && is_defined(name) == is_ifndef) skip_level = cond_depth;
                continue;
            }

            if (strncmp(ptr, "else", 4) == 0 && (ptr[4] == '\0' || ptr[4] == ' ' || ptr[4] == '\t' || ptr[4] == '\r')) {
                if (cond_depth == 0) {
                    printk("[preproc Error] #else without #ifdef/#ifndef\n");
                    continue;
                }
                if (skip_level == cond_depth) skip_level = 0;
                else if (skip_level == 0) skip_level = cond_depth;
                continue;
            }

            if (strncmp(ptr, "endif", 5) == 0) {
                if (cond_depth == 0) {
                    printk("[preproc Error] #endif without #ifdef/#ifndef\n");
                    continue;
                }
                if (skip_level == cond_depth) skip_level = 0;
                cond_depth--;
                continue;
            }

            if (skip_level) continue;

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

                /* Where to look: an absolute path is itself; a quoted name
                 * is tried beside the including file first, then in /ram0/
                 * and /ram0/include/ like an angle-bracket one. */
                const char *dirs[3];
                int ndirs = 0;
                if (hdr_path[0] == '/') {
                    dirs[ndirs++] = "";
                } else {
                    if (delim == '"' && inc_dir[depth][0]) dirs[ndirs++] = inc_dir[depth];
                    dirs[ndirs++] = "/ram0/";
                    dirs[ndirs++] = "/ram0/include/";
                }

                char vfs_path[INC_DIR_LEN];
                char *text = NULL;
                bool too_big = false;
                uint32_t mark = hdr_used;
                for (int i = 0; i < ndirs && !text && !too_big; i++) {
                    if (!join_path(vfs_path, dirs[i], hdr_path)) continue;
                    text = read_header(vfs_path, &too_big);
                }

                if (text && depth >= MAX_INCLUDE_DEPTH) {
                    printk("[preproc Error] Include depth exceeded %d at '%s'\n",
                           MAX_INCLUDE_DEPTH, vfs_path);
                } else if (text) {
                    set_dir_of(inc_dir[depth + 1], vfs_path);
                    preprocess_internal(text, depth + 1);
                } else if (too_big) {
                    printk("[preproc Error] Header '%s' does not fit the %u bytes "
                           "left for headers\n", vfs_path, (unsigned)(HDR_BUF_SIZE - mark));
                } else if (strcmp(hdr_path, "lugal.h") == 0 || strcmp(hdr_path, "include/lugal.h") == 0) {
                    preprocess_internal(builtin_lugal_h, depth + 1);
                } else {
                    printk("[preproc Error] Header file '%s' not found on VFS!\n", hdr_path);
                }
                hdr_used = mark;
                continue;
            }
            continue;
        }

        if (!skip_level) {
            expand_macros_and_emit(line);
            emit_char('\n');
        }
    }
}

char *preprocess(const char *src, const char *src_path) {
    macro_cnt = 0;
    preproc_out_idx = 0;
    preproc_buf[0] = '\0';
    hdr_used = 0;
    set_dir_of(inc_dir[0], src_path ? src_path : "");

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
    inc_dir = (char (*)[INC_DIR_LEN])chibicc_pool_alloc(INC_DIR_LEN * (MAX_INCLUDE_DEPTH + 1));
    return macros && preproc_buf && hdr_buf && inc_dir;
}

void preprocess_pools_clear(void) {
    macros = NULL;
    preproc_buf = NULL;
    hdr_buf = NULL;
    inc_dir = NULL;
}

uint32_t preprocess_pools_bytes(void) {
    return (uint32_t)(sizeof(Macro) * MAX_MACROS) + PREPROC_BUF_SIZE + HDR_BUF_SIZE +
           INC_DIR_LEN * (MAX_INCLUDE_DEPTH + 1);
}
