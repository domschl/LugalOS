/*
 * chibicc - Recursive Descent Parser with String Literal Support
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "kernel/printk.h"
#include <string.h>

#if defined(CONFIG_BOARD_RP2350)
#define MAX_NODES 256
#define MAX_OBJS 128
#define MAX_TYPES 64
#else
#define MAX_NODES 2048
#define MAX_OBJS 512
#define MAX_TYPES 256
#endif

static Node *node_pool;
static int node_pool_idx = 0;

static Obj *obj_pool;
static int obj_pool_idx = 0;

#define MAX_FNS 64
static Function *fn_pool;
static int fn_pool_idx = 0;

static Type *type_pool;
static int type_pool_idx = 0;


#define MAX_MEMBERS 128
static Member *member_pool;
static int member_pool_idx = 0;

/* The trailing "" is Type's `name[32]`; spelled out rather than left to the
 * implicit zero so the initializer covers every field the struct has. */
Type ty_char_obj  = {TY_CHAR, 1, NULL, 0, NULL, ""};
Type ty_short_obj = {TY_SHORT, 2, NULL, 0, NULL, ""};
Type ty_int_obj   = {TY_INT, 4, NULL, 0, NULL, ""};
Type ty_long_obj  = {TY_LONG, 8, NULL, 0, NULL, ""};

Type *ty_char  = &ty_char_obj;
Type *ty_short = &ty_short_obj;
Type *ty_int   = &ty_int_obj;
Type *ty_long  = &ty_long_obj;

Obj *globals = NULL;

/* Reuse the last slot instead of wrapping to 0 on exhaustion: index 0 and
 * everything after it up to the current index is still referenced by
 * already-built AST/type nodes, so wrapping there would silently corrupt
 * them. Reusing the last slot only corrupts the newest, not-yet-linked
 * allocation, and chibicc_pool_exhausted stops chibicc_compile() from
 * emitting or persisting the (now unreliable) result. */
static void type_pool_exhausted(void) {
    printk("[chibicc Error] Type pool exhausted!\n");
    chibicc_pool_exhausted = true;
    type_pool_idx = MAX_TYPES - 1;
}

static Type *pointer_to(Type *base) {
    if (type_pool_idx >= MAX_TYPES) type_pool_exhausted();
    Type *ty = &type_pool[type_pool_idx++];
    memset(ty, 0, sizeof(Type));
    ty->kind = TY_PTR;
    ty->size = 8;
    ty->base = base;
    return ty;
}

static Type *array_of(Type *base, int len) {
    if (type_pool_idx >= MAX_TYPES) type_pool_exhausted();
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
        chibicc_pool_exhausted = true;
        node_pool_idx = MAX_NODES - 1;
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
        chibicc_pool_exhausted = true;
        obj_pool_idx = MAX_OBJS - 1;
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

static Obj *new_gvar(const char *name, Type *ty) {
    if (obj_pool_idx >= MAX_OBJS) {
        printk("[chibicc Error] Obj pool exhausted!\n");
        chibicc_pool_exhausted = true;
        obj_pool_idx = MAX_OBJS - 1;
    }
    Obj *var = &obj_pool[obj_pool_idx++];
    memset(var, 0, sizeof(Obj));
    strncpy(var->name, name, 31);
    var->name[31] = '\0';
    var->ty = ty;
    var->is_global = true;
    var->next = globals;
    globals = var;
    return var;
}

static Obj *find_var(Token *tok) {
    for (Obj *var = locals; var; var = var->next) {
        if ((int)strlen(var->name) == tok->len && strncmp(tok->loc, var->name, tok->len) == 0) {
            return var;
        }
    }
    for (Obj *var = globals; var; var = var->next) {
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
    if (equal(tok, op)) {
        return tok->next;
    }
    return tok;
}

/* Forward declarations */
static Type *typespec(Token **rest, Token *tok);
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

static Member *find_member(Type *ty, Token *tok) {
    for (Member *m = ty->members; m; m = m->next) {
        if ((int)strlen(m->name) == tok->len && strncmp(tok->loc, m->name, tok->len) == 0) {
            return m;
        }
    }
    return NULL;
}

static Type *struct_decl(Token **rest, Token *tok) {
    tok = skip(tok, "struct");

    Token *tag = NULL;
    if (tok->kind == TK_IDENT) {
        tag = tok;
        tok = tok->next;
    }

    if (tag && !equal(tok, "{")) {
        *rest = tok;
        char tag_name[32];
        int tlen = tag->len < 31 ? tag->len : 31;
        strncpy(tag_name, tag->loc, tlen);
        tag_name[tlen] = '\0';

        for (int i = 0; i < type_pool_idx; i++) {
            if (type_pool[i].kind == TY_STRUCT && strcmp(type_pool[i].name, tag_name) == 0) {
                return &type_pool[i];
            }
        }
        if (type_pool_idx >= MAX_TYPES) type_pool_exhausted();
        Type *ty = &type_pool[type_pool_idx++];
        memset(ty, 0, sizeof(Type));
        ty->kind = TY_STRUCT;
        strncpy(ty->name, tag_name, 31);
        ty->size = 16;
        return ty;
    }

    tok = skip(tok, "{");

    if (type_pool_idx >= MAX_TYPES) type_pool_exhausted();
    Type *ty = &type_pool[type_pool_idx++];
    memset(ty, 0, sizeof(Type));
    ty->kind = TY_STRUCT;
    if (tag) {
        int tlen = tag->len < 31 ? tag->len : 31;
        strncpy(ty->name, tag->loc, tlen);
        ty->name[tlen] = '\0';
    }

    Member head = {0};
    Member *cur = &head;
    int offset = 0;

    while (!equal(tok, "}")) {
        Type *m_ty = typespec(&tok, tok);
        Token *m_tok = tok;
        tok = tok->next;

        if (member_pool_idx >= MAX_MEMBERS) {
            printk("[chibicc Error] Member pool exhausted!\n");
            chibicc_pool_exhausted = true;
            member_pool_idx = MAX_MEMBERS - 1;
        }
        Member *m = &member_pool[member_pool_idx++];
        memset(m, 0, sizeof(Member));
        int len = m_tok->len < 31 ? m_tok->len : 31;
        strncpy(m->name, m_tok->loc, len);
        m->name[len] = '\0';
        m->ty = m_ty;

        int align = m_ty->size < 8 ? m_ty->size : 8;
        if (align > 1) {
            offset = (offset + align - 1) & ~(align - 1);
        }
        m->offset = offset;
        offset += m_ty->size;

        cur = cur->next = m;
        tok = skip(tok, ";");
    }

    *rest = skip(tok, "}");
    ty->members = head.next;
    ty->size = (offset + 7) & ~7;
    return ty;
}

static Type *typespec(Token **rest, Token *tok) {
    if (equal(tok, "void")) {
        *rest = tok->next;
        return ty_int;
    }
    if (equal(tok, "char")) {
        *rest = tok->next;
        return ty_char;
    }
    if (equal(tok, "short")) {
        *rest = tok->next;
        return ty_short;
    }
    if (equal(tok, "int")) {
        *rest = tok->next;
        return ty_int;
    }
    if (equal(tok, "long")) {
        *rest = tok->next;
        return ty_long;
    }
    if (equal(tok, "struct")) {
        return struct_decl(rest, tok);
    }
    *rest = tok;
    return ty_int;
}

static Node *funcall(Token **rest, Token *tok) {
    Token *start = tok;
    tok = tok->next->next; // skip name and '('

    Node head = {0};
    Node *cur = &head;

    while (!equal(tok, ")") && tok->kind != TK_EOF) {
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

static int str_label_idx = 0;

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

    if (tok->kind == TK_STR) {
        char labelbuf[32];
        labelbuf[0] = '.'; labelbuf[1] = 'L'; labelbuf[2] = 's'; labelbuf[3] = 't'; labelbuf[4] = 'r';
        labelbuf[5] = '0' + (str_label_idx % 10); labelbuf[6] = '\0';
        str_label_idx++;

        int len = strlen(tok->str) + 1;
        Obj *var = new_gvar(labelbuf, array_of(ty_char, len));
        var->init_data = tok->str;

        Node *node = new_node(ND_STR);
        node->var = var;
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

        while (1) {
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
                continue;
            }

            // Member access struct.member
            if (equal(tok, ".")) {
                tok = tok->next;
                Token *m_tok = tok;
                tok = tok->next;
                Member *m = NULL;
                if (node->var && node->var->ty && node->var->ty->kind == TY_STRUCT) {
                    m = find_member(node->var->ty, m_tok);
                }
                Node *m_node = new_node(ND_MEMBER);
                m_node->lhs = node;
                m_node->member = m;
                node = m_node;
                continue;
            }

            // Pointer member access struct_ptr->member -> (*struct_ptr).member
            if (equal(tok, "->")) {
                tok = tok->next;
                Token *m_tok = tok;
                tok = tok->next;
                Node *deref = new_binary(ND_DEREF, node, NULL);
                Member *m = NULL;
                if (node->var && node->var->ty && node->var->ty->kind == TY_PTR && node->var->ty->base) {
                    m = find_member(node->var->ty->base, m_tok);
                }
                Node *m_node = new_node(ND_MEMBER);
                m_node->lhs = deref;
                m_node->member = m;
                node = m_node;
                continue;
            }

            if (equal(tok, "++")) {
                tok = tok->next;
                Node *one = new_num(1);
                Node *add_node = new_binary(ND_ADD, node, one);
                node = new_binary(ND_ASSIGN, node, add_node);
                continue;
            }
            if (equal(tok, "--")) {
                tok = tok->next;
                Node *one = new_num(1);
                Node *sub_node = new_binary(ND_SUB, node, one);
                node = new_binary(ND_ASSIGN, node, sub_node);
                continue;
            }

            break;
        }

        *rest = tok;
        return node;
    }

    printk("[chibicc Error] Expected expression\n");
    *rest = tok->next;
    return new_num(0);
}

static Node *unary(Token **rest, Token *tok) {
    if (equal(tok, "++")) {
        Node *target = unary(rest, tok->next);
        Node *one = new_num(1);
        Node *add_node = new_binary(ND_ADD, target, one);
        return new_binary(ND_ASSIGN, target, add_node);
    }
    if (equal(tok, "--")) {
        Node *target = unary(rest, tok->next);
        Node *one = new_num(1);
        Node *sub_node = new_binary(ND_SUB, target, one);
        return new_binary(ND_ASSIGN, target, sub_node);
    }
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

static bool is_typename(Token *tok) {
    return equal(tok, "char") || equal(tok, "short") ||
           equal(tok, "int")  || equal(tok, "long") ||
           equal(tok, "struct");
}

static Node *stmt(Token **rest, Token *tok) {
    if (equal(tok, "return")) {
        Node *node = new_node(ND_RETURN);
        node->lhs = expr(&tok, tok->next);
        *rest = skip(tok, ";");
        return node;
    }

    if (equal(tok, "break")) {
        Node *node = new_node(ND_BREAK);
        *rest = skip(tok->next, ";");
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

    if (is_typename(tok)) {
        Type *ty = typespec(&tok, tok);

        if (equal(tok, ";")) {
            *rest = tok->next;
            return new_node(ND_EXPR_STMT);
        }

        while (equal(tok, "*")) {
            ty = pointer_to(ty);
            tok = tok->next;
        }

        Token *var_tok = tok;
        tok = tok->next;

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
    while (!equal(tok, "}") && tok->kind != TK_EOF) {
        Token *old_tok = tok;
        cur = cur->next = stmt(&tok, tok);
        if (tok == old_tok && tok->kind != TK_EOF) {
            tok = tok->next;
        }
    }
    if (equal(tok, "}")) tok = tok->next;
    *rest = tok;
    Node *node = new_node(ND_BLOCK);
    node->body = head.next;
    return node;
}

static Function *function(Token **rest, Token *tok) {
    node_pool_idx = 0;
    obj_pool_idx = 0;
    locals = NULL;

    Type *ret_ty = typespec(&tok, tok);
    (void)ret_ty;

    Token *fn_tok = tok;
    tok = tok->next;
    tok = skip(tok, "(");

    Obj head = {0};
    Obj *cur_param = &head;

    while (!equal(tok, ")") && tok->kind != TK_EOF) {
        if (cur_param != &head) tok = skip(tok, ",");
        Type *ty = typespec(&tok, tok);
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
    if (equal(tok, ";")) {
        *rest = tok->next;
        return NULL;
    }
    tok = skip(tok, "{");

    if (fn_pool_idx >= MAX_FNS) {
        printk("[chibicc Error] Function pool exhausted!\n");
        chibicc_pool_exhausted = true;
        fn_pool_idx = MAX_FNS - 1;
    }
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
    type_pool_idx = 0;
    member_pool_idx = 0;
    globals = NULL;
    str_label_idx = 0;

    Function head = {0};
    Function *cur = &head;

    while (tok->kind != TK_EOF) {
        Token *old_tok = tok;
        if (equal(tok, "struct")) {
            Token *start = tok;
            Token *t = tok->next;
            if (t->kind == TK_IDENT) t = t->next;
            if (equal(t, "{")) {
                typespec(&tok, start);
                tok = skip(tok, ";");
                continue;
            }
        }
        Function *fn = function(&tok, tok);
        if (fn) cur = cur->next = fn;
        if (tok == old_tok && tok->kind != TK_EOF) {
            tok = tok->next;
        }
    }
    return head.next;
}

/* Arena-backed (C6): see user/chibicc/pools.c. */
bool parse_pools_init(void) {
    node_pool = (Node *)chibicc_pool_alloc(sizeof(Node) * MAX_NODES);
    obj_pool = (Obj *)chibicc_pool_alloc(sizeof(Obj) * MAX_OBJS);
    fn_pool = (Function *)chibicc_pool_alloc(sizeof(Function) * MAX_FNS);
    type_pool = (Type *)chibicc_pool_alloc(sizeof(Type) * MAX_TYPES);
    member_pool = (Member *)chibicc_pool_alloc(sizeof(Member) * MAX_MEMBERS);
    return node_pool && obj_pool && fn_pool && type_pool && member_pool;
}

void parse_pools_clear(void) {
    node_pool = NULL;
    obj_pool = NULL;
    fn_pool = NULL;
    type_pool = NULL;
    member_pool = NULL;
}

uint32_t parse_pools_bytes(void) {
    return (uint32_t)(sizeof(Node) * MAX_NODES) + (uint32_t)(sizeof(Obj) * MAX_OBJS) +
           (uint32_t)(sizeof(Function) * MAX_FNS) + (uint32_t)(sizeof(Type) * MAX_TYPES) +
           (uint32_t)(sizeof(Member) * MAX_MEMBERS);
}
