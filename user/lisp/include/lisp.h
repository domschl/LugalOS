#ifndef LUGALOS_USER_LISP_H
#define LUGALOS_USER_LISP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LISP_NIL,
    LISP_INT,
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
        char sym[32];
        struct {
            struct lisp_val *car;
            struct lisp_val *cdr;
        } pair;
        lisp_prim_fn prim;
        struct {
            struct lisp_val *params;
            struct lisp_val *body;
            struct lisp_val *env;
        } lambda;
    } u;
} lisp_val_t;

void lisp_init(void);
lisp_val_t *lisp_eval(lisp_val_t *val, lisp_val_t *env);
lisp_val_t *lisp_read(const char **str);
void lisp_print(lisp_val_t *val);
void lisp_repl(void);

#endif /* LUGALOS_USER_LISP_H */
