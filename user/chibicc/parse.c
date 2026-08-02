/*
 * chibicc - Recursive Descent Parser with Pointers, Arrays, Control Flow & Function Calls
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "kernel/printk.h"
#include <string.h>

#define MAX_NODES 256
static Node node_pool[MAX_NODES];
static int node_pool_idx = 0;

#define MAX_OBJS 64
static Obj obj_pool[MAX_OBJS];
static int obj_pool_idx = 0;

#define MAX_FNS 16
static Function fn_pool[MAX_FNS];
static int fn_pool_idx = 0;

#define MAX_TYPES 64
static Type type_pool[MAX_TYPES];
static int type_pool_idx = 0;

Type ty_int_obj = {TY_INT, 8, NULL, 0};
Type *ty_int = &ty_int_obj;

static Type *pointer_to(Type *base) {
    if (type_pool_idx >= MAX_TYPES) type_pool_idx = 0;
    Type *ty = &type_pool[type_pool_idx++];
    memset(ty, 0, sizeof(Type));
    ty->kind = TY_PTR;
    ty->size = 8;
    ty->base = base;
    return ty;
}

static Type *array_of(Type *base, int len) {
    if (type_pool_idx >= MAX_TYPES) type_pool_idx = 0;
    Type *ty = &type_pool[type_pool_idx++];
    memset(ty, 0, sizeof(Type));
    ty->kind = TY_ARRAY;
    ty->base = base;
    ty->size = base->size * len;
    ty->array_len = len;
    return ty;
}

static Obj *locals = NULL;

static Node *new_node(NodeKind kind) {
    if (node_pool_idx >= MAX_NODES) {
        printk("[chibicc Error] Node pool exhausted!\n");
        node_pool_idx = 0;
    }
    Node *node = &node_pool[node_pool_idx++];
    memset(node, 0, sizeof(Node));
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

static Obj *new_var(const char *name, Type *ty) {
    if (obj_pool_idx >= MAX_OBJS) {
        printk("[chibicc Error] Obj pool exhausted!\n");
        obj_pool_idx = 0;
    }
    Obj *var = &obj_pool[obj_pool_idx++];
    memset(var, 0, sizeof(Obj));
    strncpy(var->name, name, 31);
    var->name[31] = '\0';
    var->ty = ty;
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

static Node *funcall(Token **rest, Token *tok) {
    Token *start = tok;
    tok = tok->next->next; // skip name and '('

    Node head = {0};
    Node *cur = &head;

    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        cur = cur->next = expr(&tok, tok);
    }
    *rest = skip(tok, ")");

    Node *node = new_node(ND_FUNCALL);
    int len = start->len < 31 ? start->len : 31;
    strncpy(node->funcname, start->loc, len);
    node->funcname[len] = '\0';
    node->args = head.next;
    return node;
}

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
        if (equal(tok->next, "(")) {
            return funcall(rest, tok);
        }

        Obj *var = find_var(tok);
        if (!var) {
            char namebuf[32];
            int len = tok->len < 31 ? tok->len : 31;
            strncpy(namebuf, tok->loc, len);
            namebuf[len] = '\0';
            var = new_var(namebuf, ty_int);
        }
        Node *node = new_node(ND_VAR);
        node->var = var;
        tok = tok->next;

        // Array indexing arr[idx] -> *(arr + idx * elem_size)
        if (equal(tok, "[")) {
            tok = tok->next;
            Node *idx = expr(&tok, tok);
            tok = skip(tok, "]");

            int elem_size = 8;
            if (node->var && node->var->ty) {
                if (node->var->ty->kind == TY_ARRAY && node->var->ty->base) {
                    elem_size = node->var->ty->base->size;
                } else if (node->var->ty->kind == TY_PTR && node->var->ty->base) {
                    elem_size = node->var->ty->base->size;
                }
            }
            if (elem_size > 1) {
                idx = new_binary(ND_MUL, idx, new_num(elem_size));
            }
            Node *add_node = new_binary(ND_ADD, node, idx);
            node = new_binary(ND_DEREF, add_node, NULL);
        }

        *rest = tok;
        return node;
    }

    printk("[chibicc Error] Expected expression\n");
    *rest = tok->next;
    return new_num(0);
}

static Node *unary(Token **rest, Token *tok) {
    if (equal(tok, "+")) return unary(rest, tok->next);
    if (equal(tok, "-")) return new_binary(ND_NEG, unary(rest, tok->next), new_num(0));
    if (equal(tok, "&")) return new_binary(ND_ADDR, unary(rest, tok->next), NULL);
    if (equal(tok, "*")) return new_binary(ND_DEREF, unary(rest, tok->next), NULL);
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
        if (equal(tok, "%")) {
            node = new_binary(ND_MOD, node, unary(&tok, tok->next));
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

    if (equal(tok, "if")) {
        Node *node = new_node(ND_IF);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        if (equal(tok, "else")) {
            node->els = stmt(&tok, tok->next);
        }
        *rest = tok;
        return node;
    }

    if (equal(tok, "while")) {
        Node *node = new_node(ND_FOR);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        *rest = tok;
        return node;
    }

    if (equal(tok, "for")) {
        Node *node = new_node(ND_FOR);
        tok = skip(tok->next, "(");
        if (!equal(tok, ";")) node->init = stmt(&tok, tok);
        else tok = skip(tok, ";");
        if (!equal(tok, ";")) node->cond = expr(&tok, tok);
        tok = skip(tok, ";");
        if (!equal(tok, ")")) node->inc = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        *rest = tok;
        return node;
    }

    if (equal(tok, "int")) {
        Type *ty = ty_int;
        tok = tok->next; // skip "int"
        while (equal(tok, "*")) {
            ty = pointer_to(ty);
            tok = tok->next;
        }

        Token *var_tok = tok;
        tok = tok->next;

        // Array declaration int a[10]
        if (equal(tok, "[")) {
            tok = tok->next;
            int len = tok->val;
            tok = skip(tok->next, "]");
            ty = array_of(ty, len);
        }

        Obj *var = find_var(var_tok);
        if (!var) {
            char namebuf[32];
            int len = var_tok->len < 31 ? var_tok->len : 31;
            strncpy(namebuf, var_tok->loc, len);
            namebuf[len] = '\0';
            var = new_var(namebuf, ty);
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
    Node *node = new_node(ND_BLOCK);
    node->body = head.next;
    return node;
}

static Function *function(Token **rest, Token *tok) {
    node_pool_idx = 0;
    obj_pool_idx = 0;
    type_pool_idx = 0;
    locals = NULL;

    tok = skip(tok, "int");
    Token *fn_tok = tok;
    tok = tok->next;
    tok = skip(tok, "(");

    Obj head = {0};
    Obj *cur_param = &head;

    while (!equal(tok, ")")) {
        if (cur_param != &head) tok = skip(tok, ",");
        Type *ty = ty_int;
        tok = skip(tok, "int");
        while (equal(tok, "*")) {
            ty = pointer_to(ty);
            tok = tok->next;
        }
        Token *param_tok = tok;
        tok = tok->next;

        char p_name[32];
        int len = param_tok->len < 31 ? param_tok->len : 31;
        strncpy(p_name, param_tok->loc, len);
        p_name[len] = '\0';

        Obj *p_var = new_var(p_name, ty);
        cur_param = cur_param->next = p_var;
    }
    tok = skip(tok, ")");
    tok = skip(tok, "{");

    if (fn_pool_idx >= MAX_FNS) fn_pool_idx = 0;
    Function *fn = &fn_pool[fn_pool_idx++];
    memset(fn, 0, sizeof(Function));

    int len = fn_tok->len < 31 ? fn_tok->len : 31;
    strncpy(fn->name, fn_tok->loc, len);
    fn->name[len] = '\0';

    fn->params = head.next;
    fn->body = compound_stmt(rest, tok);
    fn->locals = locals;

    int offset = 16;
    for (Obj *var = locals; var; var = var->next) {
        int sz = var->ty ? var->ty->size : 8;
        if (sz < 8) sz = 8;
        offset += sz;
        var->offset = -offset;
    }
    fn->stack_size = offset + 16;
    return fn;
}

Function *parse(Token *tok) {
    fn_pool_idx = 0;
    Function head = {0};
    Function *cur = &head;
    while (tok->kind != TK_EOF) {
        cur = cur->next = function(&tok, tok);
    }
    return head.next;
}
