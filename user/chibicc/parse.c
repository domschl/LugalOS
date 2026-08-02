/*
 * chibicc - Recursive Descent Parser
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "kernel/printk.h"
#include <string.h>

#define MAX_NODES 1024
static Node node_pool[MAX_NODES];
static int node_pool_idx = 0;

#define MAX_OBJS 256
static Obj obj_pool[MAX_OBJS];
static int obj_pool_idx = 0;

static Obj *locals = NULL;

static Node *new_node(NodeKind kind) {
    if (node_pool_idx >= MAX_NODES) {
        printk("[chibicc Error] Node pool exhausted!\n");
        node_pool_idx = 0;
    }
    Node *node = &node_pool[node_pool_idx++];
    node->kind = kind;
    return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs) {
    Node *node = new_node(kind);
    node->lhs = lhs;
    node->rhs = rhs;
    return node;
}

static Node *new_num(long val) {
    Node *node = new_node(ND_NUM);
    node->val = val;
    return node;
}

static Obj *new_var(const char *name) {
    if (obj_pool_idx >= MAX_OBJS) {
        printk("[chibicc Error] Obj pool exhausted!\n");
        obj_pool_idx = 0;
    }
    Obj *var = &obj_pool[obj_pool_idx++];
    strncpy(var->name, name, 31);
    var->name[31] = '\0';
    var->next = locals;
    locals = var;
    return var;
}

static Obj *find_var(Token *tok) {
    for (Obj *var = locals; var; var = var->next) {
        if ((int)strlen(var->name) == tok->len && strncmp(tok->loc, var->name, tok->len) == 0) {
            return var;
        }
    }
    return NULL;
}

static bool equal(Token *tok, const char *op) {
    return tok->len == (int)strlen(op) && strncmp(tok->loc, op, tok->len) == 0;
}

static Token *skip(Token *tok, const char *op) {
    if (!equal(tok, op)) {
        printk("[chibicc Error] Expected '%s'\n", op);
        return tok;
    }
    return tok->next;
}

/* Forward declarations */
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);

static Node *primary(Token **rest, Token *tok) {
    if (equal(tok, "(")) {
        Node *node = expr(&tok, tok->next);
        *rest = skip(tok, ")");
        return node;
    }

    if (tok->kind == TK_NUM) {
        Node *node = new_num(tok->val);
        *rest = tok->next;
        return node;
    }

    if (tok->kind == TK_IDENT) {
        Obj *var = find_var(tok);
        if (!var) {
            char namebuf[32];
            int len = tok->len < 31 ? tok->len : 31;
            strncpy(namebuf, tok->loc, len);
            namebuf[len] = '\0';
            var = new_var(namebuf);
        }
        Node *node = new_node(ND_VAR);
        node->var = var;
        *rest = tok->next;
        return node;
    }

    printk("[chibicc Error] Expected expression\n");
    *rest = tok->next;
    return new_num(0);
}

static Node *unary(Token **rest, Token *tok) {
    if (equal(tok, "+")) return unary(rest, tok->next);
    if (equal(tok, "-")) return new_binary(ND_NEG, unary(rest, tok->next), new_num(0));
    return primary(rest, tok);
}

static Node *mul(Token **rest, Token *tok) {
    Node *node = unary(&tok, tok);
    while (1) {
        if (equal(tok, "*")) {
            node = new_binary(ND_MUL, node, unary(&tok, tok->next));
            continue;
        }
        if (equal(tok, "/")) {
            node = new_binary(ND_DIV, node, unary(&tok, tok->next));
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *add(Token **rest, Token *tok) {
    Node *node = mul(&tok, tok);
    while (1) {
        if (equal(tok, "+")) {
            node = new_binary(ND_ADD, node, mul(&tok, tok->next));
            continue;
        }
        if (equal(tok, "-")) {
            node = new_binary(ND_SUB, node, mul(&tok, tok->next));
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *relational(Token **rest, Token *tok) {
    Node *node = add(&tok, tok);
    while (1) {
        if (equal(tok, "<")) {
            node = new_binary(ND_LT, node, add(&tok, tok->next));
            continue;
        }
        if (equal(tok, "<=")) {
            node = new_binary(ND_LE, node, add(&tok, tok->next));
            continue;
        }
        if (equal(tok, ">")) {
            node = new_binary(ND_LT, add(&tok, tok->next), node);
            continue;
        }
        if (equal(tok, ">=")) {
            node = new_binary(ND_LE, add(&tok, tok->next), node);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *equality(Token **rest, Token *tok) {
    Node *node = relational(&tok, tok);
    while (1) {
        if (equal(tok, "==")) {
            node = new_binary(ND_EQ, node, relational(&tok, tok->next));
            continue;
        }
        if (equal(tok, "!=")) {
            node = new_binary(ND_NE, node, relational(&tok, tok->next));
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *assign(Token **rest, Token *tok) {
    Node *node = equality(&tok, tok);
    if (equal(tok, "=")) {
        node = new_binary(ND_ASSIGN, node, assign(&tok, tok->next));
    }
    *rest = tok;
    return node;
}

static Node *expr(Token **rest, Token *tok) {
    return assign(rest, tok);
}

static Node *stmt(Token **rest, Token *tok) {
    if (equal(tok, "return")) {
        Node *node = new_node(ND_RETURN);
        node->lhs = expr(&tok, tok->next);
        *rest = skip(tok, ";");
        return node;
    }

    if (equal(tok, "int")) {
        tok = tok->next; // skip "int"
        Token *var_tok = tok;
        tok = tok->next;
        Obj *var = find_var(var_tok);
        if (!var) {
            char namebuf[32];
            int len = var_tok->len < 31 ? var_tok->len : 31;
            strncpy(namebuf, var_tok->loc, len);
            namebuf[len] = '\0';
            var = new_var(namebuf);
        }
        if (equal(tok, "=")) {
            Node *node = new_node(ND_EXPR_STMT);
            Node *lhs = new_node(ND_VAR);
            lhs->var = var;
            node->lhs = new_binary(ND_ASSIGN, lhs, expr(&tok, tok->next));
            *rest = skip(tok, ";");
            return node;
        }
        *rest = skip(tok, ";");
        return new_node(ND_EXPR_STMT);
    }

    if (equal(tok, "{")) {
        return compound_stmt(rest, tok->next);
    }

    Node *node = new_node(ND_EXPR_STMT);
    node->lhs = expr(&tok, tok);
    *rest = skip(tok, ";");
    return node;
}

static Node *compound_stmt(Token **rest, Token *tok) {
    Node head = {0};
    Node *cur = &head;
    while (!equal(tok, "}")) {
        cur = cur->next = stmt(&tok, tok);
    }
    *rest = tok->next;
    Node *node = new_node(ND_EXPR_STMT);
    node->body = head.next;
    return node;
}

static Function *function(Token **rest, Token *tok) {
    node_pool_idx = 0;
    obj_pool_idx = 0;
    locals = NULL;

    tok = skip(tok, "int");
    Token *fn_tok = tok;
    tok = tok->next;
    tok = skip(tok, "(");
    tok = skip(tok, ")");
    tok = skip(tok, "{");

    Function *fn = &((Function){0});
    char fn_name[32];
    int len = fn_tok->len < 31 ? fn_tok->len : 31;
    strncpy(fn_name, fn_tok->loc, len);
    fn_name[len] = '\0';
    fn->name = fn_name;

    fn->body = compound_stmt(rest, tok);
    fn->locals = locals;

    int offset = 16;
    for (Obj *var = locals; var; var = var->next) {
        offset += 8;
        var->offset = -offset;
    }
    fn->stack_size = offset + 16;
    return fn;
}

Function *parse(Token *tok) {
    Function head = {0};
    Function *cur = &head;
    while (tok->kind != TK_EOF) {
        cur = cur->next = function(&tok, tok);
    }
    return head.next;
}
