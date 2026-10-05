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
    LISP_LAMBDA,
    LISP_BIGNUM,
    LISP_RATIO
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
        struct {
            char *sym;  /* LISP_SYMBOL: same pool as str */
            struct lisp_val *sym_next; /* intrusive symbol hash bucket chain */
        };
        struct {
            struct lisp_val *car;
            struct lisp_val *cdr;
        } pair;
        struct {
            int16_t sign;       /* +1 or -1 */
            uint16_t len;       /* number of limbs (1..32) */
            uint16_t capacity;  /* 8 or 32 */
            uint32_t *limbs;    /* limbs array in little-endian order */
        } bignum;
        struct {
            struct lisp_val *num; /* LISP_INT or LISP_BIGNUM */
            struct lisp_val *den; /* LISP_INT or LISP_BIGNUM (> 1) */
        } ratio;
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

/* 37.3b, plan/phase37_screen_layouts_and_apps.md §2.2: at a prompt, call
 * the program's (canvas-on-redraw f) if the canvas was lost since it last
 * drew; and put the screen back to text (after an `exec`'d program or a
 * chess session). Both no-ops where Lisp's canvas is the ST7735's. */
void lisp_canvas_poll(void);
void lisp_canvas_reset(void);
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

/* S3 (plan/phase13_lisp_engine_extensions.md): the collector's one safe
 * point, exposed so any genuinely top-level, non-nested per-command
 * dispatch loop can call it between commands -- not just lisp_repl()'s own
 * loop internally. kernel/shell.c's POSIX-shell command loop
 * (parse_and_eval_cmd() via lisp_eval_string()) is exactly this shape: a
 * fresh top-level form each iteration, driven by raw interactive input,
 * never itself nested inside another expression's still-in-progress
 * evaluation. See gc_collect()'s own comment (user/lisp/lisp.c) for why a
 * collection is only exact right here and nowhere mid-expression. */
void lisp_gc_safepoint(void);

/* Phase 44: held around every evaluation from outside the engine, from
 * lisp_eval_string() to the lisp_print() of its value -- the value is
 * garbage to the next collection until printed. Re-entrant per task. See
 * g_lisp_lock in user/lisp/lisp.c. */
void lisp_lock(void);
void lisp_unlock(void);

/* A primitive that becomes an interactive session -- a game, the chess
 * console, an appliance loop -- runs for minutes inside the form that
 * called it. Holding g_lisp_lock that long parks every other terminal's
 * next command behind it (the vterm freeze: `chess` in one terminal, `ls`
 * in a second, and no terminal takes input again). Dropping the lock with
 * a bare lisp_unlock() is no better: the session's own half-evaluated
 * form lives only on its task's stack, which no other task's collection
 * scans, so the first collection elsewhere frees it.
 *
 * lisp_park() does both halves: it records the caller's stack, from its
 * own frame up, and its callee-saved registers as collection roots, then
 * releases the lock entirely, however deep this task holds it.
 * lisp_unpark() takes the lock back to that same depth and drops the
 * roots. The record lives in the caller's frame, between the two calls;
 * the session may take lisp_lock() around its own evaluations meanwhile. */
typedef struct lisp_park {
    struct lisp_park *next;
    uintptr_t lo, hi;          /* the parked stack range, [lo, hi) */
    uintptr_t regs[12];        /* s0..s11 at the moment of parking */
    int depth;                 /* g_lisp_lock depth to restore */
} lisp_park_t;

void lisp_park(lisp_park_t *p);
void lisp_unpark(lisp_park_t *p);


#endif /* LUGALOS_USER_LISP_H */
