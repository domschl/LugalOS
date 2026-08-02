/*
 * chibicc - A Small C Compiler
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#ifndef LUGALOS_USER_CHIBICC_H
#define LUGALOS_USER_CHIBICC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TK_IDENT,
    TK_PUNCT,
    TK_KEYWORD,
    TK_NUM,
    TK_EOF,
} TokenKind;

typedef struct Token Token;
struct Token {
    TokenKind kind;
    Token *next;
    long val;
    char *loc;
    int len;
};

typedef enum {
    ND_ADD,
    ND_SUB,
    ND_MUL,
    ND_DIV,
    ND_MOD,
    ND_NEG,
    ND_EQ,
    ND_NE,
    ND_LT,
    ND_LE,
    ND_ASSIGN,
    ND_ADDR,
    ND_DEREF,
    ND_MEMBER,
    ND_VAR,
    ND_NUM,
    ND_IF,
    ND_FOR,
    ND_BLOCK,
    ND_FUNCALL,
    ND_RETURN,
    ND_EXPR_STMT,
} NodeKind;

typedef struct Type Type;
typedef struct Member Member;
typedef struct Node Node;
typedef struct Obj Obj;
typedef struct Function Function;

typedef enum {
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
} TypeKind;

struct Member {
    Member *next;
    Type *ty;
    char name[32];
    int offset;
};

struct Type {
    TypeKind kind;
    int size;
    Type *base;
    int array_len;
    Member *members;
};

struct Obj {
    Obj *next;
    char name[32];
    Type *ty;
    int offset;
};

struct Node {
    NodeKind kind;
    Node *next;
    Node *lhs;
    Node *rhs;
    Node *body;

    // If or For statement
    Node *cond;
    Node *then;
    Node *els;
    Node *init;
    Node *inc;

    // Struct Member Access
    Member *member;

    // Function Call
    char funcname[32];
    Node *args;

    Type *ty;
    Obj *var;
    long val;
};

struct Function {
    Function *next;
    char name[32];
    Node *body;
    Obj *params;
    Obj *locals;
    int stack_size;
};

/* Global Types */
extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;

/* Function Prototypes */
Token *tokenize(char *input);
Function *parse(Token *tok);
int codegen(Function *prog, uint8_t *code_buf, int max_size);
int chibicc_compile(const char *src_path, const char *dst_elf_path);

#endif /* LUGALOS_USER_CHIBICC_H */
