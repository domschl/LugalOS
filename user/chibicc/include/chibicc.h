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
    TK_STR,
    TK_EOF,
} TokenKind;

typedef struct Token Token;
struct Token {
    TokenKind kind;
    Token *next;
    long val;
    char *loc;
    int len;
    char *str;
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
    ND_STR,
    ND_IF,
    ND_FOR,
    ND_BLOCK,
    ND_FUNCALL,
    ND_RETURN,
    ND_BREAK,
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
    char name[32];
};

struct Obj {
    /* The `locals` chain, built by new_var() as it prepends each new
     * variable. */
    Obj *next;
    /* The parameter chain of the function being parsed, kept separate from
     * `next` on purpose.
     *
     * Both lists used to share `next`, which works for exactly as long as a
     * function has at most one parameter. With two, new_var() prepends `b`
     * to locals (b->next = a) and the parameter walk then links a->next = b,
     * so the two point at each other: a cycle, which the stack-offset loop
     * in parse.c walks until its `int` overflows and UBSan halts the board.
     * `int main(int a, int b)` was enough to do it. */
    Obj *param_next;
    char name[32];
    Type *ty;
    int offset;
    bool is_global;
    char *init_data;
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
    int code_offset;
};

/* Global Variables & Types */
extern Type *ty_char;
extern Type *ty_short;
extern Type *ty_int;
extern Type *ty_long;

extern Obj *globals;

/* Set when any fixed-size compiler pool (tokens, nodes, objs, types,
 * members, functions, string literal buffers) runs out of room mid-compile.
 * When this happens the pool allocators clamp to their last slot instead of
 * wrapping back to index 0 (which would silently alias and corrupt AST
 * nodes built earlier in the same compilation), so compilation can finish
 * without a memory-safety violation -- but the result is not trustworthy.
 * chibicc_compile() checks this after parsing and refuses to emit or write
 * out a binary if it's set. Reset at the start of every tokenize() call. */
extern bool chibicc_pool_exhausted;

/* Function Prototypes */
char *preprocess(const char *input);
Token *tokenize(char *input);
Function *parse(Token *tok);
int codegen(Function *prog, uint8_t *code_buf, int max_size);
int chibicc_compile(const char *src_path, const char *dst_elf_path);

/* --- Working memory (C6, plan/phase6_memory_and_processes.md) ---
 *
 * chibicc's pools live on the heap for the duration of a compile and are
 * returned afterwards, so an idle compiler costs nothing. See
 * user/chibicc/pools.c for why this rather than making cc a user process. */
bool chibicc_pools_acquire(void);
void chibicc_pools_release(void);
void *chibicc_pool_alloc(uint32_t bytes);

/* Each module claims its own pools out of the arena. Called only by
 * chibicc_pools_acquire()/release(). */
uint32_t tokenize_pools_bytes(void);
bool tokenize_pools_init(void);
void tokenize_pools_clear(void);
uint32_t parse_pools_bytes(void);
bool parse_pools_init(void);
void parse_pools_clear(void);
uint32_t preprocess_pools_bytes(void);
bool preprocess_pools_init(void);
void preprocess_pools_clear(void);
uint32_t main_pools_bytes(void);
bool main_pools_init(void);
void main_pools_clear(void);

#endif /* LUGALOS_USER_CHIBICC_H */
