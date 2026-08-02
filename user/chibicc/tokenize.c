/*
 * chibicc - Tokenizer & Lexer
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

static Token *new_token(TokenKind kind, char *start, char *end) {
    if (token_pool_idx >= MAX_TOKENS) {
        printk("[chibicc Error] Token pool exhausted!\n");
        token_pool_idx = 0;
    }
    Token *tok = &token_pool[token_pool_idx++];
    tok->kind = kind;
    tok->loc = start;
    tok->len = (int)(end - start);
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

static bool is_keyword(Token *tok) {
    static const char *kw[] = {"return", "int", "if", "else", "while", "for", NULL};
    for (int i = 0; kw[i]; i++) {
        if (tok->len == (int)strlen(kw[i]) && strncmp(tok->loc, kw[i], tok->len) == 0) {
            return true;
        }
    }
    return false;
}

Token *tokenize(char *p) {
    token_pool_idx = 0;
    Token head = {0};
    Token *cur = &head;

    while (*p) {
        // Skip whitespace
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }

        // Skip line comments
        if (startswith(p, "//")) {
            p += 2;
            while (*p && *p != '\n') p++;
            continue;
        }

        // Skip block comments
        if (startswith(p, "/*")) {
            char *q = strchr(p + 2, '*');
            while (q && q[1] != '/') {
                q = strchr(q + 1, '*');
            }
            if (!q) {
                printk("[chibicc Error] Unclosed block comment\n");
                break;
            }
            p = q + 2;
            continue;
        }

        // Numbers
        if (*p >= '0' && *p <= '9') {
            cur = cur->next = new_token(TK_NUM, p, p);
            char *q = p;
            long val = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    val = val * 16 + ((*p >= 'a') ? (*p - 'a' + 10) : ((*p >= 'A') ? (*p - 'A' + 10) : (*p - '0')));
                    p++;
                }
            } else {
                while (*p >= '0' && *p <= '9') {
                    val = val * 10 + (*p - '0');
                    p++;
                }
            }
            cur->val = val;
            cur->len = (int)(p - q);
            continue;
        }

        // Identifiers and Keywords
        if (is_alpha(*p)) {
            char *q = p;
            while (is_alnum(*p)) p++;
            cur = cur->next = new_token(TK_IDENT, q, p);
            if (is_keyword(cur)) {
                cur->kind = TK_KEYWORD;
            }
            continue;
        }

        // Multi-character punctuation
        if (startswith(p, "==") || startswith(p, "!=") || startswith(p, "<=") || startswith(p, ">=")) {
            cur = cur->next = new_token(TK_PUNCT, p, p + 2);
            p += 2;
            continue;
        }

        // Single-character punctuation
        if (strchr("+-*/%()={};<>", *p)) {
            cur = cur->next = new_token(TK_PUNCT, p, p + 1);
            p++;
            continue;
        }

        printk("[chibicc Error] Cannot tokenize character '%c'\n", *p);
        p++;
    }

    cur = cur->next = new_token(TK_EOF, p, p);
    return head.next;
}
