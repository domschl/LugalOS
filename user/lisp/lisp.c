#include "lisp.h"
#include "kernel/printk.h"
#include "kernel/path.h"
#include "kernel/console.h"
#include "kernel/shell.h"
#include "kernel/time.h"
#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "drivers/loopback_net.h"
#include "drivers/uart_net.h"
#include "drivers/usb_cdc.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "kernel/device.h"
#include "kernel/klog.h"
#include "kernel/sched.h"
#include "lugalos_config.h"
#if CONFIG_ENABLE_CC
#include "user/chibicc/include/chibicc.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
#include "drivers/st7735.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
#include "drivers/tm1638.h"
#endif
#if CONFIG_ENABLE_CHESS
#include "chess_ui.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
#include "drivers/pico_clock_green.h"
#endif
#include "arch/elf.h"
#include <string.h>


#if defined(CONFIG_BOARD_RP2350)
/* 512 was already close to the ceiling a full hardware test-suite run
 * reaches within one boot session -- H1+H2 (plan/phase9_chess_computer.md)
 * added 7 new global primitive bindings (canvas-*, tm-*) and tipped it into
 * exhaustion partway through tests/hw/test_rp2350.py. 768 restored
 * headroom then; S4 (plan/phase13_lisp_engine_extensions.md) tipped it
 * again by registering ~38 new stdlib primitives, found live the same way
 * H1+H2 was -- test_rp2350.py's C2 failed with "Node pool exhausted"
 * partway through an unrelated command, not because of anything wrong with
 * the new primitives themselves, but because global_env's accumulated,
 * still-*reachable* bindings across ~20 hardware tests plus 38 more
 * permanent boot-time registrations left too little headroom. S3's
 * collector (added the phase before S4) does not change this calculus: it
 * reclaims genuine garbage, not live definitions still hanging off
 * global_env, and every one of those ~20 tests' own top-level `define`s is
 * exactly that -- live, by construction, for the rest of the session.
 * 1024 restores headroom again; it's a static array outside palloc's
 * managed pages, so the RAM cost is a few more KB against 520 KB of
 * physical SRAM, not a fraction of the page budget. */
#define NODE_POOL_SIZE 1024
#else
#define NODE_POOL_SIZE 4096
#endif



static lisp_val_t node_pool[NODE_POOL_SIZE];

static int node_pool_idx = 0;
static bool node_pool_exhausted_warned = false;

/* S3 (plan/phase13_lisp_engine_extensions.md): free-list head for reclaimed
 * node_pool cells, threaded through the existing pair.cdr field (no extra
 * storage) -- see gc_collect() further down for how cells get here. NULL
 * means empty, i.e. every collection so far has reclaimed nothing new. */
static lisp_val_t *node_free_list = NULL;

/* Guards against unbounded C-stack recursion in lisp_eval() (see its
 * definition near the bottom of this file for the full rationale). */
#define LISP_MAX_EVAL_DEPTH 100
static int eval_depth = 0;
static bool eval_depth_exceeded_warned = false;

/* J2 (plan/phase10_chess_completion.md): Ctrl-C for a run-away-but-legal
 * evaluation -- eval_depth above only catches unbounded *recursion*, not a
 * legitimately bounded-depth call that is simply expensive (an
 * exponential-blowup recursive definition well under the depth-100
 * ceiling can still run for a long time). See lisp_eval()'s own use of
 * these for the full mechanism. */
static unsigned long eval_poll_count = 0;
static bool lisp_interrupted = false;

static lisp_val_t nil_val = { .type = LISP_NIL };
static lisp_val_t true_val = { .type = LISP_SYMBOL, .u.sym = "#t" };
static lisp_val_t false_val = { .type = LISP_SYMBOL, .u.sym = "#f" };

static lisp_val_t *global_env = &nil_val;

static int streq(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2 == 0;
}

static void strncpy_local(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Node allocation */
static lisp_val_t *alloc_node(lisp_type_t type) {
    /* S3: reuse a reclaimed cell before ever touching the bump cursor.
     * Always safe to do here regardless of *when* alloc_node() is called
     * (mid-expression or not) -- unlike running a collection itself (see
     * gc_collect()'s comment for why that's restricted to safe points),
     * handing out a cell a *previous, already-completed* collection
     * proved unreachable doesn't depend on anything about the current
     * call's rooting. */
    if (node_free_list) {
        lisp_val_t *v = node_free_list;
        node_free_list = v->u.pair.cdr;
        v->type = type;
        return v;
    }
    if (node_pool_idx >= NODE_POOL_SIZE) {
        /* Do NOT wrap back to index 0: global_env and every value evaluated
         * so far are chains of nodes drawn from this pool, and index 0 is
         * the oldest, still-live end of it. Wrapping there would silently
         * overwrite bound primitives and the user's own definitions rather
         * than just running out cleanly (see B6 in
         * plan/completed/2026-08-07_review_and_remediation.md). Clamp to the last slot
         * instead: further allocations alias each other and produce wrong
         * results, but no longer corrupt the environment or earlier values.
         *
         * "Wrong results" understated it, and the difference cost a hung
         * board. Two allocations from the clamped slot are the *same node*,
         * so `cell->cdr = alloc_node(...)` makes a cons cell point at itself
         * -- a cyclic list. Every list walker here then runs forever, and
         * prim_add() accumulates into `sum` while it does. On QEMU that is a
         * signed-overflow UBSan trap with a message; on RP2350, which builds
         * without UBSan, it is an unrecoverable spin: no fault, no output,
         * USB never serviced again, physical replug required.
         *
         * The clamp is kept -- allocation still has to return something
         * writable -- but it is no longer load-bearing, because lisp_eval()
         * refuses to descend once this flag is set. Nothing gets built out of
         * the aliased node. */
        if (!node_pool_exhausted_warned) {
            printk("[Lisp Error] Node pool exhausted! Further evaluation will "
                   "produce wrong results until the shell restarts.\n");
            node_pool_exhausted_warned = true;
        }
        node_pool_idx = NODE_POOL_SIZE - 1;
    }
    lisp_val_t *v = &node_pool[node_pool_idx++];
    v->type = type;
    return v;
}

/* Interned string/symbol storage, separate from node_pool (see the lambda
 * struct comment in lisp.h for the motivation). Sized as a fraction of
 * NODE_POOL_SIZE rather than matching it 1:1: env_set() alone allocates one
 * symbol per binding (every `define`, every `let` binding, every function
 * call's parameter binding), so on typical usage a meaningful fraction but
 * not the majority of node allocations are strings/symbols -- reserving a
 * full string_pool slot for every possible node would erase the memory
 * savings this pool split exists to capture. */
#define STRING_POOL_SIZE (NODE_POOL_SIZE / 2)
#define STRING_SLOT_LEN 128

static char string_pool[STRING_POOL_SIZE][STRING_SLOT_LEN];
static int string_pool_idx = 0;
static bool string_pool_exhausted_warned = false;

/* S3: free-list head for reclaimed string_pool slots, threaded through the
 * slot's own leading bytes (reinterpreted as an int index, -1 = end of
 * list) rather than a separate side array -- a slot is either on this
 * list (its content is the "next free" index) or handed out (its content
 * is whatever string was written into it); nothing reads a slot's bytes as
 * both at once. -1 means empty. */
static int string_free_head = -1;

static char *alloc_string_slot(void) {
    if (string_free_head >= 0) {
        int idx = string_free_head;
        int next;
        memcpy(&next, string_pool[idx], sizeof(next));
        string_free_head = next;
        return string_pool[idx];
    }
    if (string_pool_idx >= STRING_POOL_SIZE) {
        /* Same clamp-not-wrap policy as alloc_node() (see B6 in
         * plan/completed/2026-08-07_review_and_remediation.md): reusing slot 0 would
         * silently corrupt whatever still-live value points at it, e.g. the
         * name of a primitive bound in global_env. */
        if (!string_pool_exhausted_warned) {
            printk("[Lisp Error] String pool exhausted! Further strings/symbols will alias.\n");
            string_pool_exhausted_warned = true;
        }
        string_pool_idx = STRING_POOL_SIZE - 1;
    }
    return string_pool[string_pool_idx++];
}

/* S3 (plan/phase13_lisp_engine_extensions.md): mark-sweep collector, per
 * the design and host-timing prototype recorded in S0. Precise, cooperative,
 * safe-point-only collection -- see lisp_eval()'s own call site (near the
 * bottom of this file) for exactly where "safe point" applies and why it
 * can only mean "between complete top-level forms," never mid-expression.
 *
 * One bit per node_pool/string_pool slot, a separate bitmap rather than a
 * stolen struct field, so the already-tuned 32-byte lisp_val_t layout (V6,
 * plan/completed/2026-08-07_review_and_remediation.md) stays untouched. */
#define NODE_MARK_BITS_SIZE ((NODE_POOL_SIZE + 7) / 8)
#define STRING_MARK_BITS_SIZE ((STRING_POOL_SIZE + 7) / 8)
static uint8_t node_mark_bits[NODE_MARK_BITS_SIZE];
static uint8_t string_mark_bits[STRING_MARK_BITS_SIZE];

/* Explicit, array-based work-list instead of C recursion for the mark
 * phase: a live structure can be an arbitrarily long list or deeply nested
 * (a long `let*` chain, a big `quote`d list), and marking it via a
 * recursive C function would risk overflowing the very C stack this whole
 * collector exists to relieve allocation pressure on -- collecting
 * shouldn't itself become a way to crash the board. gc_push() marks a node
 * the instant it is pushed, not when it is popped, so a node already
 * marked is never pushed again; that is what guarantees this array never
 * needs to hold more than NODE_POOL_SIZE entries no matter how bushy the
 * live graph is, so it can be sized exactly and statically rather than
 * guessed. */
static lisp_val_t *gc_work_stack[NODE_POOL_SIZE];

static int string_slot_index(const char *slot) {
    if (!slot) return -1;
    const char *base = &string_pool[0][0];
    if (slot < base || slot >= base + (size_t)STRING_POOL_SIZE * STRING_SLOT_LEN) return -1;
    return (int)((slot - base) / STRING_SLOT_LEN);
}

static void gc_mark_string_slot(const char *slot) {
    int idx = string_slot_index(slot);
    if (idx < 0) return;
    string_mark_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static void gc_push(int *sp, lisp_val_t *v) {
    if (!v) return;
    /* nil_val/true_val/false_val are static sentinels declared outside
     * node_pool, never allocated via alloc_node() -- no mark bit exists
     * for them and none is needed, since they're permanent and never
     * reclaimed. */
    if (v < node_pool || v >= node_pool + NODE_POOL_SIZE) return;
    long idx = v - node_pool;
    uint8_t bit = (uint8_t)(1u << (idx % 8));
    if (node_mark_bits[idx / 8] & bit) return; /* already marked/pushed/processed */
    node_mark_bits[idx / 8] |= bit;
    gc_work_stack[(*sp)++] = v;
}

static void gc_mark(lisp_val_t *root) {
    int sp = 0;
    gc_push(&sp, root);
    while (sp > 0) {
        lisp_val_t *v = gc_work_stack[--sp];
        switch (v->type) {
            case LISP_STRING:
            case LISP_SYMBOL:
                gc_mark_string_slot(v->u.str);
                break;
            case LISP_PAIR:
                gc_push(&sp, v->u.pair.car);
                gc_push(&sp, v->u.pair.cdr);
                break;
            case LISP_LAMBDA:
                gc_push(&sp, v->u.lambda.params);
                gc_push(&sp, v->u.lambda.body);
                /* NULL means "resolve against live global_env at call
                 * time" (B3) -- nothing extra to mark, global_env is
                 * already this collection's root. */
                if (v->u.lambda.env) gc_push(&sp, v->u.lambda.env);
                break;
            default:
                break; /* LISP_INT, LISP_PRIMITIVE, LISP_NIL: no children */
        }
    }
}

/* Runs one full stop-the-world mark-sweep pass. Safe to call ONLY from
 * lisp_eval()'s eval_depth==0 entry (see its call site) -- global_env is
 * the only root this collector uses, so anything alive purely on the C
 * stack at any other point (a partially built argument list, an `if`'s
 * just-computed condition, a lambda call's local_env mid-construction,
 * the very AST of whatever top-level form is still executing) would be
 * invisible to this mark phase and could be reclaimed out from under the
 * evaluation still using it. Between complete top-level forms, nothing but
 * global_env matters -- every C stack frame from the form that just
 * finished has already returned -- so rooting from it alone is exact
 * there, not approximate.
 *
 * One direct, load-bearing consequence: a single top-level form that
 * itself exhausts a pool while consing (e.g. one huge non-terminating
 * loop) still aborts that form exactly as before S3 -- there is no safe
 * point inside it to collect at. What S3 actually buys is the *next*
 * top-level form no longer inheriting a permanently degraded shell: this
 * pass runs before it, and any garbage the previous (aborted or merely
 * finished) form left behind becomes reclaimable capacity again.
 *
 * Frees nothing by itself if nothing is garbage: node_pool_exhausted_warned
 * / string_pool_exhausted_warned are cleared below only if this pass
 * actually grew the corresponding free list, so a session with genuinely
 * no reclaimable garbage still degrades to nil exactly as it did before
 * S3, rather than retrying a hopeless collection on every later form. */
static void gc_collect(void) {
    memset(node_mark_bits, 0, sizeof(node_mark_bits));
    memset(string_mark_bits, 0, sizeof(string_mark_bits));

    /* Cells already on a free list are unreachable by definition, but must
     * not be re-added to that list during the sweep below -- doing so
     * would grow it a cycle or a duplicate entry. Pre-marking them here
     * makes the sweep treat "already free" exactly like "still live":
     * either way, leave it alone. */
    for (lisp_val_t *c = node_free_list; c; c = c->u.pair.cdr) {
        long idx = c - node_pool;
        node_mark_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
    }
    for (int idx = string_free_head; idx >= 0;) {
        string_mark_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
        int next;
        memcpy(&next, string_pool[idx], sizeof(next));
        idx = next;
    }

    gc_mark(global_env);

    for (int i = 0; i < node_pool_idx; i++) {
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (!(node_mark_bits[i / 8] & bit)) {
            node_pool[i].u.pair.cdr = node_free_list;
            node_free_list = &node_pool[i];
        }
    }
    if (node_free_list) node_pool_exhausted_warned = false;

    for (int i = 0; i < string_pool_idx; i++) {
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (!(string_mark_bits[i / 8] & bit)) {
            memcpy(string_pool[i], &string_free_head, sizeof(string_free_head));
            string_free_head = i;
        }
    }
    if (string_free_head >= 0) string_pool_exhausted_warned = false;
}

/* Public wrapper (declared in lisp.h) -- see its own comment there for who
 * calls this and why each call site is a genuine safe point. */
void lisp_gc_safepoint(void) {
    if (node_pool_exhausted_warned || string_pool_exhausted_warned) {
        gc_collect();
    }
}

lisp_val_t *make_int(long val) {
    lisp_val_t *v = alloc_node(LISP_INT);
    v->u.i = val;
    return v;
}

lisp_val_t *make_str(const char *str) {
    lisp_val_t *v = alloc_node(LISP_STRING);
    char *slot = alloc_string_slot();
    strncpy_local(slot, str ? str : "", STRING_SLOT_LEN);
    v->u.str = slot;
    return v;
}

lisp_val_t *make_sym(const char *sym) {
    lisp_val_t *v = alloc_node(LISP_SYMBOL);
    char *slot = alloc_string_slot();
    strncpy_local(slot, sym, STRING_SLOT_LEN);
    v->u.sym = slot;
    return v;
}


lisp_val_t *make_pair(lisp_val_t *car, lisp_val_t *cdr) {
    lisp_val_t *v = alloc_node(LISP_PAIR);
    v->u.pair.car = car;
    v->u.pair.cdr = cdr;
    return v;
}

lisp_val_t *make_prim(lisp_prim_fn fn) {
    lisp_val_t *v = alloc_node(LISP_PRIMITIVE);
    v->u.prim = fn;
    return v;
}

/* Environment management */
static lisp_val_t *env_get(lisp_val_t *env, const char *sym) {
    for (lisp_val_t *curr = env; curr && curr->type == LISP_PAIR; curr = curr->u.pair.cdr) {
        lisp_val_t *binding = curr->u.pair.car;
        if (binding && binding->type == LISP_PAIR) {
            lisp_val_t *k = binding->u.pair.car;
            if (k && k->type == LISP_SYMBOL && streq(k->u.sym, sym)) {
                return binding->u.pair.cdr;
            }
        }
    }
    return NULL;
}

static void env_set(lisp_val_t **env, const char *sym, lisp_val_t *val) {
    lisp_val_t *binding = make_pair(make_sym(sym), val);
    *env = make_pair(binding, *env);
}

/* Safe argument-list accessors for primitives. `args` is the proper list of
 * already-evaluated call arguments built by lisp_eval, terminated by the
 * shared &nil_val node -- but a primitive can easily be called with fewer
 * arguments than it expects. &nil_val is a real, non-NULL pointer whose
 * type is LISP_NIL, not LISP_PAIR, so checking a cdr for "!= NULL" is not
 * enough to guarantee the next car is safe to read: that was exactly the
 * crash in e.g. (= 1), which read &nil_val's zero-initialized pair.car as a
 * live pointer and dereferenced it. lisp_list_ref is the one place that
 * checks the type, not just the pointer, before descending. */
int lisp_list_len(lisp_val_t *args) {
    int n = 0;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) n++;
    return n;
}

lisp_val_t *lisp_list_ref(lisp_val_t *args, int n) {
    lisp_val_t *c = args;
    for (int i = 0; i < n; i++) {
        if (!c || c->type != LISP_PAIR) return NULL;
        c = c->u.pair.cdr;
    }
    if (!c || c->type != LISP_PAIR) return NULL;
    return c->u.pair.car;
}

static long arg_int(lisp_val_t *args, int n, long default_val) {
    lisp_val_t *v = lisp_list_ref(args, n);
    return (v && v->type == LISP_INT) ? v->u.i : default_val;
}

/* S4 (plan/phase13_lisp_engine_extensions.md): the truthiness rule `if`/
 * `cond`/`while` each already had inline and identically -- anything except
 * #f (either the true_val/false_val singletons or a freshly read/quoted
 * symbol whose text is "#f", per the `(= #t #t)` comment on prim_eq below)
 * and nil is true. Factored out once there was a fourth, real user of it
 * (`filter`, further down) rather than duplicating it a fourth time. */
static bool lisp_truthy(lisp_val_t *v) {
    return v && v != &false_val &&
           !(v->type == LISP_SYMBOL && streq(v->u.sym, "#f")) &&
           !(v->type == LISP_NIL);
}

/* Forward declaration: lisp_apply() (S2) is defined much further down,
 * next to the evaluator it was factored out of, but map/filter/for-each/
 * apply below -- all added in S4 -- need to call it. */
static lisp_val_t *lisp_apply(lisp_val_t *fn, lisp_val_t *args, lisp_val_t *env);

/* Built-in primitives */
static lisp_val_t *prim_add(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long sum = 0;
    /* Bounded, belt-and-braces against a cyclic list. lisp_eval() now refuses
     * to build one (see alloc_node), but this loop is where a cycle actually
     * manifested -- spinning forever while accumulating into `sum` until the
     * signed overflow at this line -- and the bound is free and exactly
     * correct: a list cannot have more elements than the pool has nodes. */
    int guard = 0;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (c->u.pair.car->type == LISP_INT) {
            sum += c->u.pair.car->u.i;
        }
    }
    return make_int(sum);
}

static lisp_val_t *prim_sub(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    long res = arg_int(args, 0, 0);
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        if (c->u.pair.car->type == LISP_INT) {
            res -= c->u.pair.car->u.i;
        }
    }
    return make_int(res);
}

static lisp_val_t *prim_mul(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long prod = 1;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        if (c->u.pair.car->type == LISP_INT) {
            prod *= c->u.pair.car->u.i;
        }
    }
    return make_int(prod);
}

static bool lisp_values_equal(lisp_val_t *a, lisp_val_t *b) {
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
        case LISP_INT:
            return a->u.i == b->u.i;
        case LISP_STRING:
            return strcmp(a->u.str, b->u.str) == 0;
        case LISP_SYMBOL:
            /* #t/#f are themselves LISP_SYMBOL ("#t"/"#f"), so this also
             * makes (= #t #t) behave sensibly rather than always false. */
            return streq(a->u.sym, b->u.sym);
        default:
            return false;
    }
}

/* S4 (plan/phase13_lisp_engine_extensions.md): generalized from strictly
 * 2-arg to N-ary (all args equal to the first, which for equality is the
 * same thing as all-equal-to-each-other) -- everything else about it,
 * including cross-type behavior, is unchanged. */
static lisp_val_t *prim_eq(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int n = lisp_list_len(args);
    if (n < 2) return &false_val;
    lisp_val_t *first = lisp_list_ref(args, 0);
    for (int i = 1; i < n; i++) {
        if (!lisp_values_equal(first, lisp_list_ref(args, i))) return &false_val;
    }
    return &true_val;
}

/* S4: chained numeric comparison shared by </>/<=/>=, matching the
 * standard Scheme reading of e.g. (< 1 2 3) as "1<2 AND 2<3", not just
 * "first < last". Requires at least 2 int args, same as prim_eq requiring
 * at least 2 -- a single or missing argument returns #f rather than
 * vacuously #t, for consistency with the existing `=` behavior this
 * generalizes rather than chasing full R7RS single-argument semantics. */
static lisp_val_t *prim_chain_compare(lisp_val_t *args, bool (*cmp)(long, long)) {
    int n = lisp_list_len(args);
    if (n < 2) return &false_val;
    lisp_val_t *prev = lisp_list_ref(args, 0);
    if (!prev || prev->type != LISP_INT) return &false_val;
    long prev_val = prev->u.i;
    for (int i = 1; i < n; i++) {
        lisp_val_t *cur = lisp_list_ref(args, i);
        if (!cur || cur->type != LISP_INT) return &false_val;
        if (!cmp(prev_val, cur->u.i)) return &false_val;
        prev_val = cur->u.i;
    }
    return &true_val;
}

static bool cmp_lt(long a, long b) { return a < b; }
static bool cmp_gt(long a, long b) { return a > b; }
static bool cmp_le(long a, long b) { return a <= b; }
static bool cmp_ge(long a, long b) { return a >= b; }

static lisp_val_t *prim_lt(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_lt); }
static lisp_val_t *prim_gt(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_gt); }
static lisp_val_t *prim_le(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_le); }
static lisp_val_t *prim_ge(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_ge); }

/* (/= a b c ...) -- R7RS semantics are "every pair is different", not just
 * adjacent chaining like </>/<=/>=, since a<b<c doesn't imply a!=c the way
 * it implies a<c, but it DOES already imply a!=b and b!=c specifically --
 * "not equal" has no transitive shortcut the ordering relations get for
 * free. O(n^2) is fine: argument counts here are always small. */
static lisp_val_t *prim_ne(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int n = lisp_list_len(args);
    if (n < 2) return &false_val;
    for (int i = 0; i < n; i++) {
        lisp_val_t *a = lisp_list_ref(args, i);
        if (!a || a->type != LISP_INT) return &false_val;
        for (int j = i + 1; j < n; j++) {
            lisp_val_t *b = lisp_list_ref(args, j);
            if (!b || b->type != LISP_INT || a->u.i == b->u.i) return &false_val;
        }
    }
    return &true_val;
}

/* (/ a b c ...) -- chained integer division, matching prim_sub's shape.
 * Guards division by zero explicitly: this build runs with
 * -fsanitize=undefined -fno-sanitize-recover=all, so an actual C division
 * by zero here would be a fatal UBSan trap on QEMU and an unrecoverable
 * fault on RP2350, not a catchable error -- degrading to nil (the same
 * signal used elsewhere for malformed input, e.g. B1's arity fixes) is the
 * only acceptable outcome. */
static lisp_val_t *prim_div(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    long res = arg_int(args, 0, 0);
    int guard = 0;
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (c->u.pair.car->type == LISP_INT) {
            long divisor = c->u.pair.car->u.i;
            if (divisor == 0) return &nil_val;
            res /= divisor;
        }
    }
    return make_int(res);
}

static lisp_val_t *prim_quotient(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long b = arg_int(args, 1, 0);
    if (b == 0) return &nil_val;
    return make_int(arg_int(args, 0, 0) / b);
}

static lisp_val_t *prim_remainder(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long b = arg_int(args, 1, 0);
    if (b == 0) return &nil_val;
    return make_int(arg_int(args, 0, 0) % b);
}

/* modulo differs from remainder when signs differ: the result takes the
 * divisor's sign (R7RS), not the dividend's. */
static lisp_val_t *prim_modulo(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long a = arg_int(args, 0, 0);
    long b = arg_int(args, 1, 0);
    if (b == 0) return &nil_val;
    long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return make_int(r);
}

static lisp_val_t *prim_abs(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long a = arg_int(args, 0, 0);
    return make_int(a < 0 ? -a : a);
}

static lisp_val_t *prim_min(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    long best = arg_int(args, 0, 0);
    int guard = 0;
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (c->u.pair.car->type == LISP_INT && c->u.pair.car->u.i < best) best = c->u.pair.car->u.i;
    }
    return make_int(best);
}

static lisp_val_t *prim_max(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    long best = arg_int(args, 0, 0);
    int guard = 0;
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (c->u.pair.car->type == LISP_INT && c->u.pair.car->u.i > best) best = c->u.pair.car->u.i;
    }
    return make_int(best);
}

/* --- Predicates --- */

static lisp_val_t *prim_null_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_NIL) ? &true_val : &false_val;
}

static lisp_val_t *prim_pair_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_PAIR) ? &true_val : &false_val;
}

static lisp_val_t *prim_symbol_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_SYMBOL) ? &true_val : &false_val;
}

static lisp_val_t *prim_string_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_STRING) ? &true_val : &false_val;
}

static lisp_val_t *prim_integer_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_INT) ? &true_val : &false_val;
}

static lisp_val_t *prim_procedure_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && (a->type == LISP_PRIMITIVE || a->type == LISP_LAMBDA)) ? &true_val : &false_val;
}

static lisp_val_t *prim_zero_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_INT && a->u.i == 0) ? &true_val : &false_val;
}

/* #t/#f are LISP_SYMBOL, same as every other symbol -- distinguished only
 * by text, not by a dedicated type (see prim_eq's comment) -- so this
 * checks symbol text, matching lisp_truthy()'s own convention, rather than
 * pointer identity against &true_val/&false_val alone (which would miss a
 * freshly read/quoted '#t or '#f that isn't literally one of those two
 * singleton nodes). */
static lisp_val_t *prim_boolean_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (a && a->type == LISP_SYMBOL && (streq(a->u.sym, "#t") || streq(a->u.sym, "#f"))) {
        return &true_val;
    }
    return &false_val;
}

/* --- List processing --- */

static lisp_val_t *prim_cons(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    return make_pair(a ? a : &nil_val, b ? b : &nil_val);
}

static lisp_val_t *prim_car(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_PAIR) ? a->u.pair.car : &nil_val;
}

static lisp_val_t *prim_cdr(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_PAIR) ? a->u.pair.cdr : &nil_val;
}

/* (list a b c ...) -- `args` is already the proper, already-evaluated
 * list of arguments the caller built; that IS the answer, with no further
 * allocation needed. */
static lisp_val_t *prim_list(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return args ? args : &nil_val;
}

static lisp_val_t *prim_length(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type == LISP_NIL) return make_int(0);
    if (a->type != LISP_PAIR) return &nil_val;
    int n = 0;
    int guard = 0;
    for (lisp_val_t *c = a; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) n++;
    return make_int(n);
}

/* (append list1 list2 ... listN) -- every list but the last is shallow-
 * copied (its own cons cells must not be shared, since e.g. `(append a a)`
 * would otherwise make cdr'ing past the first copy of `a` immediately
 * re-enter it, an infinite list); the last list is linked in directly,
 * matching standard Scheme append semantics. */
static lisp_val_t *prim_append(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int n = lisp_list_len(args);
    if (n == 0) return &nil_val;
    lisp_val_t *head = &nil_val;
    lisp_val_t *tail = NULL;
    for (int i = 0; i < n - 1; i++) {
        lisp_val_t *lst = lisp_list_ref(args, i);
        int guard = 0;
        for (lisp_val_t *c = lst; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
            lisp_val_t *new_p = make_pair(c->u.pair.car, &nil_val);
            if (!tail) { head = new_p; tail = new_p; } else { tail->u.pair.cdr = new_p; tail = new_p; }
        }
    }
    lisp_val_t *last = lisp_list_ref(args, n - 1);
    if (!tail) return last ? last : &nil_val;
    tail->u.pair.cdr = last ? last : &nil_val;
    return head;
}

static lisp_val_t *prim_reverse(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *lst = lisp_list_ref(args, 0);
    lisp_val_t *result = &nil_val;
    int guard = 0;
    for (lisp_val_t *c = lst; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
        result = make_pair(c->u.pair.car, result);
    }
    return result;
}

static lisp_val_t *prim_list_ref(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *lst = lisp_list_ref(args, 0);
    long n = arg_int(args, 1, -1);
    if (n < 0) return &nil_val;
    lisp_val_t *r = lisp_list_ref(lst, (int)n);
    return r ? r : &nil_val;
}

static lisp_val_t *prim_map(lisp_val_t *args, lisp_val_t *env) {
    lisp_val_t *fn = lisp_list_ref(args, 0);
    lisp_val_t *lst = lisp_list_ref(args, 1);
    if (!fn) return &nil_val;
    lisp_val_t *head = &nil_val;
    lisp_val_t *tail = NULL;
    int guard = 0;
    for (lisp_val_t *c = lst; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
        lisp_val_t *call_args = make_pair(c->u.pair.car, &nil_val);
        lisp_val_t *result = lisp_apply(fn, call_args, env);
        lisp_val_t *new_p = make_pair(result, &nil_val);
        if (!tail) { head = new_p; tail = new_p; } else { tail->u.pair.cdr = new_p; tail = new_p; }
    }
    return head;
}

static lisp_val_t *prim_filter(lisp_val_t *args, lisp_val_t *env) {
    lisp_val_t *fn = lisp_list_ref(args, 0);
    lisp_val_t *lst = lisp_list_ref(args, 1);
    if (!fn) return &nil_val;
    lisp_val_t *head = &nil_val;
    lisp_val_t *tail = NULL;
    int guard = 0;
    for (lisp_val_t *c = lst; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
        lisp_val_t *call_args = make_pair(c->u.pair.car, &nil_val);
        if (lisp_truthy(lisp_apply(fn, call_args, env))) {
            lisp_val_t *new_p = make_pair(c->u.pair.car, &nil_val);
            if (!tail) { head = new_p; tail = new_p; } else { tail->u.pair.cdr = new_p; tail = new_p; }
        }
    }
    return head;
}

static lisp_val_t *prim_for_each(lisp_val_t *args, lisp_val_t *env) {
    lisp_val_t *fn = lisp_list_ref(args, 0);
    lisp_val_t *lst = lisp_list_ref(args, 1);
    if (!fn) return &nil_val;
    int guard = 0;
    for (lisp_val_t *c = lst; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
        lisp_val_t *call_args = make_pair(c->u.pair.car, &nil_val);
        lisp_apply(fn, call_args, env);
    }
    return &nil_val;
}

/* --- String processing --- */

static lisp_val_t *prim_string_append(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    char buf[STRING_SLOT_LEN];
    size_t used = 0;
    buf[0] = '\0';
    int guard = 0;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE && used < sizeof(buf) - 1;
         c = c->u.pair.cdr, guard++) {
        const char *s = get_str_val(c->u.pair.car);
        size_t len = strlen(s);
        if (used + len > sizeof(buf) - 1) len = sizeof(buf) - 1 - used;
        memcpy(buf + used, s, len);
        used += len;
        buf[used] = '\0';
    }
    return make_str(buf);
}

static lisp_val_t *prim_string_length(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type != LISP_STRING) return make_int(0);
    return make_int((long)strlen(a->u.str));
}

/* (substring str start end) -- end defaults to the string's own length. */
static lisp_val_t *prim_substring(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type != LISP_STRING) return make_str("");
    const char *s = a->u.str;
    long len = (long)strlen(s);
    long start = arg_int(args, 1, 0);
    long end = arg_int(args, 2, len);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start > end) start = end;
    long n = end - start;
    char buf[STRING_SLOT_LEN];
    if (n >= (long)sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s + start, (size_t)n);
    buf[n] = '\0';
    return make_str(buf);
}

static lisp_val_t *prim_string_to_number(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *s = get_str_val(lisp_list_ref(args, 0));
    if (!s || !*s) return &false_val;
    const char *p = s;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') { p++; }
    if (!*p) return &false_val;
    long val = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return &false_val;
        val = val * 10 + (*p - '0');
    }
    return make_int(sign * val);
}

/* Converts via unsigned magnitude rather than negating the value directly,
 * so LONG_MIN (whose negation is undefined behavior -- this build runs
 * with UBSan trapping on it) converts correctly instead of crashing on the
 * one input where naive negation breaks. */
static lisp_val_t *prim_number_to_string(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type != LISP_INT) return make_str("");
    long v = a->u.i;
    bool neg = v < 0;
    unsigned long uv = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    char digits[24];
    int i = 0;
    if (uv == 0) digits[i++] = '0';
    while (uv > 0) { digits[i++] = (char)('0' + (uv % 10)); uv /= 10; }
    char buf[26];
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = digits[--i];
    buf[j] = '\0';
    return make_str(buf);
}

static lisp_val_t *prim_string_eq(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    if (!a || !b) return &false_val;
    return (strcmp(get_str_val(a), get_str_val(b)) == 0) ? &true_val : &false_val;
}

/* --- Procedure invocation --- */

/* (apply fn a1 a2 ... args-list) -- R7RS shape: every argument except the
 * first (the function) and the last (a list) is passed through as-is; the
 * last argument's own elements are appended to the call. (apply fn list)
 * is the common 2-arg case, handled the same way with zero passed-through
 * args in between. */
static lisp_val_t *prim_apply(lisp_val_t *args, lisp_val_t *env) {
    lisp_val_t *fn = lisp_list_ref(args, 0);
    if (!fn) return &nil_val;
    int n = lisp_list_len(args);
    if (n < 2) return lisp_apply(fn, &nil_val, env);
    lisp_val_t *head = &nil_val;
    lisp_val_t *tail = NULL;
    for (int i = 1; i < n - 1; i++) {
        lisp_val_t *new_p = make_pair(lisp_list_ref(args, i), &nil_val);
        if (!tail) { head = new_p; tail = new_p; } else { tail->u.pair.cdr = new_p; tail = new_p; }
    }
    int guard = 0;
    for (lisp_val_t *c = lisp_list_ref(args, n - 1); c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        lisp_val_t *new_p = make_pair(c->u.pair.car, &nil_val);
        if (!tail) { head = new_p; tail = new_p; } else { tail->u.pair.cdr = new_p; tail = new_p; }
    }
    return lisp_apply(fn, head, env);
}

/* (eval expr) -- always against global_env: this engine has no first-class
 * environment object to evaluate against another one, matching the scope
 * of everything else here. */
static lisp_val_t *prim_eval(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *expr = lisp_list_ref(args, 0);
    if (!expr) return &nil_val;
    return lisp_eval(expr, global_env);
}

static lisp_val_t *prim_peek(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    if (!a1 || a1->type != LISP_INT) return make_int(0);
    uintptr_t addr = (uintptr_t)a1->u.i;
    uint32_t val = *(volatile uint32_t *)addr;
    return make_int((long)val);
}

static lisp_val_t *prim_poke(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    lisp_val_t *a2 = lisp_list_ref(args, 1);
    if (!a1 || a1->type != LISP_INT || !a2 || a2->type != LISP_INT) return &false_val;
    uintptr_t addr = (uintptr_t)a1->u.i;
    uint32_t val = (uint32_t)a2->u.i;
    *(volatile uint32_t *)addr = val;
    return &true_val;
}

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
/* Canvas primitives over the ST7735 TFT (H1, plan/phase9_chess_computer.md).
 * Deliberately generic -- raw RGB565 ints in, no chess-specific vocabulary --
 * so any future display consumer (not just H4's chess engine) can use them. */
static lisp_val_t *prim_canvas_fill(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    st7735_fill_screen((uint16_t)arg_int(args, 0, ST7735_BLACK));
    return &true_val;
}

static lisp_val_t *prim_canvas_pixel(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int x = (int)arg_int(args, 0, 0);
    int y = (int)arg_int(args, 1, 0);
    uint16_t color = (uint16_t)arg_int(args, 2, ST7735_WHITE);
    st7735_draw_pixel(x, y, color);
    return &true_val;
}

static lisp_val_t *prim_canvas_rect(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int x = (int)arg_int(args, 0, 0);
    int y = (int)arg_int(args, 1, 0);
    int w = (int)arg_int(args, 2, 0);
    int h = (int)arg_int(args, 3, 0);
    uint16_t color = (uint16_t)arg_int(args, 4, ST7735_WHITE);
    st7735_draw_rect(x, y, w, h, color);
    return &true_val;
}

static lisp_val_t *prim_canvas_text(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int x = (int)arg_int(args, 0, 0);
    int y = (int)arg_int(args, 1, 0);
    const char *str = get_str_val(lisp_list_ref(args, 2));
    uint16_t color = (uint16_t)arg_int(args, 3, ST7735_WHITE);
    int size = (int)arg_int(args, 4, 1);
    st7735_draw_string(x, y, str, color, size);
    return &true_val;
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
/* TM1638 primitives (H2, plan/phase9_chess_computer.md). */
static lisp_val_t *prim_tm_display(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    tm1638_display_string(get_str_val(lisp_list_ref(args, 0)));
    return &true_val;
}

static lisp_val_t *prim_tm_set_leds(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    tm1638_set_leds((uint8_t)arg_int(args, 0, 0));
    return &true_val;
}

static lisp_val_t *prim_tm_get_key(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return make_int(tm1638_get_key());
}
#endif

#if CONFIG_ENABLE_CHESS
/* Chess primitives (H4, plan/phase9_chess_computer.md). chess-selftest has
 * no hardware dependency (builds/runs on every target); chess-run needs
 * the display and keypad both present, matching chess_ui.h's own guard. */
static lisp_val_t *prim_chess_selftest(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    chess_selftest();
    return &true_val;
}

/* `(perft [n])` (J4, plan/phase10_chess_completion.md) -- move-generation
 * correctness suite, `n` defaulting to 0 (chess_perft()'s own "<=0 means
 * the documented default depth" convention, matching upstream's own
 * `run_perft_tests()` no-argument wrapper) when omitted. No hardware
 * dependency, same as chess-selftest above. */
static lisp_val_t *prim_perft(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int max_depth = (int)arg_int(args, 0, 0);
    chess_perft(max_depth);
    return &true_val;
}

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638
static lisp_val_t *prim_chess_run(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    chess_run(); /* returns on Ctrl-C or the TM1638 STOP key (J2) */
    return &true_val;
}
#endif

/* Found live to matter, not just theoretical: `chess` below dispatches
 * to chess_run() unconditionally at *compile* time on a board with both
 * TM1638 and DISPLAY -- there was no way at all to reach the console
 * REPL there, even though chess_console_run() itself has no hardware
 * dependency and builds on every target (chess_ui.h's own comment). This
 * gives it a always-available name of its own, the same shape as
 * `chess-run`/`chess-selftest` below `chess` itself. */
static lisp_val_t *prim_chess_console(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    chess_console_run(); /* returns on 'quit' */
    return &true_val;
}

/* `chess` (J0/J1, plan/phase10_chess_completion.md): the by-default,
 * always-discoverable entry point the user's own proposal names --
 * dispatches to chess_run() where the hardware for it exists (same as
 * `chess-run` above), otherwise to J1's text console REPL
 * (chess_console_run(), chess_ui.c) rather than chess-selftest's
 * fixed-position benchmark, which is not "playing chess" and would be a
 * misleading stand-in. chess-run/chess-console/chess-selftest keep
 * working as the lower-level, hardware-independent-of-choice names
 * either way. */
static lisp_val_t *prim_chess(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638
    chess_run(); /* returns on Ctrl-C or the TM1638 STOP key (J2) */
#else
    chess_console_run(); /* returns on 'quit' */
#endif
    return &true_val;
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
/* `clock` (L4, plan/phase11_pico_clock_green.md): the Pico-Clock-Green
 * appliance loop. Unlike chess -- a general-purpose program also useful
 * interactively on non-dedicated boards, so it stays opt-in via
 * usr_init.lisp -- this hardware has no other purpose, so it's the one
 * persona auto-started via (boot-program) below rather than requiring an
 * SD-card override (user's own distinction, 2026-08-13). */
static lisp_val_t *prim_clock(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    pico_clock_green_run(); /* returns on Ctrl-C */
    return &true_val;
}

/* `(clock-light)` -- diagnostic: raw 12-bit LDR reading (0-4095), the same
 * single-shot conversion the display loop's own auto-brightness samples.
 * Not needed for normal use; exists to check LDR_DARK_THRESHOLD's polarity
 * and value against real ambient light on real hardware. */
static lisp_val_t *prim_clock_light(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return make_int((long)pico_clock_green_read_light());
}
#endif

const char *get_str_val(lisp_val_t *val) {
    if (!val) return "";
    if (val->type == LISP_STRING) return val->u.str;
    if (val->type == LISP_SYMBOL) return val->u.sym;
    return "";
}

static lisp_val_t *prim_ls(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *path = "/";
    if (args && args->type == LISP_PAIR) {
        path = get_str_val(args->u.pair.car);
    }
    vfs_ls(path);
    return &nil_val;
}

static lisp_val_t *prim_cat(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &nil_val;
    const char *path = get_str_val(args->u.pair.car);

    int fd = vfs_open(path, VFS_O_READ);
    if (fd >= 0) {
        vfs_stat_t st;
        /* fat32/proc/remote9p report a real size; device nodes (MOUNT_DEV)
         * always report 0 (fs/vfs_server.c's vfs_fstat()) -- and some, like
         * /dev/uart, never signal EOF, so a second read just blocks waiting
         * for the next byte typed. Stream in bounded chunks only when the
         * size is known; otherwise take the one read a device gives. */
        bool known_size = (vfs_fstat(fd, &st) == 0) && st.size > 0;
        char buf[4096];
        if (known_size) {
            /* Was a single vfs_read() into a fixed 4096-byte buffer, so a
             * file bigger than that (e.g. a 4378-byte init.lisp) was
             * silently truncated. Chunked reads bound only by the file's own
             * size fix that without growing the buffer. */
            uint64_t offset = 0;
            for (;;) {
                int n = vfs_pread(fd, buf, sizeof(buf) - 1, offset);
                if (n <= 0) break;
                buf[n] = '\0';
                cprintf("%s", buf);
                offset += (uint64_t)n;
            }
        } else {
            int n = vfs_pread(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                cprintf("%s", buf);
            }
        }
        vfs_close(fd);
        cprintf("\n");
        return &nil_val;
    }

    /* /srv/ endpoints are message channels, not handle-addressable
     * (fs/include/fs/vfs.h) -- fall back to the legacy whole-message read,
     * which is bounded by the IPC message size already. */
    static char buf[4096];
    int len = vfs_read(path, buf, sizeof(buf) - 1);
    if (len >= 0) {
        buf[len] = '\0';
        cprintf("%s\n", buf);
    } else {
        cprintf("cat: cannot read path '%s'\n", path);
    }
    return &nil_val;
}

static lisp_val_t *prim_touch(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    return (vfs_write(path, "", 0) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_write(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    lisp_val_t *a2 = lisp_list_ref(args, 1);
    if (!a1 || !a2) return &false_val;
    const char *path = get_str_val(a1);
    const char *text = get_str_val(a2);
    return (vfs_write(path, text, strlen(text)) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_mkdir(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    return (vfs_mkdir(path) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_rmdir(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    return (vfs_rmdir(path) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_cp(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    lisp_val_t *a2 = lisp_list_ref(args, 1);
    if (!a1 || !a2) return &false_val;
    const char *src = get_str_val(a1);
    const char *dst = get_str_val(a2);
    return (vfs_cp(src, dst) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_rm(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    return (vfs_remove(path) == 0) ? &true_val : &false_val;
}

#if CONFIG_ENABLE_CC
static lisp_val_t *prim_cc(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    lisp_val_t *a2 = lisp_list_ref(args, 1);
    if (!a1 || !a2) return &false_val;
    char safe_src[128];
    char safe_dst[128];
    strncpy_local(safe_src, get_str_val(a1), sizeof(safe_src));
    strncpy_local(safe_dst, get_str_val(a2), sizeof(safe_dst));
    return (chibicc_compile(safe_src, safe_dst) == 0) ? &true_val : &false_val;
}
#endif

/* (path-set "ram0 sd0 flash0") -- reorder or replace the command search path.
 * Belongs in init.lisp: which volumes exist, and which should win when two
 * carry the same utility, is a property of a board rather than of the
 * kernel. Reading it back is `cat /proc/path`. */
/* (spawn "/path/to.elf") -- start a program without waiting for it, returning
 * its pid. (exec ...) blocks until its program is dead, so a caller using it
 * can only ever have one resident however many slots the loader has; this is
 * what makes concurrency observable. */
static lisp_val_t *prim_spawn(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    char safe_path[128];
    const char *p = get_str_val(args->u.pair.car);
    strncpy_local(safe_path, p, 127); safe_path[127] = '\0';
    return make_int(elf_spawn(safe_path));
}

static lisp_val_t *prim_path_set(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *spec = get_str_val(args->u.pair.car);
    return (path_set(spec) > 0) ? &true_val : &false_val;
}

/* (which "uhello") -- where a bare name would actually resolve to, without
 * running it. The path is a policy that can be changed at runtime, so being
 * able to ask which file won is worth a primitive. */
static lisp_val_t *prim_which(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    static char found[128];
    if (path_resolve("bin", name, ".elf", found, sizeof(found)) != 0) return &false_val;
    return make_str(found);
}

static lisp_val_t *prim_exec(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    char safe_path[128];
    const char *p = get_str_val(args->u.pair.car);
    strncpy_local(safe_path, p, 127); safe_path[127] = '\0';
    int res = elf_load_and_run(safe_path);
    return make_int(res);
}

/* /proc/<name> files are real byte streams now (A1, vfs_open()/vfs_pread()),
 * not a printk() side effect -- read and print the actual content instead
 * of relying on vfs_read()/vfs_ls() to have printed it themselves. */
static void print_proc_file(const char *path) {
    static char buf[512];
    int len = vfs_read(path, buf, sizeof(buf));
    if (len >= 0) {
        cprintf("%s", buf);
    } else {
        cprintf("(no data for '%s')\n", path);
    }
}

static lisp_val_t *prim_ps(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    print_proc_file("/proc/ps");
    return &nil_val;
}

static lisp_val_t *prim_meminfo(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    print_proc_file("/proc/meminfo");
    return &nil_val;
}

static lisp_val_t *prim_version(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    print_proc_file("/proc/version");
    return &nil_val;
}

static lisp_val_t *prim_df(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    print_proc_file("/proc/df");
    return &nil_val;
}

static lisp_val_t *prim_top(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    cprintf("\n==================================================\n");
    cprintf("           LugalOS System Monitor                 \n");
    cprintf("==================================================\n");
    print_proc_file("/proc/version");
    cprintf("\n[Process States]\n");
    print_proc_file("/proc/ps");
    cprintf("\n[Memory Status]\n");
    print_proc_file("/proc/meminfo");
    cprintf("\n[Storage Usage]\n");
    print_proc_file("/proc/df");
    cprintf("==================================================\n");
    return &nil_val;
}

static lisp_val_t *prim_load(lisp_val_t *args, lisp_val_t *env) {

    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    static char buf[8192];
    int len = vfs_read(path, buf, sizeof(buf) - 1);
    if (len < 0) {
        cprintf("load: cannot open file '%s'\n", path);
        return &false_val;
    }
    buf[len] = '\0';
    lisp_eval_string(buf);
    return &true_val;
}

static lisp_val_t *prim_display(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (args && args->type == LISP_PAIR) {
        lisp_val_t *v = args->u.pair.car;
        if (v->type == LISP_STRING) {
            cprintf("%s", v->u.str);
        } else {
            lisp_print(v);
        }
    }
    return &nil_val;
}

static lisp_val_t *prim_newline(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    cprintf("\n");
    return &nil_val;
}

static lisp_val_t *prim_read_file(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_str("");
    const char *path = get_str_val(args->u.pair.car);
    static char buf[4096];
    int len = vfs_read(path, buf, sizeof(buf) - 1);
    if (len < 0) return make_str("");
    buf[len] = '\0';
    return make_str(buf);
}

static lisp_val_t *prim_write_file(lisp_val_t *args, lisp_val_t *env) {

    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    lisp_val_t *a2 = lisp_list_ref(args, 1);
    if (!a1 || !a2) return &false_val;
    const char *path = get_str_val(a1);
    const char *text = get_str_val(a2);
    int res = vfs_write(path, text, strlen(text));
    return (res == 0) ? &true_val : &false_val;
}

/* (board) -- which machine this is, as opposed to (arch), which is the
 * instruction set. They are not the same question and init.lisp needs both:
 * RP2350 and QEMU's virt board are both "rv32", but one has 512 KB of SRAM
 * and the other 128 MB, so a RAM disk size that is sensible on one is most of
 * the machine on the other. Before this there was no way for a boot script to
 * tell them apart (C5). */
/* (bind "name") -- give a port to a protocol (C8).
 *
 * The device name says which protocol: `uart` is that wire as a console,
 * `uartslip` is the same wire as dedicated 9P, `usbcon` is ACM1 as a console
 * where `usbnet` is ACM1 as a 9P link. One wire, several names, one holder --
 * so this refuses rather than creating a second owner of the same registers.
 *
 * Refusing rather than taking over is deliberate. A takeover that silently
 * moved the console off the port an operator is typing on would lock them out
 * of the machine; being told what holds the wire lets them release it on
 * purpose. */
static lisp_val_t *prim_bind(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    if (!name) return &false_val;

    /* A console device is bound as the terminal; a 9P link is served. Which
     * one is not a parameter -- it is what the device *is*. */
    if (dev_get(name, DEV_KIND_CONSOLE)) {
        return (console_bind_device(name) == 0) ? &true_val : &false_val;
    }
    if (dev_get(name, DEV_KIND_P9LINK)) {
        /* Claims the wire; does NOT start serving on it.
         *
         * Those are two different things and conflating them was actively
         * destructive: registering a background 9P server on `uartslip` means
         * the 9P poller starts consuming that UART's input, and on the QEMU
         * targets that UART *is* the console -- so a bind intended as policy
         * silently ate the session it was typed into.
         *
         * Binding declares who owns the wire. Starting traffic is what
         * `p9serve`, `p9share` and the boot-time DEV_F_BACKGROUND_9P
         * registration do, and they can now only do it on a wire this says
         * they hold. */
        return (dev_claim(name) == 0) ? &true_val : &false_val;
    }
    cprintf("bind: no such port '%s'\n", name);
    return &false_val;
}

/* (release "name") -- give the wire back, so something else can take it. */
static lisp_val_t *prim_release(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    if (!name) return &false_val;
    /* Releases the claim only; anything already serving on the wire is
     * stopped by whatever started it. Symmetric with (bind). */
    dev_release(name);
    return &true_val;
}

/* (ports) -- what this board can bind, and what currently holds each wire. */
static lisp_val_t *prim_ports(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    print_proc_file("/proc/ports");
    return &nil_val;
}

static lisp_val_t *prim_board(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
#if defined(CONFIG_BOARD_RP2350)
    return make_str("rp2350");
#elif defined(CONFIG_BOARD_K210)
    return make_str("k210");
#else
    return make_str("qemu-virt");
#endif
}

static lisp_val_t *prim_arch(lisp_val_t *args, lisp_val_t *env) {

    (void)args; (void)env;
#if defined(CONFIG_TARGET_RV32)
    return make_str("rv32");
#else
    return make_str("rv64");
#endif
}

/* `(boot-program)` (L4, plan/phase11_pico_clock_green.md): the name of the
 * program init.lisp should auto-start for this build persona, or "" for
 * none (plain interactive shell). A build-time fact, the same shape as
 * (board)/(arch) above -- not a runtime setting, and deliberately not tied
 * to every CONFIG_ENABLE_* flag: chess is a general-purpose program also
 * useful interactively on a non-dedicated board, so it stays opt-in via
 * /sd0/system/etc/usr_init.lisp rather than auto-starting here (user's own
 * distinction, 2026-08-13) -- only single-purpose appliance hardware like
 * the clock earns an entry. */
static lisp_val_t *prim_boot_program(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    return make_str("clock");
#else
    return make_str("");
#endif
}

/* (mounted? "/sd0") -- is that volume mounted and writable?
 *
 * Not the same question as (dev-present? "spisd"): a card reader can be
 * probed and working with no card in it, and a read-only volume is mounted
 * but is not somewhere to put scratch files. init.lisp needs this one to
 * decide whether a RAM disk earns its memory (C5). */
static lisp_val_t *prim_mounted(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    return vfs_volume_writable(name) ? &true_val : &false_val;
}

static lisp_val_t *prim_mount_ramdisk(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int size_kb = (int)arg_int(args, 0, 512);
    int res = vfs_mount_ramdisk(size_kb);
    return (res == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_format(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a1 = lisp_list_ref(args, 0);
    if (!a1) return &false_val;
    return (vfs_format(get_str_val(a1)) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_time(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return make_int((long)time_get_ms());
}

static lisp_val_t *prim_date(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    rtc_time_t tm;
    time_get_rtc(&tm);
    char buf[32];
    time_format_iso(&tm, buf, sizeof(buf));
    return make_str(buf);
}

static lisp_val_t *prim_set_date(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    rtc_time_t tm;
    if (args->u.pair.car->type == LISP_STRING || args->u.pair.car->type == LISP_SYMBOL) {
        const char *str = (args->u.pair.car->type == LISP_STRING) ? args->u.pair.car->u.str : args->u.pair.car->u.sym;
        if (time_parse_iso(str, &tm)) {
            time_set_rtc(&tm);
            i2c_rtc_write_time(&tm);
            return &true_val;
        }
    } else if (args->u.pair.car->type == LISP_INT) {
        lisp_val_t *curr = args;
        int vals[6] = {0};
        for (int i = 0; i < 6 && curr && curr->type == LISP_PAIR; i++) {
            if (curr->u.pair.car->type == LISP_INT) {
                vals[i] = (int)curr->u.pair.car->u.i;
            }
            curr = curr->u.pair.cdr;
        }
        tm.year = (uint16_t)vals[0]; tm.month = (uint8_t)vals[1]; tm.day = (uint8_t)vals[2];
        tm.hour = (uint8_t)vals[3]; tm.min = (uint8_t)vals[4]; tm.sec = (uint8_t)vals[5]; tm.ms = 0;
        time_set_rtc(&tm);
        i2c_rtc_write_time(&tm);
        return &true_val;
    }
    return &false_val;
}

static lisp_val_t *prim_eeprom_read(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint16_t offset = 0;
    size_t len = 64;

    if (args && args->type == LISP_PAIR) {
        lisp_val_t *a1 = args->u.pair.car;
        if (a1 && a1->type == LISP_INT) offset = (uint16_t)a1->u.i;
        lisp_val_t *rest = args->u.pair.cdr;
        if (rest && rest->type == LISP_PAIR) {
            lisp_val_t *a2 = rest->u.pair.car;
            if (a2 && a2->type == LISP_INT) len = (size_t)a2->u.i;
        }
    }

    if (len > 512) len = 512;
    char eeprom_buf[513];
    memset(eeprom_buf, 0, sizeof(eeprom_buf));

    int res = at24c32_read(offset, (uint8_t *)eeprom_buf, len);
    if (res < 0) return &nil_val;
    eeprom_buf[res] = '\0';
    return make_str(eeprom_buf);
}

static lisp_val_t *prim_eeprom_write(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    lisp_val_t *a1 = args->u.pair.car;
    lisp_val_t *rest = args->u.pair.cdr;
    if (!a1 || a1->type != LISP_INT || !rest || rest->type != LISP_PAIR) return &false_val;
    lisp_val_t *a2 = rest->u.pair.car;
    if (!a2 || a2->type != LISP_STRING) return &false_val;

    uint16_t offset = (uint16_t)a1->u.i;
    const char *str = a2->u.str;
    size_t len = strlen(str);

    int res = at24c32_write(offset, (const uint8_t *)str, len);
    return res >= 0 ? make_int(res) : &false_val;
}

static lisp_val_t *prim_p9_loopback(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *payload = "9P_Lisp_Loopback_Test";
    if (args && args->type == LISP_PAIR && args->u.pair.car->type == LISP_STRING) {
        payload = args->u.pair.car->u.str;
    }

    char out_buf[256];
    memset(out_buf, 0, sizeof(out_buf));

    int res = loopback_9p_rpc(payload, out_buf, sizeof(out_buf));
    if (res >= 0) {
        return make_str(out_buf);
    }
    return &false_val;
}

static lisp_val_t *prim_p9_cat(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);

    char out_buf[512];
    memset(out_buf, 0, sizeof(out_buf));

    int res = loopback_9p_cat(path, out_buf, sizeof(out_buf));
    if (res >= 0) {
        return make_str(out_buf);
    }
    return &false_val;
}

/* Resolves a DEV_KIND_P9LINK device from the B0 registry (kernel/device.h)
 * by name. A NULL/empty name selects this board's default background link --
 * virtio-console on QEMU, ACM1/EP4 on RP2350 -- which is what the flag
 * DEV_F_BACKGROUND_9P marks, so callers that don't care get the same link
 * they used to get from a hardcoded virtio_console_get_link() call, while
 * callers that do care can now name one. */
static p9_link_t *lisp_resolve_link(const char *name) {
    if (name && name[0]) {
        return (p9_link_t *)dev_get(name, DEV_KIND_P9LINK);
    }
    uint32_t cursor = 0;
    return (p9_link_t *)dev_next_with_flags(&cursor, DEV_KIND_P9LINK, DEV_F_BACKGROUND_9P);
}

/* Node-to-node 9P (A4): fetches a file from whatever real peer is bridged
 * onto one of this node's 9P links -- unlike p9-cat/p9-loopback above, this
 * isn't talking to this node's own local 9P server, it's talking to a
 * genuinely different machine (see the multi-node test in tests/runner.py).
 * See fs/p9_link.c's p9_link_cat() for why this is safe alongside the link's
 * own background server role.
 *
 * (p9-remote-cat "path" ["device"]) -- was QEMU-only behind an RP2350 guard
 * while it hardcoded virtio_console_get_link(); resolving through the device
 * registry instead means RP2350 can use it over its `usbnet` link, so the
 * guard is gone. */
static lisp_val_t *prim_p9_remote_cat(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);

    const char *devname = NULL;
    lisp_val_t *rest = args->u.pair.cdr;
    if (rest && rest->type == LISP_PAIR) devname = get_str_val(rest->u.pair.car);

    p9_link_t *link = lisp_resolve_link(devname);
    if (!link) return &false_val;

    char out_buf[512];
    memset(out_buf, 0, sizeof(out_buf));

    int res = p9_link_cat(link, path, out_buf, sizeof(out_buf));
    if (res >= 0) {
        return make_str(out_buf);
    }
    return &false_val;
}

/* Attaches a remote peer's namespace at /<name>/ (A5) -- turns "9P works"
 * into "distributed namespace works": once mounted, standard commands
 * (ls, cat, write, cp, mkdir, rm) work through /<name>/ exactly like any
 * other mount, not just through a special-purpose primitive like
 * p9-remote-cat above.
 *
 * (mount-remote "name" ["device"]) -- the optional second argument names a
 * DEV_KIND_P9LINK device from the B0 registry (see /proc/devices). Omitted,
 * it uses this board's default background link. A5's completion notes listed
 * "mount-remote can currently only target the virtio-console link" as
 * deferred; the device registry is what makes resolving one by name
 * possible, so that limitation is closed here. */
static lisp_val_t *prim_mount_remote(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);

    const char *devname = NULL;
    lisp_val_t *rest = args->u.pair.cdr;
    if (rest && rest->type == LISP_PAIR) devname = get_str_val(rest->u.pair.car);

    p9_link_t *link = lisp_resolve_link(devname);
    if (!link) return &false_val;

    return (vfs_mount_remote(name, link) == 0) ? &true_val : &false_val;
}

/* --- B0 part 3: binding primitives ---
 * These exist so that *policy* -- which link serves 9P, where the kernel log
 * goes, which hardware a boot script assumes -- lives in init.lisp instead of
 * being compiled into kernel_main(). The registries themselves (B0 parts 1
 * and 2) are what make naming these things at runtime possible. */

/* (devices) / (klog-sinks): print the registries. Both read through /proc,
 * so they show exactly what a remote 9P client reading the same file sees --
 * no second, divergent formatting path. */
static lisp_val_t *prim_devices(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    print_proc_file("/proc/devices");
    return &nil_val;
}

/* (dev-present? "name") -- lets init.lisp branch on what this board actually
 * has, rather than the script having to know which target it booted on. */
static lisp_val_t *prim_dev_present(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    if (!name) return &false_val;

    const char *dname;
    bool present;
    for (uint32_t i = 0; dev_info(i, &dname, NULL, &present); i++) {
        if (strcmp(dname, name) == 0) return present ? &true_val : &false_val;
    }
    return &false_val;
}

static lisp_val_t *prim_klog_sinks(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    const char *name;
    bool attached;
    for (uint32_t i = 0; klog_sink_info(i, &name, &attached); i++) {
        cprintf("  %s: %s\n", name, attached ? "attached" : "detached");
    }
    return &nil_val;
}

/* (klog-detach "console") / (klog-attach "console") -- the scenario this
 * whole milestone is named for: a channel carries kernel log output until
 * init.lisp decides something else should own it. The ring keeps recording
 * either way, so nothing is lost (see kernel/klog.h). */
static lisp_val_t *prim_klog_detach(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    return (name && klog_sink_detach(name) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_klog_attach(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    return (name && klog_sink_attach(name) == 0) ? &true_val : &false_val;
}

/* (p9-serve "device") / (p9-unserve "device") -- bind or unbind a named
 * p9link device as a background 9P server at runtime. This is the
 * general-purpose form of what DEV_F_BACKGROUND_9P does automatically at
 * boot, and of what `p9share` does for the UART demux specifically. Note it
 * does NOT arm the UART demux itself -- `uartdemux` still needs `p9share`,
 * because sharing a wire with the console is a driver-level mode change, not
 * just a registration.
 *
 * #f means the device name didn't resolve to a present p9link. #t means the
 * registration was *requested*: p9_link_register_background() returns void
 * and, past its two-slot limit, logs and drops rather than reporting an
 * error (A3b), so there is no success code here to forward honestly. Check
 * the kernel log if a link doesn't come up. */
static lisp_val_t *prim_p9_serve(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    p9_link_t *link = lisp_resolve_link(get_str_val(args->u.pair.car));
    if (!link) return &false_val;
    p9_link_register_background(link);
    return &true_val;
}

static lisp_val_t *prim_p9_unserve(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    p9_link_t *link = lisp_resolve_link(get_str_val(args->u.pair.car));
    if (!link) return &false_val;
    p9_link_unregister_background(link);
    return &true_val;
}

/* (mount-local "name") -- attaches this node's OWN namespace at /<name>/
 * through the local 9P channel (B1). Useful in itself, but mainly a
 * demonstration: /<name>/sd0/x reaches the same bytes as /sd0/x, having
 * traversed serialized, copied 9P frames through the same client code that
 * talks to a peer over a USB cable. If that works, an address-space boundary
 * (B3) changes nothing above the channel. */
/* (spawn-pump n) -- B2/D5. Creates a task that services background 9P links
 * and yields, n times, then exits.
 *
 * This exists to make the D5 hazard actually reachable in a test. A4's
 * correctness argument was "nothing can run while a synchronous client
 * exchange is in flight, because there is no scheduler". B2 makes that
 * false, but only if something else is genuinely runnable -- with just the
 * boot task alive, sched_yield() has nobody to switch to and the dangerous
 * interleaving never occurs. With this task running, a client waiting for
 * its reply yields, this task runs p9_link_background_poll() on the *same*
 * link, and reads the reply off the wire. Routing it correctly (by 9P type
 * parity and tag, see fs/p9_link.c) is what keeps the client from hanging. */
static void pump_task_body(void *arg) {
    long n = (long)(uintptr_t)arg;
    for (long i = 0; i < n; i++) {
        p9_link_background_poll();
        sched_yield();
    }
}

/* (console-bind "uart"|"usb") -- hand the terminal to a named device from
 * the registry (B4). This is §5.2's scenario made runtime: a channel carries
 * output until init.lisp decides something else should own it. Kernel
 * diagnostics are a separate stream (see klog-detach), so moving the console
 * does not move the log, and vice versa. */
static lisp_val_t *prim_console_bind(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    if (!name) return &false_val;
    return (console_bind_device(name) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_console_device(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return make_str(console_bound_device());
}

static lisp_val_t *prim_spawn_pump(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long n = 512;
    if (args && args->type == LISP_PAIR && args->u.pair.car->type == LISP_INT) {
        n = args->u.pair.car->u.i;
    }
    int pid = task_create("pump", pump_task_body, (void *)(uintptr_t)n);
    return (pid >= 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_mount_local(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    if (!name) return &false_val;
    return (vfs_mount_local(name) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_unmount(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    return (vfs_unmount(name) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_p9_uart_send(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *payload = "SLIP_9P_UART_Test";
    if (args && args->type == LISP_PAIR && args->u.pair.car->type == LISP_STRING) {
        payload = args->u.pair.car->u.str;
    }

    char out_buf[256];
    memset(out_buf, 0, sizeof(out_buf));

    int res = uart_net_rpc(payload, out_buf, sizeof(out_buf));
    if (res >= 0) {
        return make_str(out_buf);
    }
    return &false_val;
}

static lisp_val_t *prim_i2c_scan(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    i2c_scan_bus();
    return &nil_val;
}

static lisp_val_t *prim_usb_status(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    usb_cdc_debug_dump();
    return &nil_val;
}

static lisp_val_t *prim_lsh(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    shell_run();
    return &nil_val;
}

/* Discoverability (D2/D3 in plan/completed/2026-08-07_review_and_remediation.md): the
 * Lisp engine is the shell's execution core, but had no way to list what's
 * actually callable short of reading the source. This walks global_env
 * directly rather than maintaining a separate hand-written list, so it can
 * never drift out of sync with what's actually bound the way cmd_help() in
 * kernel/shell.c had. */
static lisp_val_t *prim_help(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    cprintf("\nLugalOS Lisp Machine -- Bound Globals:\n");
    cprintf("-------------------------------------------------\n");
    int count = 0;
    for (lisp_val_t *c = global_env; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        lisp_val_t *binding = c->u.pair.car;
        if (!binding || binding->type != LISP_PAIR) continue;
        lisp_val_t *k = binding->u.pair.car;
        if (!k || k->type != LISP_SYMBOL) continue;
        lisp_val_t *v = binding->u.pair.cdr;
        const char *kind = "value";
        if (v) {
            if (v->type == LISP_PRIMITIVE) kind = "primitive";
            else if (v->type == LISP_LAMBDA) kind = "closure";
        }
        cprintf("  %s -- %s\n", k->u.sym, kind);
        count++;
    }
    cprintf("-------------------------------------------------\n");
    cprintf("%d bound symbols. Special forms (not primitives, so not listed\n"
           "above): define, lambda, quote / ', if, begin, let, let*, while, cond.\n\n", count);
    return &nil_val;
}

void lisp_init(void) {
    node_pool_idx = 0;
    node_pool_exhausted_warned = false;
    string_pool_idx = 0;
    string_pool_exhausted_warned = false;
    eval_depth = 0;
    eval_depth_exceeded_warned = false;
    global_env = &nil_val;

    env_set(&global_env, "#t", &true_val);
    env_set(&global_env, "#f", &false_val);

    env_set(&global_env, "+", make_prim(prim_add));
    env_set(&global_env, "-", make_prim(prim_sub));
    env_set(&global_env, "*", make_prim(prim_mul));
    env_set(&global_env, "=", make_prim(prim_eq));

    /* S4 (plan/phase13_lisp_engine_extensions.md): standard library,
     * as C primitives -- see prim_map()/prim_filter()'s own comments for
     * why (touching existing cons cells directly costs no interpreter-
     * level node allocation for the traversal itself, unlike a Lisp-
     * defined equivalent recursing through the evaluator). */
    env_set(&global_env, "<", make_prim(prim_lt));
    env_set(&global_env, ">", make_prim(prim_gt));
    env_set(&global_env, "<=", make_prim(prim_le));
    env_set(&global_env, ">=", make_prim(prim_ge));
    env_set(&global_env, "/=", make_prim(prim_ne));
    env_set(&global_env, "/", make_prim(prim_div));
    env_set(&global_env, "quotient", make_prim(prim_quotient));
    env_set(&global_env, "remainder", make_prim(prim_remainder));
    env_set(&global_env, "modulo", make_prim(prim_modulo));
    env_set(&global_env, "abs", make_prim(prim_abs));
    env_set(&global_env, "min", make_prim(prim_min));
    env_set(&global_env, "max", make_prim(prim_max));
    env_set(&global_env, "null?", make_prim(prim_null_p));
    env_set(&global_env, "pair?", make_prim(prim_pair_p));
    env_set(&global_env, "symbol?", make_prim(prim_symbol_p));
    env_set(&global_env, "string?", make_prim(prim_string_p));
    env_set(&global_env, "integer?", make_prim(prim_integer_p));
    env_set(&global_env, "procedure?", make_prim(prim_procedure_p));
    env_set(&global_env, "zero?", make_prim(prim_zero_p));
    env_set(&global_env, "boolean?", make_prim(prim_boolean_p));
    env_set(&global_env, "cons", make_prim(prim_cons));
    env_set(&global_env, "car", make_prim(prim_car));
    env_set(&global_env, "cdr", make_prim(prim_cdr));
    env_set(&global_env, "list", make_prim(prim_list));
    env_set(&global_env, "length", make_prim(prim_length));
    env_set(&global_env, "append", make_prim(prim_append));
    env_set(&global_env, "reverse", make_prim(prim_reverse));
    env_set(&global_env, "list-ref", make_prim(prim_list_ref));
    env_set(&global_env, "nth", make_prim(prim_list_ref));
    env_set(&global_env, "map", make_prim(prim_map));
    env_set(&global_env, "filter", make_prim(prim_filter));
    env_set(&global_env, "for-each", make_prim(prim_for_each));
    env_set(&global_env, "string-append", make_prim(prim_string_append));
    env_set(&global_env, "string-length", make_prim(prim_string_length));
    env_set(&global_env, "substring", make_prim(prim_substring));
    env_set(&global_env, "string->number", make_prim(prim_string_to_number));
    env_set(&global_env, "number->string", make_prim(prim_number_to_string));
    env_set(&global_env, "string=?", make_prim(prim_string_eq));
    env_set(&global_env, "apply", make_prim(prim_apply));
    env_set(&global_env, "eval", make_prim(prim_eval));

    env_set(&global_env, "peek", make_prim(prim_peek));
    env_set(&global_env, "poke", make_prim(prim_poke));
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    env_set(&global_env, "canvas-fill", make_prim(prim_canvas_fill));
    env_set(&global_env, "canvas-pixel", make_prim(prim_canvas_pixel));
    env_set(&global_env, "canvas-rect", make_prim(prim_canvas_rect));
    env_set(&global_env, "canvas-text", make_prim(prim_canvas_text));
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    env_set(&global_env, "tm-display", make_prim(prim_tm_display));
    env_set(&global_env, "tm-set-leds", make_prim(prim_tm_set_leds));
    env_set(&global_env, "tm-get-key", make_prim(prim_tm_get_key));
#endif
#if CONFIG_ENABLE_CHESS
    env_set(&global_env, "chess-selftest", make_prim(prim_chess_selftest));
    env_set(&global_env, "perft", make_prim(prim_perft));
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638
    env_set(&global_env, "chess-run", make_prim(prim_chess_run));
#endif
    env_set(&global_env, "chess-console", make_prim(prim_chess_console));
    env_set(&global_env, "chess", make_prim(prim_chess));
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    env_set(&global_env, "clock", make_prim(prim_clock));
    env_set(&global_env, "clock-light", make_prim(prim_clock_light));
#endif
    env_set(&global_env, "ls", make_prim(prim_ls));
    env_set(&global_env, "cat", make_prim(prim_cat));
    env_set(&global_env, "touch", make_prim(prim_touch));
    env_set(&global_env, "write", make_prim(prim_write));
    env_set(&global_env, "mkdir", make_prim(prim_mkdir));
    env_set(&global_env, "rmdir", make_prim(prim_rmdir));
    env_set(&global_env, "cp", make_prim(prim_cp));
    env_set(&global_env, "rm", make_prim(prim_rm));
#if CONFIG_ENABLE_CC
    env_set(&global_env, "cc", make_prim(prim_cc));
#endif
    env_set(&global_env, "exec", make_prim(prim_exec));
    env_set(&global_env, "spawn", make_prim(prim_spawn));
    env_set(&global_env, "path-set", make_prim(prim_path_set));
    env_set(&global_env, "which", make_prim(prim_which));
    env_set(&global_env, "ps", make_prim(prim_ps));
    env_set(&global_env, "meminfo", make_prim(prim_meminfo));
    env_set(&global_env, "version", make_prim(prim_version));
    env_set(&global_env, "df", make_prim(prim_df));
    env_set(&global_env, "top", make_prim(prim_top));
    env_set(&global_env, "time", make_prim(prim_time));
    env_set(&global_env, "date", make_prim(prim_date));
    env_set(&global_env, "set-date", make_prim(prim_set_date));
    env_set(&global_env, "set-time", make_prim(prim_set_date));
    env_set(&global_env, "i2c-scan", make_prim(prim_i2c_scan));
    env_set(&global_env, "eeprom-read", make_prim(prim_eeprom_read));
    env_set(&global_env, "eeprom-write", make_prim(prim_eeprom_write));
    env_set(&global_env, "p9-loopback", make_prim(prim_p9_loopback));
    env_set(&global_env, "p9-cat", make_prim(prim_p9_cat));
    /* No longer RP2350-guarded: both resolve their link through the B0
     * device registry rather than hardcoding virtio-console, so RP2350 can
     * use them over its `usbnet` (ACM1/EP4) link. */
    env_set(&global_env, "p9-remote-cat", make_prim(prim_p9_remote_cat));
    env_set(&global_env, "mount-remote", make_prim(prim_mount_remote));
    env_set(&global_env, "console-bind", make_prim(prim_console_bind));
    env_set(&global_env, "console-device", make_prim(prim_console_device));
    env_set(&global_env, "spawn-pump", make_prim(prim_spawn_pump));
    env_set(&global_env, "mount-local", make_prim(prim_mount_local));
    env_set(&global_env, "unmount", make_prim(prim_unmount));

    /* B0 part 3: registry/sink binding, so init.lisp owns the policy. */
    env_set(&global_env, "devices", make_prim(prim_devices));
    env_set(&global_env, "dev-present?", make_prim(prim_dev_present));
    env_set(&global_env, "klog-sinks", make_prim(prim_klog_sinks));
    env_set(&global_env, "klog-detach", make_prim(prim_klog_detach));
    env_set(&global_env, "klog-attach", make_prim(prim_klog_attach));
    env_set(&global_env, "p9-serve", make_prim(prim_p9_serve));
    env_set(&global_env, "p9-unserve", make_prim(prim_p9_unserve));
    env_set(&global_env, "p9-uart-send", make_prim(prim_p9_uart_send));

    env_set(&global_env, "load", make_prim(prim_load));
    env_set(&global_env, "display", make_prim(prim_display));
    env_set(&global_env, "newline", make_prim(prim_newline));
    env_set(&global_env, "read-file", make_prim(prim_read_file));
    env_set(&global_env, "write-file", make_prim(prim_write_file));

    env_set(&global_env, "arch", make_prim(prim_arch));
    env_set(&global_env, "board", make_prim(prim_board));
    env_set(&global_env, "boot-program", make_prim(prim_boot_program));
    env_set(&global_env, "bind", make_prim(prim_bind));
    env_set(&global_env, "release", make_prim(prim_release));
    env_set(&global_env, "ports", make_prim(prim_ports));
    env_set(&global_env, "mount-ramdisk", make_prim(prim_mount_ramdisk));
    env_set(&global_env, "mounted?", make_prim(prim_mounted));
    env_set(&global_env, "format", make_prim(prim_format));
    env_set(&global_env, "lsh", make_prim(prim_lsh));
    env_set(&global_env, "usb-status", make_prim(prim_usb_status));
    env_set(&global_env, "help", make_prim(prim_help));

    printk("[Lisp Engine] Initialized as Core Microkernel Execution Engine.\n");


    /* Automatically load system boot scripts if present.
     *
     * Found through the search path (C1) rather than by trying /sd0 and then
     * /flash0 by hand, which is what this did before. Same idea, generalised:
     * the two hardcoded volumes *were* a search path, just one that only this
     * function knew about and that no board could reorder. Now a board that
     * mounts something else, or wants a different precedence, says so in one
     * place and every lookup follows. */
    static char boot_buf[8192];
    char script[128];
    int len = 0;

    if (path_resolve("etc", "stdlib.lisp", "", script, sizeof(script)) == 0) {
        len = vfs_read(script, boot_buf, sizeof(boot_buf) - 1);
    }
    if (len > 0) {
        boot_buf[len] = '\0';
        lisp_eval_string(boot_buf);
        printk("[Lisp Boot] Loaded %s\n", script);
    }

    len = 0;
    if (path_resolve("etc", "init.lisp", "", script, sizeof(script)) == 0) {
        len = vfs_read(script, boot_buf, sizeof(boot_buf) - 1);
    }
    if (len > 0) {
        boot_buf[len] = '\0';
        lisp_eval_string(boot_buf);
        printk("[Lisp Boot] Executed %s\n", script);
    }
}



/* Printer */
void lisp_print(lisp_val_t *val) {
    if (!val || val->type == LISP_NIL) {
        cprintf("()");
        return;
    }
    switch (val->type) {
        case LISP_INT:
            cprintf("%ld", val->u.i);
            break;
        case LISP_STRING:
            cprintf("\"%s\"", val->u.str);
            break;
        case LISP_SYMBOL:
            cprintf("%s", val->u.sym);
            break;
        case LISP_PRIMITIVE:
            cprintf("<#primitive>");
            break;
        case LISP_LAMBDA:
            cprintf("<#closure>");
            break;
        case LISP_PAIR: {
            /* S4 (plan/phase13_lisp_engine_extensions.md): now that `cons`
             * exists, an improper (dotted) pair -- (cons 1 2), whose cdr is
             * neither a pair nor nil -- is reachable for the first time.
             * Previously every pair the reader or the evaluator built was
             * always nil-terminated, so this loop silently stopping at a
             * non-pair cdr without printing it was never visible: `(cons 1
             * 2)` printed as "(1)", quietly losing the 2. */
            cprintf("(");
            lisp_print(val->u.pair.car);
            lisp_val_t *c = val->u.pair.cdr;
            while (c && c->type == LISP_PAIR) {
                cprintf(" ");
                lisp_print(c->u.pair.car);
                c = c->u.pair.cdr;
            }
            if (c && c->type != LISP_NIL) {
                cprintf(" . ");
                lisp_print(c);
            }
            cprintf(")");
            break;
        }
        default:
            cprintf("?");
            break;
    }
}

/* Lexer / Parser */
static void skip_whitespace(const char **str) {
    while (1) {
        while (**str == ' ' || **str == '\t' || **str == '\r' || **str == '\n') {
            (*str)++;
        }
        if (**str == ';') {
            while (**str != '\n' && **str != '\0') {
                (*str)++;
            }
        } else {
            break;
        }
    }
}


static bool is_delimiter(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' || c == ';';
}

static bool is_number_token(const char *str) {
    if (!str || *str == '\0') return false;
    const char *p = str;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (is_delimiter(*p)) return false;
        while (!is_delimiter(*p)) {
            char c = *p;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
            p++;
        }
        return true;
    }

    if (*p == '+' || *p == '-') {
        p++;
    }
    if (is_delimiter(*p)) return false;

    while (!is_delimiter(*p)) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        p++;
    }
    return true;
}

lisp_val_t *lisp_read(const char **str) {
    skip_whitespace(str);
    if (**str == '\0') return NULL;

    /* Quote Syntax 'expr */
    if (**str == '\'') {
        (*str)++; // skip '\''
        lisp_val_t *quoted_val = lisp_read(str);
        if (!quoted_val) quoted_val = &nil_val;
        return make_pair(make_sym("quote"), make_pair(quoted_val, &nil_val));
    }

    /* Double Quoted Strings "..." */
    if (**str == '"') {

        (*str)++; // skip opening quote
        char buf[128];
        int i = 0;
        while (**str != '"' && **str != '\0' && i < 127) {
            if (**str == '\\' && (*str)[1] != '\0') {
                (*str)++;
                if (**str == 'n') buf[i++] = '\n';
                else if (**str == 't') buf[i++] = '\t';
                else buf[i++] = **str;
            } else {
                buf[i++] = **str;
            }
            (*str)++;
        }
        if (**str == '"') (*str)++; // skip closing quote
        buf[i] = '\0';
        return make_str(buf);
    }

    if (**str == '(') {
        (*str)++; // skip '('
        skip_whitespace(str);
        if (**str == ')') {
            (*str)++;
            return &nil_val;
        }

        lisp_val_t *head = NULL;
        lisp_val_t *tail = NULL;

        while (**str != ')' && **str != '\0') {
            lisp_val_t *elem = lisp_read(str);
            if (!elem) break;
            lisp_val_t *new_pair = make_pair(elem, &nil_val);
            if (!head) {
                head = new_pair;
                tail = head;
            } else {
                tail->u.pair.cdr = new_pair;
                tail = new_pair;
            }
            skip_whitespace(str);
        }
        if (**str == ')') (*str)++;
        return head ? head : &nil_val;
    }

    /* Numbers & Digit-Prefixed Token Validation (Standard Scheme Syntax Rule) */
    if ((**str >= '0' && **str <= '9') || (**str == '0' && ((*str)[1] == 'x' || (*str)[1] == 'X'))) {
        if (!is_number_token(*str)) {
            char bad_tok[32];
            int i = 0;
            while (**str != '\0' && !is_delimiter(**str)) {
                if (i < 31) bad_tok[i++] = **str;
                (*str)++;
            }
            bad_tok[i] = '\0';
            printk("[Lisp Syntax Error] Invalid identifier starting with digit: '%s'\n", bad_tok);
            return &nil_val;
        }

        if (**str == '0' && ((*str)[1] == 'x' || (*str)[1] == 'X')) {
            (*str) += 2;
            long val = 0;
            while ((**str >= '0' && **str <= '9') || (**str >= 'a' && **str <= 'f') || (**str >= 'A' && **str <= 'F')) {
                char c = **str;
                val = val * 16;
                if (c >= '0' && c <= '9') val += (c - '0');
                else if (c >= 'a' && c <= 'f') val += (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') val += (c - 'A' + 10);
                (*str)++;
            }
            return make_int(val);
        } else {
            long val = 0;
            int sign = 1;
            if (**str == '-') {
                sign = -1;
                (*str)++;
            } else if (**str == '+') {
                (*str)++;
            }
            while (**str >= '0' && **str <= '9') {
                val = val * 10 + (**str - '0');
                (*str)++;
            }
            return make_int(sign * val);
        }
    }

    /* S4 (plan/phase13_lisp_engine_extensions.md): a leading sign
     * immediately followed by a digit is a signed number literal (-5,
     * +3) -- found missing while testing `abs`/`modulo`/`number->string`
     * on negative numbers, all of which need to be typeable as literals
     * to be usable at all. The sign-handling code in the digit-first
     * branch just above has existed all along but was unreachable: this
     * is the gate that was missing, not new number-parsing logic. Must
     * check for a following digit specifically -- the bare symbols `-`/`+`
     * (the subtraction/addition primitives themselves) and identifiers
     * like `->foo` still need to fall through to the symbol reader below. */
    if ((**str == '-' || **str == '+') && (*str)[1] >= '0' && (*str)[1] <= '9') {
        int sign = (**str == '-') ? -1 : 1;
        (*str)++;
        long val = 0;
        while (**str >= '0' && **str <= '9') {
            val = val * 10 + (**str - '0');
            (*str)++;
        }
        return make_int(sign * val);
    }

    /* Symbols */
    char buf[32];
    int i = 0;
    while (**str != '\0' && **str != ' ' && **str != '\t' && **str != '\n' && **str != '\r' && **str != '(' && **str != ')') {
        if (i < 31) buf[i++] = **str;
        (*str)++;
    }
    buf[i] = '\0';
    return make_sym(buf);
}

/* S2 (plan/phase13_lisp_engine_extensions.md), fixing a bug found while
 * testing `while`, pre-existing since long before S1/S2 and not specific
 * to either: `define` always writes into the *global_env* variable itself
 * (env_set(&global_env, ...) reassigns it to a new pointer) rather than
 * mutating an existing binding in place. B3 (plan/completed/
 * 2026-08-07_review_and_remediation.md) already fixed the resulting
 * staleness for a lambda closure's *initial* environment (a closure
 * defined at global scope stores env=NULL and re-resolves live global_env
 * at call time instead of a frozen snapshot from definition time) -- but
 * every special form that evaluates a *sequence* of forms/bindings had the
 * same staleness for its own internal sequence: `env`/`local_env` is
 * captured once and reused for every later form, so a `define` earlier in
 * the same body/binding-list is invisible to a later form in it. Concretely,
 * `(begin (define x 1) x)` at the top level reported "Unbound symbol: x",
 * and (this is what surfaced it) a `(while cond body...)` loop whose body
 * mutates state via `define` never saw its own updates and spun until pool
 * exhaustion.
 *
 * `refresh_global_tail` closes this for every affected form (if, begin,
 * let, let*, cond, while, lambda application -- both here and in
 * lisp_apply()) by
 * keeping the form's own local environment extension's fallthrough pointed
 * at the LIVE global_env instead of the frozen snapshot it started with.
 * `original_tail` is what `local_env` was before this particular form added
 * anything to it (itself, if it added nothing at all -- true for if/begin/
 * cond/while, which don't introduce bindings); `was_global` is whether that
 * starting point was global_env's identity at entry, computed once per form
 * invocation before any nested evaluation could have moved global_env on.
 * If not was_global (a genuine lexical/non-global scope), this is a no-op --
 * correct, since `define` never targets a non-global scope's own bindings,
 * so there is nothing to keep in sync there.
 *
 * Safe to call before every nested evaluation: at most a scan proportional
 * to the bindings *this form itself* added (typically a handful), and
 * idempotent -- re-patching an already-current tail is harmless. */
static lisp_val_t *refresh_global_tail(lisp_val_t *local_env, lisp_val_t *original_tail, bool was_global) {
    if (!was_global) return local_env;
    if (local_env == original_tail) return global_env;
    lisp_val_t *c = local_env;
    while (c->type == LISP_PAIR && c->u.pair.cdr != original_tail) {
        c = c->u.pair.cdr;
    }
    if (c->type == LISP_PAIR) c->u.pair.cdr = global_env;
    return local_env;
}

/* Shared by begin/let/cond-clause/lambda-body, all of which have the same
 * "evaluate a sequence, the value is the last form's value" shape. Evaluates
 * every form but the last as a non-tail side effect (still going through the
 * real, depth-guarded lisp_eval(), and through refresh_global_tail() so an
 * earlier form's `define` is visible to a later one) and hands the last
 * form back unevaluated -- NULL if `body` is empty -- so the caller can
 * splice it into the trampoline in lisp_eval_step() as a tail position
 * instead of recursing into it. This is the one piece of plumbing tail-call
 * optimization needs: everywhere a form used to be evaluated via "res =
 * lisp_eval(last, env); return res;", it instead becomes "val = last;
 * goto tail_call;". */
static lisp_val_t *eval_all_but_last(lisp_val_t *body, lisp_val_t *env, lisp_val_t *original_tail, bool was_global) {
    if (!body || body->type != LISP_PAIR) return NULL;
    lisp_val_t *c = body;
    while (c->u.pair.cdr && c->u.pair.cdr->type == LISP_PAIR) {
        lisp_eval(c->u.pair.car, refresh_global_tail(env, original_tail, was_global));
        c = c->u.pair.cdr;
    }
    return c->u.pair.car;
}

/* S2 (plan/phase13_lisp_engine_extensions.md): applies an already-resolved
 * callable `fn` to an already-evaluated `args` list -- the shape `map`/
 * `filter`/`for-each`/a future `apply` primitive need (they hold a function
 * value and a list of values, not raw call syntax to evaluate), factored
 * out of what used to be inlined at the bottom of lisp_eval_step().
 *
 * Deliberately NOT part of the tail-call trampoline: a lambda's body here
 * is evaluated fully, form by form, through the real depth-guarded
 * lisp_eval() -- including its last form -- and returns a genuine C value,
 * at the cost of one more eval_depth level than the trampoline would use.
 * That is the right trade for a caller that needs an actual answer back
 * (it's a nested, non-tail operation from the caller's point of view no
 * matter what). lisp_eval_step()'s OWN tail-position lambda application
 * (below) deliberately does NOT call this -- it needs the goto-based
 * continuation instead of a real return to stay tail-call optimized, which
 * this function structurally cannot provide. */
static lisp_val_t *lisp_apply(lisp_val_t *fn, lisp_val_t *args, lisp_val_t *env) {
    if (!fn) return &nil_val;

    if (fn->type == LISP_PRIMITIVE) {
        return fn->u.prim(args, env);
    }

    if (fn->type == LISP_LAMBDA) {
        lisp_val_t *local_env = fn->u.lambda.env ? fn->u.lambda.env : global_env;
        bool was_global = (local_env == global_env);
        lisp_val_t *original_tail = local_env;
        lisp_val_t *p = fn->u.lambda.params;
        lisp_val_t *a = args;
        while (p && p->type == LISP_PAIR && a && a->type == LISP_PAIR) {
            if (p->u.pair.car->type == LISP_SYMBOL) {
                env_set(&local_env, p->u.pair.car->u.sym, a->u.pair.car);
            }
            p = p->u.pair.cdr;
            a = a->u.pair.cdr;
        }
        lisp_val_t *last = eval_all_but_last(fn->u.lambda.body, local_env, original_tail, was_global);
        if (!last) return &nil_val;
        return lisp_eval(last, refresh_global_tail(local_env, original_tail, was_global));
    }

    return &nil_val;
}

/* Evaluator. lisp_eval_step() holds the actual logic; every NON-tail
 * descent inside it (argument evaluation, an `if`/`cond` test, an operator
 * lookup, all but the last form of a body) calls the public lisp_eval()
 * below, so the depth guard in that wrapper sees every level of genuine
 * C-stack nesting. A form in TAIL position -- the branch an `if` selects,
 * the last form of `begin`/`let`/a matched `cond` clause/a lambda body, or
 * a lambda application appearing in one of those spots -- does NOT recurse:
 * it rewrites `val`/`env` in place and jumps back to `tail_call` below,
 * reusing this same C stack frame and never touching eval_depth. That is
 * what makes an ordinary tail-recursive loop, e.g.
 * (define (loop n) (if (done? n) n (loop (next n)))), run in *this* call
 * indefinitely instead of growing the call stack by one lisp_eval() per
 * iteration -- previously the only way to loop, and hard-capped at
 * LISP_MAX_EVAL_DEPTH (100) regardless of available stack.
 *
 * This bounds C-stack growth and the depth counter, not memory: node_pool /
 * string_pool allocation (e.g. binding a lambda's parameters into a fresh
 * env on every iteration) still accumulates every trampoline pass, since
 * nothing here is freed. A collector is tracked separately (plan/
 * phase13_lisp_engine_extensions.md, S0/S3) as the fix for that; this only
 * fixes unbounded call-stack/eval_depth growth. */
static lisp_val_t *lisp_eval_step(lisp_val_t *val, lisp_val_t *env) {
tail_call:
    if (!val) return &nil_val;

    if (val->type == LISP_INT || val->type == LISP_STRING || val->type == LISP_PRIMITIVE || val->type == LISP_LAMBDA) {
        return val;
    }


    if (val->type == LISP_SYMBOL) {
        lisp_val_t *res = env_get(env, val->u.sym);
        if (res) return res;
        cprintf("Unbound symbol: %s\n", val->u.sym);
        return &nil_val;
    }

    if (val->type == LISP_PAIR) {
        lisp_val_t *op = val->u.pair.car;
        lisp_val_t *args = val->u.pair.cdr;
        /* See refresh_global_tail()'s comment: whether `env` is exactly
         * global_env's identity right now, computed once per trampoline
         * iteration before any nested evaluation below could move
         * global_env on. Used throughout this PAIR branch to keep a
         * `define` earlier in the same form visible to a later part of it. */
        bool was_global = (env == global_env);

        /* Special form: quote */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "quote")) {
            return (args && args->type == LISP_PAIR) ? args->u.pair.car : &nil_val;
        }

        /* Special form: if -- the taken branch is a tail position. */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "if")) {
            if (args && args->type == LISP_PAIR && args->u.pair.cdr) {
                lisp_val_t *cond_val = lisp_eval(args->u.pair.car, refresh_global_tail(env, env, was_global));
                bool is_true = lisp_truthy(cond_val);
                if (is_true) {
                    val = args->u.pair.cdr->u.pair.car;
                    env = refresh_global_tail(env, env, was_global);
                    goto tail_call;
                } else if (args->u.pair.cdr->u.pair.cdr) {
                    val = args->u.pair.cdr->u.pair.cdr->u.pair.car;
                    env = refresh_global_tail(env, env, was_global);
                    goto tail_call;
                }
            }
            return &nil_val;
        }

        /* Special form: begin -- the last form is a tail position. */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "begin")) {
            lisp_val_t *last = eval_all_but_last(args, env, env, was_global);
            if (!last) return &nil_val;
            val = last;
            env = refresh_global_tail(env, env, was_global);
            goto tail_call;
        }

        /* Special form: let -- bindings are evaluated against the outer
         * env, not each other (that's let*, right below), the last body
         * form is a tail position in the extended env. */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "let")) {
            if (args && args->type == LISP_PAIR) {
                lisp_val_t *bindings = args->u.pair.car;
                lisp_val_t *body = args->u.pair.cdr;
                lisp_val_t *local_env = env;

                for (lisp_val_t *b = bindings; b && b->type == LISP_PAIR; b = b->u.pair.cdr) {
                    lisp_val_t *pair = b->u.pair.car;
                    if (pair && pair->type == LISP_PAIR && pair->u.pair.car->type == LISP_SYMBOL) {
                        lisp_val_t *bound = lisp_eval(pair->u.pair.cdr ? pair->u.pair.cdr->u.pair.car : &nil_val, refresh_global_tail(env, env, was_global));
                        env_set(&local_env, pair->u.pair.car->u.sym, bound);
                    }
                }

                lisp_val_t *last = eval_all_but_last(body, local_env, env, was_global);
                if (!last) return &nil_val;
                val = last;
                env = refresh_global_tail(local_env, env, was_global);
                goto tail_call;
            }
        }

        /* Special form: let* (S2, plan/phase13_lisp_engine_extensions.md)
         * -- identical to `let` above except each binding's init-expr is
         * evaluated against `local_env` (already containing every
         * preceding binding) rather than the outer `env`, giving the
         * sequential visibility `let` doesn't: (let* ((a 1) (b a)) b)
         * sees `a` while computing `b`; the same form under plain `let`
         * would report an unbound symbol. */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "let*")) {
            if (args && args->type == LISP_PAIR) {
                lisp_val_t *bindings = args->u.pair.car;
                lisp_val_t *body = args->u.pair.cdr;
                lisp_val_t *local_env = env;

                for (lisp_val_t *b = bindings; b && b->type == LISP_PAIR; b = b->u.pair.cdr) {
                    lisp_val_t *pair = b->u.pair.car;
                    if (pair && pair->type == LISP_PAIR && pair->u.pair.car->type == LISP_SYMBOL) {
                        lisp_val_t *bound = lisp_eval(pair->u.pair.cdr ? pair->u.pair.cdr->u.pair.car : &nil_val, refresh_global_tail(local_env, env, was_global));
                        env_set(&local_env, pair->u.pair.car->u.sym, bound);
                    }
                }

                lisp_val_t *last = eval_all_but_last(body, local_env, env, was_global);
                if (!last) return &nil_val;
                val = last;
                env = refresh_global_tail(local_env, env, was_global);
                goto tail_call;
            }
        }

        /* Special form: while (S2, plan/phase13_lisp_engine_extensions.md)
         * -- (while cond body...). A plain C loop, not part of the tail
         * trampoline at all: it never recurses for control flow, so it
         * needs neither TCO nor a depth-guard bound to be safe, and is the
         * cheapest possible looping construct here. Each iteration's
         * condition/body evaluation still goes through the real, depth-
         * guarded lisp_eval(), so an interrupt or node-pool exhaustion
         * mid-loop is caught the same way it always is: that call returns
         * nil, `is_true` treats nil as false (see the `if` special form
         * above for the same truthiness rule), and the loop exits on its
         * own next condition check -- no separate abort check needed here.
         * Returns nil always, matching Scheme convention that `while` is
         * for side effects, not a value. */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "while")) {
            if (args && args->type == LISP_PAIR) {
                lisp_val_t *cond_expr = args->u.pair.car;
                lisp_val_t *body = args->u.pair.cdr;
                for (;;) {
                    lisp_val_t *cond_val = lisp_eval(cond_expr, refresh_global_tail(env, env, was_global));
                    if (!lisp_truthy(cond_val)) break;
                    for (lisp_val_t *c = body; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                        lisp_eval(c->u.pair.car, refresh_global_tail(env, env, was_global));
                    }
                }
            }
            return &nil_val;
        }

        /* Special form: cond -- the last form of the matched clause is a
         * tail position. */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "cond")) {
            for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                lisp_val_t *clause = c->u.pair.car;
                if (clause && clause->type == LISP_PAIR) {
                    lisp_val_t *pred = clause->u.pair.car;
                    bool is_else = (pred->type == LISP_SYMBOL && streq(pred->u.sym, "else"));
                    lisp_val_t *pval = is_else ? &true_val : lisp_eval(pred, refresh_global_tail(env, env, was_global));
                    if (lisp_truthy(pval)) {
                        lisp_val_t *last = eval_all_but_last(clause->u.pair.cdr, env, env, was_global);
                        if (!last) return &nil_val;
                        val = last;
                        env = refresh_global_tail(env, env, was_global);
                        goto tail_call;
                    }
                }
            }
            return &nil_val;
        }

        /* Special form: define
         *
         * Two forms:
         *   (define name value-expr)             -- bind a value
         *   (define (name arg...) body-form...)  -- bind a function, i.e.
         *     sugar for (define name (lambda (arg...) body-form...))
         */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "define")) {
            if (args && args->type == LISP_PAIR) {
                lisp_val_t *target = args->u.pair.car;
                lisp_val_t *rest = args->u.pair.cdr;

                if (target && target->type == LISP_PAIR) {
                    lisp_val_t *name_sym = target->u.pair.car;
                    if (!name_sym || name_sym->type != LISP_SYMBOL) {
                        printk("[Lisp Error] define: function name must be a symbol\n");
                        return &nil_val;
                    }
                    lisp_val_t *lam = alloc_node(LISP_LAMBDA);
                    lam->u.lambda.params = target->u.pair.cdr;
                    lam->u.lambda.body = rest;
                    lam->u.lambda.env = (env == global_env) ? NULL : env;
                    env_set(&global_env, name_sym->u.sym, lam);
                    return name_sym;
                }

                if (!target || target->type != LISP_SYMBOL) {
                    printk("[Lisp Error] define: expected a symbol or (name arg...)\n");
                    return &nil_val;
                }

                lisp_val_t *eval_val = (rest && rest->type == LISP_PAIR)
                    ? lisp_eval(rest->u.pair.car, env) : &nil_val;
                env_set(&global_env, target->u.sym, eval_val);
                return target;
            }
        }

        /* Special form: lambda -- (lambda (arg...) body-form...) */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "lambda")) {
            lisp_val_t *params = (args && args->type == LISP_PAIR) ? args->u.pair.car : &nil_val;
            lisp_val_t *body = (args && args->type == LISP_PAIR) ? args->u.pair.cdr : &nil_val;
            lisp_val_t *lam = alloc_node(LISP_LAMBDA);
            lam->u.lambda.params = params;
            lam->u.lambda.body = body;
            /* A lambda created directly in the global scope closes over the
             * LIVE global environment (NULL is the marker, resolved at call
             * time below) rather than a frozen snapshot of it -- otherwise
             * a function couldn't see its own binding while evaluating its
             * own body: (define ...) prepends onto global_env *after* this
             * lambda value has already captured whatever global_env was
             * before that happened (see B3 in
             * plan/completed/2026-08-07_review_and_remediation.md). Lambdas created
             * inside another lambda's body or a `let` still get a normal
             * frozen-snapshot closure, which is correct lexical scoping. */
            lam->u.lambda.env = (env == global_env) ? NULL : env;
            return lam;
        }


        /* Evaluate Operator */
        lisp_val_t *fn = lisp_eval(op, refresh_global_tail(env, env, was_global));
        if (!fn) return &nil_val;
        /* An abort (pool exhaustion, Ctrl-C, depth exceeded) discovered
         * during this lisp_eval() call surfaces here as fn == &nil_val,
         * not as a clean unwind all the way to lisp_eval()'s own caller --
         * the trampoline above means this call frame IS the top for a
         * whole tail-recursive loop, not just one link in a recursing
         * chain. Left unchecked, `fn` is simply not callable below and the
         * fallback at the end of this function returns the raw, unevaluated
         * tail-call form instead of nil, which is confusing and, more
         * importantly, breaks the documented contract that an aborted
         * evaluation degrades to nil (see lisp_eval()'s own comments). */
        if (node_pool_exhausted_warned || lisp_interrupted) return &nil_val;

        /* Evaluate Arguments */
        lisp_val_t *eval_args_head = &nil_val;
        lisp_val_t *eval_args_tail = NULL;

        for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
            lisp_val_t *ev = lisp_eval(c->u.pair.car, refresh_global_tail(env, env, was_global));
            lisp_val_t *new_p = make_pair(ev, &nil_val);
            if (!eval_args_tail) {
                eval_args_head = new_p;
                eval_args_tail = new_p;
            } else {
                eval_args_tail->u.pair.cdr = new_p;
                eval_args_tail = new_p;
            }
        }

        if (fn->type == LISP_PRIMITIVE) {
            return lisp_apply(fn, eval_args_head, env);
        }

        if (fn->type == LISP_LAMBDA) {
            /* Deliberately NOT routed through lisp_apply() (S2, plan/
             * phase13_lisp_engine_extensions.md) even though that function
             * handles this exact case too: this call is in tail position
             * of the trampoline, and lisp_apply() always fully recurses
             * and returns, which would undo S1's tail-call optimization.
             * The logic below is the same as lisp_apply()'s LISP_LAMBDA
             * branch up through parameter binding, then diverges for the
             * body's last form (goto tail_call instead of a real return).
             *
             * Create new scope extending lambda environment. NULL means
             * this closure was defined at global scope -- resolve against
             * whatever global_env is *right now*, not a stale snapshot
             * (see the lambda special form above and B3 in
             * plan/completed/2026-08-07_review_and_remediation.md). */
            lisp_val_t *local_env = fn->u.lambda.env ? fn->u.lambda.env : global_env;
            bool lambda_was_global = (local_env == global_env);
            lisp_val_t *original_tail = local_env;
            lisp_val_t *p = fn->u.lambda.params;
            lisp_val_t *a = eval_args_head;
            while (p && p->type == LISP_PAIR && a && a->type == LISP_PAIR) {
                if (p->u.pair.car->type == LISP_SYMBOL) {
                    env_set(&local_env, p->u.pair.car->u.sym, a->u.pair.car);
                }
                p = p->u.pair.cdr;
                a = a->u.pair.cdr;
            }
            /* Body is a list of forms (see the lambda special form above),
             * evaluated in sequence like `begin` -- the last is a tail
             * position: this is what lets a self- or mutually-recursive
             * call in tail position loop instead of recursing (see the
             * lisp_eval_step() comment above). */
            lisp_val_t *last = eval_all_but_last(fn->u.lambda.body, local_env, original_tail, lambda_was_global);
            if (!last) return &nil_val;
            val = last;
            env = refresh_global_tail(local_env, original_tail, lambda_was_global);
            goto tail_call;
        }
    }

    return val;
}

/* This interpreter recurses on the C stack with no other bound, and every
 * NON-tail `if`/`begin`/`let`/`cond`/function-call sub-evaluation above
 * (argument evaluation, a test expression, all but the last form of a body)
 * descends through this wrapper -- so a runaway or accidentally-
 * nonterminating NON-tail-recursive definition (e.g. `(+ 1 (f (+ n 1)))`,
 * where the recursive call is an argument, not the whole result) would
 * otherwise overflow the C stack directly (see A4 in
 * plan/completed/2026-08-07_review_and_remediation.md), which on a
 * freestanding kernel has no guard page and no signal handler to recover
 * from it. LISP_MAX_EVAL_DEPTH (defined near the top of this file, with
 * eval_depth/eval_depth_exceeded_warned) is a conservative default, not a
 * profiled figure -- the smallest target (RP2350) has not been
 * stack-profiled under this evaluator, so this errs toward stopping well
 * before real exhaustion rather than trying to use the full available
 * stack.
 *
 * A *tail*-recursive definition, e.g. `(define (loop n) (loop (+ n 1)))`,
 * does not hit this guard at all: lisp_eval_step()'s trampoline (see its own
 * comment) reuses this same call instead of recursing for the tail call
 * itself, so eval_depth only reflects genuine nesting, never the length of
 * a tail-recursive loop. That loop can still run forever in wall-clock
 * terms -- see the Ctrl-C poll below, and node_pool/string_pool exhaustion,
 * which is what bounds a single such command even with the S3 collector in
 * place (see gc_collect()'s own comment for exactly why: there is no safe
 * point *inside* one still-executing top-level form to collect at). */
lisp_val_t *lisp_eval(lisp_val_t *val, lisp_val_t *env) {
    /* An exhausted node pool is not survivable by carrying on. alloc_node()
     * clamps to its last slot when it runs out, so every further allocation
     * returns the same node and any cons cell built from two of them points
     * at itself -- after which the first list walker to touch it spins
     * forever. See alloc_node() for the full chain and what it cost.
     *
     * Refusing here, in the same place and the same shape as the depth guard
     * below, is what makes that unreachable: evaluation unwinds instead of
     * descending, so no structure is ever built out of the aliased node. The
     * shell stays alive and answers -- degraded, returning nil for everything
     * until it restarts, which is what the warning already promised -- rather
     * than taking the machine down with it. */
    if (eval_depth == 0) {
        /* S3: one of the collector's safe points (see lisp_gc_safepoint()'s
         * declaration in lisp.h, and gc_collect()'s own comment, for why
         * nowhere mid-expression qualifies). eval_depth==0 genuinely
         * happens during boot -- lisp_init() evaluates stdlib.lisp/
         * init.lisp form by form via lisp_eval_string(), returning to 0
         * between each -- but NOT during the interactive session that
         * follows: init.lisp's last form is (lsh), which blocks for the
         * rest of the session inside shell_run() inside THIS lisp_eval()
         * call, so eval_depth sits at >=1 the whole time a user is typing
         * (found live, not assumed -- debugged via instrumentation after
         * this check alone silently never fired past boot). This covers
         * the boot-time case; lisp_repl()'s and kernel/shell.c's own
         * per-command loops cover the interactive ones. */
        lisp_gc_safepoint();
    }
    if (node_pool_exhausted_warned) {
        return &nil_val;
    }
    if (eval_depth == 0) {
        /* A fresh top-level call (lisp_repl()/lisp_eval_string()), not a
         * recursive one -- eval_depth is never 0 mid-evaluation, since the
         * caller that reaches this point has already incremented it below
         * before making any further nested lisp_eval() call. Clear any
         * interrupt left latched from a previous evaluation here, so
         * Ctrl-C aborts only the call it was pressed during, not every one
         * after it.
         *
         * eval_poll_count resets here too -- found live, not assumed: left
         * as a single ever-incrementing counter across the whole session,
         * the 1024-call poll would eventually land mid-evaluation of some
         * ordinary *fast* command purely by cumulative chance, draining
         * whatever a caller had already queued up next on the same input
         * stream (tests/runner.py pipelines several commands as one write,
         * e.g. "(loop 0)\n(+ 5 5)\nexit" -- two existing regression tests
         * broke exactly this way). Resetting per top-level call means only
         * a single evaluation that itself performs >=1024 lisp_eval() calls
         * -- LISP_MAX_EVAL_DEPTH=100 keeps any ordinary recursion far below
         * that -- can ever trigger the drain, which is the actual intent:
         * a wide-but-depth-bounded blowup, not routine use. */
        lisp_interrupted = false;
        eval_poll_count = 0;
    }
    if (lisp_interrupted) {
        return &nil_val;
    }
    if (eval_depth >= LISP_MAX_EVAL_DEPTH) {
        if (!eval_depth_exceeded_warned) {
            printk("[Lisp Error] Maximum evaluation depth (%d) exceeded -- "
                   "aborting to avoid a C stack overflow\n", LISP_MAX_EVAL_DEPTH);
            eval_depth_exceeded_warned = true;
        }
        return &nil_val;
    }
    /* Throttled the same way search.c's check_up_time() throttles its own
     * poll (every 2048 nodes there) -- uart_has_char() inside
     * console_interrupt_requested() isn't free enough to call on literally
     * every recursive step. The interval matters more here than it does
     * for search.c, though: console_interrupt_requested() drains and
     * discards *everything* waiting on the input device, Ctrl-C or not
     * (its own header comment in kernel/console.h says why), so any poll
     * that fires while a caller has already queued up its next command on
     * the same stream destroys that queued input. tests/runner.py does
     * exactly this -- pipelines several commands in one write -- and an
     * earlier, much lower threshold here (1024) broke two existing
     * regression tests that way: found live, not a hypothetical, since a
     * single depth-100-bounded recursive call turned out to cost more than
     * 1024 total lisp_eval() invocations (every argument sub-expression is
     * its own call too, not just the tail-recursive chain itself) well
     * before LISP_MAX_EVAL_DEPTH ever cuts it off. ~1M calls in comparison
     * gives generous headroom above what any single bounded-depth (<=100)
     * recursive chain can produce, while still being a small fraction of a
     * second of real time for the genuinely wide, long-running computation
     * (many separate depth-bounded calls, e.g. over a large list) this
     * exists for -- responsiveness to an actual runaway script is
     * unaffected in any way a human would notice. */
    if ((++eval_poll_count & 0xFFFFFUL) == 0 && console_interrupt_requested()) {
        printk("[Lisp] interrupted by Ctrl-C\n");
        lisp_interrupted = true;
        console_interrupt_clear();
        return &nil_val;
    }
    eval_depth++;
    lisp_val_t *result = lisp_eval_step(val, env);
    eval_depth--;
    return result;
}

void lisp_repl(void) {
    cprintf("\n==================================================\n");
    cprintf("       LugalOS Scheme / S-Expression REPL         \n");
    cprintf("  Type expressions like (+ 10 20) or (cat /proc/ps)\n");
    cprintf("  Type 'exit' to return to lugal shell.            \n");
    cprintf("==================================================\n");

    char buf[128];
    while (1) {
        cprintf("lisp> ");
        int idx = 0;
        while (1) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                buf[idx] = '\0';
                break;
            } else if (c == 0x08 || c == 0x7F) {
                if (idx > 0) {
                    idx--;
                    uart_puts("\b \b");
                }
            } else if (c >= 32 && c <= 126) {
                if (idx < 127) {
                    buf[idx++] = c;
                    uart_putc(c);
                    /* M4: uart_putc() batches now -- without this, this
                     * keystroke's echo only becomes visible whenever the
                     * *next* uart_getc() call happens to flush it, a
                     * one-character lag a real user would notice. */
                    uart_flush();
                }
            }
        }

        if (streq(buf, "exit")) break;
        if (idx == 0) continue;

        /* S3: the safe point for the interactive session -- see
         * gc_collect()'s comment for why a collection is only exact
         * between complete top-level forms, and lisp_eval()'s own
         * eval_depth==0 check for why *that* check alone never fires here
         * (this loop runs nested inside the (lsh) call that is this
         * session's own top-level form, at eval_depth>=1 for its whole
         * duration). Right here -- between one typed line's evaluation
         * finishing and the next one starting -- is exact for the same
         * reason eval_depth==0 is: whatever the previous line evaluated
         * has fully returned, so global_env is again the only thing that
         * matters. Relies on (lsh)/lisp_repl() only ever being reached as
         * a complete top-level form itself, never nested inside another
         * expression's still-in-progress evaluation (e.g. as an argument
         * to something else) -- true of every call site in this tree
         * today, not enforced structurally. */
        lisp_gc_safepoint();

        const char *ptr = buf;
        lisp_val_t *ast = lisp_read(&ptr);
        lisp_val_t *result = lisp_eval(ast, global_env);
        cprintf("=> ");
        lisp_print(result);
        cprintf("\n");
    }
}

lisp_val_t *lisp_eval_string(const char *str) {
    if (!str) return &nil_val;
    const char *ptr = str;
    lisp_val_t *res = &nil_val;
    while (*ptr != '\0') {
        skip_whitespace(&ptr);
        if (*ptr == '\0') break;
        lisp_val_t *ast = lisp_read(&ptr);
        if (!ast) break;
        res = lisp_eval(ast, global_env);
    }
    return res;
}

