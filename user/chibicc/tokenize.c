/*
 * chibicc - Lexer & Tokenizer with String Literal Support
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "kernel/printk.h"
#include <string.h>

#define MAX_TOKENS 512
static Token token_pool[MAX_TOKENS];
static int token_pool_idx = 0;

#define MAX_STR_BUFS 64
#define STR_BUF_LEN 128
static char str_bufs[MAX_STR_BUFS][STR_BUF_LEN];
static int str_buf_idx = 0;

bool chibicc_pool_exhausted = false;

static Token *new_token(TokenKind kind, char *start, char *end) {
    if (token_pool_idx >= MAX_TOKENS) {
        printk("[chibicc Error] Token pool exhausted!\n");
        chibicc_pool_exhausted = true;
        /* Reuse the last slot instead of wrapping to 0: index 0 holds the
         * start of the token stream, which is still linked-to and walked by
         * the parser, so wrapping there would silently corrupt it. */
        token_pool_idx = MAX_TOKENS - 1;
    }
    Token *tok = &token_pool[token_pool_idx++];
    memset(tok, 0, sizeof(Token));
    tok->kind = kind;
    tok->loc = start;
    tok->len = end - start;
    return tok;
}

static bool startswith(const char *p, const char *q) {
    return strncmp(p, q, strlen(q)) == 0;
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static Token *read_string_literal(char *start) {
    char *p = start + 1;
    if (str_buf_idx >= MAX_STR_BUFS) {
        printk("[chibicc Error] String literal buffer pool exhausted!\n");
        chibicc_pool_exhausted = true;
        str_buf_idx = MAX_STR_BUFS - 1;
    }
    char *buf = str_bufs[str_buf_idx++];
    int len = 0;
    bool truncated = false;

    while (*p != '"' && *p != '\0') {
        char c;
        if (*p == '\\' && p[1] != '\0') {
            p++;
            if (*p == 'n') c = '\n';
            else if (*p == 't') c = '\t';
            else if (*p == '0') c = '\0';
            else c = *p;
        } else {
            c = *p;
        }
        /* STR_BUF_LEN is fixed-size storage, not a growable buffer -- a
         * literal longer than it fits must be capped, not overrun into the
         * next str_bufs[] slot (or past the array on the last slot). */
        if (len < STR_BUF_LEN - 1) {
            buf[len++] = c;
        } else {
            truncated = true;
        }
        p++;
    }

    if (truncated) {
        printk("[chibicc Error] String literal exceeds %d bytes, truncated!\n", STR_BUF_LEN - 1);
        chibicc_pool_exhausted = true;
    }

    if (*p == '"') p++;
    buf[len] = '\0';

    Token *tok = new_token(TK_STR, start, p);
    tok->str = buf;
    return tok;
}

static char *my_strstr(const char *h, const char *n) {
    int nlen = strlen(n);
    if (!nlen) return (char *)h;
    while (*h) {
        if (strncmp(h, n, nlen) == 0) return (char *)h;
        h++;
    }
    return NULL;
}

Token *tokenize(char *p) {
    token_pool_idx = 0;
    str_buf_idx = 0;
    chibicc_pool_exhausted = false;
    Token head = {0};
    Token *cur = &head;

    while (*p) {
        // Skip whitespace
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }

        // Skip line comments //
        if (startswith(p, "//")) {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }

        // Skip block comments /* */
        if (startswith(p, "/*")) {
            char *q = my_strstr(p + 2, "*/");
            if (!q) {
                printk("[chibicc Error] Unclosed block comment!\n");
                break;
            }
            p = q + 2;
            continue;
        }

        // String literals "..."
        if (*p == '"') {
            cur = cur->next = read_string_literal(p);
            p += cur->len;
            continue;
        }

        // Numbers (Decimal & Hexadecimal)
        if (is_digit(*p)) {
            char *start = p;
            long val = 0;
            if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    int d = 0;
                    if (*p >= '0' && *p <= '9') d = *p - '0';
                    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
                    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
                    val = val * 16 + d;
                    p++;
                }
            } else {
                while (is_digit(*p)) {
                    val = val * 10 + (*p - '0');
                    p++;
                }
            }
            cur = cur->next = new_token(TK_NUM, start, p);
            cur->val = val;
            continue;
        }

        // Character Literals ('...')
        if (*p == '\'') {
            char *start = p;
            p++;
            int c = *p;
            if (*p == '\\') {
                p++;
                if (*p == 'n') c = '\n';
                else if (*p == 't') c = '\t';
                else if (*p == 'r') c = '\r';
                else if (*p == '0') c = '\0';
                else if (*p == '\\') c = '\\';
                else if (*p == '\'') c = '\'';
                else c = *p;
            }
            p++;
            if (*p == '\'') p++;
            cur = cur->next = new_token(TK_NUM, start, p);
            cur->val = c;
            continue;
        }

        // Keywords & Identifiers
        if (is_alpha(*p)) {
            char *start = p;
            while (is_alnum(*p)) p++;
            cur = cur->next = new_token(TK_IDENT, start, p);
            if (cur->len == 6 && strncmp(start, "return", 6) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 2 && strncmp(start, "if", 2) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 4 && strncmp(start, "else", 4) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 5 && strncmp(start, "while", 5) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 3 && strncmp(start, "for", 3) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 4 && strncmp(start, "char", 4) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 5 && strncmp(start, "short", 5) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 3 && strncmp(start, "int", 3) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 4 && strncmp(start, "long", 4) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 6 && strncmp(start, "struct", 6) == 0) cur->kind = TK_KEYWORD;
            else if (cur->len == 5 && strncmp(start, "break", 5) == 0) cur->kind = TK_KEYWORD;
            continue;
        }

        // Multi-character punctuation
        if (startswith(p, "==") || startswith(p, "!=") || startswith(p, "<=") ||
            startswith(p, ">=") || startswith(p, "->") || startswith(p, "++") ||
            startswith(p, "--")) {
            cur = cur->next = new_token(TK_PUNCT, p, p + 2);
            p += 2;
            continue;
        }

        // Single-character punctuation
        if (strchr("+-*/%()={};<>&[],.#", *p)) {
            cur = cur->next = new_token(TK_PUNCT, p, p + 1);
            p++;
            continue;
        }

        printk("[chibicc Error] Cannot tokenize character '%c'\n", *p);
        p++;
    }

    cur->next = new_token(TK_EOF, p, p);
    return head.next;
}
