#ifndef LUGALOS_USER_LISP_H
#define LUGALOS_USER_LISP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LISP_NIL,
    LISP_INT,
    LISP_STRING,
    LISP_SYMBOL,
    LISP_PAIR,
    LISP_PRIMITIVE,
    LISP_LAMBDA
} lisp_type_t;

struct lisp_val;

typedef struct lisp_val *(*lisp_prim_fn)(struct lisp_val *args, struct lisp_val *env);

typedef struct lisp_val {
    lisp_type_t type;
    union {
        long i;
        char *str;  /* LISP_STRING: pointer into the interned string pool
                     * (see string_pool in user/lisp/lisp.c) rather than an
                     * inline buffer -- keeping text out of the union means
                     * pair/int nodes, the majority of allocations, aren't
                     * each paying for 128 bytes of string capacity they
                     * never use (see V6 in
                     * plan/completed/2026-08-07_review_and_remediation.md). */
        char *sym;  /* LISP_SYMBOL: same pool as str */
        struct {
            struct lisp_val *car;
            struct lisp_val *cdr;
        } pair;
        lisp_prim_fn prim;
        struct {
            struct lisp_val *params;
            struct lisp_val *body; /* list of body forms, evaluated in
                                     * sequence like `begin` -- not just a
                                     * single expression */
            struct lisp_val *env;  /* NULL means "was defined directly in
                                     * the global scope": resolved against
                                     * the live global environment at call
                                     * time rather than a frozen snapshot,
                                     * which is what makes self-recursion
                                     * work (see B3 in
                                     * plan/completed/2026-08-07_review_and_remediation.md) */
        } lambda;
    } u;
} lisp_val_t;

void lisp_init(void);
lisp_val_t *make_int(long val);
lisp_val_t *make_str(const char *str);
lisp_val_t *make_sym(const char *sym);
lisp_val_t *make_pair(lisp_val_t *car, lisp_val_t *cdr);
lisp_val_t *make_prim(lisp_prim_fn fn);

/* Safe argument-list accessors for primitives (see user/lisp/lisp.c for the
 * rationale). Every primitive should read its arguments through these rather
 * than walking args->u.pair.cdr->u.pair.car chains directly. */
int lisp_list_len(lisp_val_t *args);
lisp_val_t *lisp_list_ref(lisp_val_t *args, int n);
const char *get_str_val(lisp_val_t *val);

lisp_val_t *lisp_eval(lisp_val_t *val, lisp_val_t *env);
lisp_val_t *lisp_eval_string(const char *str);
lisp_val_t *lisp_read(const char **str);
void lisp_print(lisp_val_t *val);
void lisp_repl(void);


#endif /* LUGALOS_USER_LISP_H */
