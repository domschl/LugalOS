#include "lisp.h"
#include "kernel/printk.h"
#include "kernel/scratch.h"
#include "kernel/path.h"
#include "kernel/console.h"
#include "kernel/line_editor.h"
#include "kernel/clipboard.h"
#include "kernel/screenshot.h"
#include "drivers/screen.h"
#include "kernel/shell.h"
#include "kernel/time.h"
#include "kernel/palloc.h"   /* BULK_BSS */
#include "drivers/psram_rp2350.h"
#include "drivers/i2c_rtc.h"
#include "drivers/i2c_bus.h"
#include "drivers/at24c32.h"
#include "drivers/loopback_net.h"
#include "drivers/uart_net.h"
#include "drivers/usb_cdc.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/device.h"
#include "net/ip.h"
#include "net/tcp.h"
#include "net/ntp.h"
#include "net/mqttd.h"
#include <limits.h>
#include "kernel/identity.h"
#include "kernel/sha256.h"
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
#include "search.h"
#include "tt.h"
#include "pgn.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
#include "drivers/pico_clock_green.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
#include "drivers/dcf77.h"
#include "drivers/dcf77_service.h"
#endif
#include "kernel/timezone.h"
#include "arch/elf.h"
#include <string.h>


#if defined(CONFIG_LISP_NODE_POOL)
/* 37.3a: a board file may size the pool itself. The RP2350-LCD-7 persona,
 * whose point is to be a Lisp machine with a screen, takes 2048: the
 * showcase demos' cellular automaton keeps two rows of ~156 cells alive at
 * once, and 1024 nodes could not hold that beside the loaded program. */
#define NODE_POOL_SIZE CONFIG_LISP_NODE_POOL
#elif defined(CONFIG_BOARD_RP2350)
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
 * 1024 restores headroom again. Contrary to what this comment used to
 * claim, that RAM cost is *not* outside palloc's managed page budget:
 * node_pool is .bss, every linker script's _heap_end is a fixed physical-
 * RAM address, and _kernel_end (.bss's own end, palloc_init()'s heap
 * *start*) moves up as .bss grows -- so growing this array shrinks the
 * managed heap by exactly its own growth, page for page. Confirmed the
 * hard way (this file's own plan/phase13_lisp_engine_extensions.md's
 * chess-won't-start regression): STRING_POOL_SIZE below scaling 1:1 with this
 * constant's own past growth (512->768->1024) had, by the time chess's
 * own on-demand move-list pools (user/chess/src/search.c) needed to
 * allocate, quietly eaten enough of the heap that they no longer fit. */
#define NODE_POOL_SIZE 1024
#elif defined(CONFIG_BOARD_ESP32P4)
/* E2, plan/phase27_esp32p4_bringup.md: the RP2350 figure, for the RP2350
 * reason, on a board with the same shape of budget.
 *
 * The QEMU 4096 below is free against 128 MB. Here it is not: measured on
 * the first link of this target, user/lisp/lisp.c alone carried 181,468
 * bytes of .bss -- and this kernel is loaded into 512 KB of L2MEM
 * (linker/esp32p4.ld), so the image overflowed its region by 2352 bytes and
 * would not link at all. The comment above already states the mechanism:
 * node_pool is .bss, _heap_end is a fixed address, so growing this array
 * shrinks the managed heap page for page. On a 512 KB board that is the
 * whole budget.
 *
 * E2's plan text asked for this differently -- build with LUGALOS_ENABLE_LISP
 * off. Board-scoping the pool is what it actually got, and the reasoning is
 * in the plan under E2: there is no ENABLE_LISP flag in this tree, inventing
 * one would put a gate no other target exercises across a dozen call sites
 * in common code, and the size problem is a *pool sizing* problem that this
 * file already has the mechanism for. The 1024 figure is not new either --
 * it is the one phase 13's S4 arrived at empirically on hardware, on a board
 * with 520 KB.
 *
 * **Phase 32 raised it for the P4 and left the RP2350 alone**, because the
 * constraint was memory and only one of the two boards lost it. That board's
 * heap went from 128 KB to 372 KB when .text moved to flash, and 1024 nodes
 * is visibly too few for it: `(fib 14)` -- 1,219 calls, each allocating
 * several cells -- exhausted the pool. Note what does *not* explain that.
 * `fib` has no tail calls, so tail-call elimination is irrelevant; and the
 * collector could not help then, because it had no safe point inside a single
 * still-executing top-level form to collect at. It was purely a pool smaller
 * than one expression's garbage. (Since 37.3a the collector runs inside a
 * form too -- gc_collect_in_form() -- so this now bounds only what one form
 * keeps *alive*, not what it allocates.)
 *
 * 2048 and not more, and the ceiling is not the heap. node_pool is .bss, and
 * after phase 32 split the image .bss lives in LOWRAM's 252 KB while the heap
 * lives elsewhere entirely -- so on this board the pool competes with the
 * boot stack, not with palloc. Measured headroom between _bss_end and
 * _stack_bottom: 44 KB at 2048 nodes, 24 KB at 3072, and **3.6 KB at 4096**,
 * which is close enough to the ASSERT that the next .bss growth anywhere in
 * the tree would trip it. Doubling is the useful part; quadrupling spends a
 * different budget than the one that was freed. */
#if defined(CONFIG_BOARD_ESP32P4)
#define NODE_POOL_SIZE 2048
#else
#define NODE_POOL_SIZE 1024
#endif
#else
#define NODE_POOL_SIZE 4096
#endif



/* 38.5, plan/phase38_psram.md: the pools are BULK_BSS -- PSRAM on a board
 * that has one (the LCD-7: 65 536 nodes, 1 MB, measured 1.7x slower than
 * SRAM and accepted for the size), ordinary .bss everywhere else. The mark
 * bitmaps further down stay in SRAM: they are small and hit at random. */
static lisp_val_t node_pool[NODE_POOL_SIZE] BULK_BSS;

static int node_pool_idx = 0;
static bool node_pool_exhausted_warned = false;

/* S3 (plan/phase13_lisp_engine_extensions.md): free-list head for reclaimed
 * node_pool cells, threaded through the existing pair.cdr field (no extra
 * storage) -- see gc_collect() further down for how cells get here. NULL
 * means empty, i.e. every collection so far has reclaimed nothing new. */
static lisp_val_t *node_free_list = NULL;
/* 38.5: the free lists' lengths, kept as they change rather than walked.
 * Walking was free while the pools were SRAM; in PSRAM a walk of the node
 * list is one cache miss per node, and gc_headroom_low() did it before
 * every shell command -- measured on the LCD-7 at 20 ms a command once the
 * pool had cycled (10 ms round trip fresh, 30 ms after). Changed in exactly
 * the places the lists are: the pops in alloc_node()/take_small()/
 * take_large() and the pushes in the sweep. */
static int node_free_count;
static int string_small_free_count;
static int string_large_free_count;

/* Guards against unbounded C-stack recursion in lisp_eval() (see its
 * definition near the bottom of this file for the full rationale). */
#define LISP_MAX_EVAL_DEPTH 256
static int eval_depth = 0;
static bool eval_depth_exceeded_warned = false;

/* J2 (plan/phase10_chess_completion.md): Ctrl-C for a run-away-but-legal
 * evaluation -- eval_depth above only catches unbounded *recursion*, not a
 * legitimately bounded-depth call that is simply expensive (an
 * exponential-blowup recursive definition well under the depth-100
 * ceiling can still run for a long time). See lisp_eval()'s own use of
 * these for the full mechanism. */
static uint8_t eval_poll_count = 0;      /* wraps: the clock every 256 calls */
static bool lisp_interrupted = false;
static uint32_t interrupt_poll_us;      /* when the console was last asked */
static bool lisp_poll_interrupt(void);

static lisp_val_t nil_val = { .type = LISP_NIL };
static lisp_val_t true_val = { .type = LISP_SYMBOL, .u.sym = "#t" };
static lisp_val_t false_val = { .type = LISP_SYMBOL, .u.sym = "#f" };

/* Flash-resident keyword singletons */
static const lisp_val_t sym_quote = { .type = LISP_SYMBOL, .u.sym = "quote" };
static const lisp_val_t sym_lambda = { .type = LISP_SYMBOL, .u.sym = "lambda" };
static const lisp_val_t sym_if = { .type = LISP_SYMBOL, .u.sym = "if" };
static const lisp_val_t sym_begin = { .type = LISP_SYMBOL, .u.sym = "begin" };
static const lisp_val_t sym_let = { .type = LISP_SYMBOL, .u.sym = "let" };
static const lisp_val_t sym_let_star = { .type = LISP_SYMBOL, .u.sym = "let*" };
static const lisp_val_t sym_while = { .type = LISP_SYMBOL, .u.sym = "while" };
static const lisp_val_t sym_cond = { .type = LISP_SYMBOL, .u.sym = "cond" };
static const lisp_val_t sym_set_bang = { .type = LISP_SYMBOL, .u.sym = "set!" };
static const lisp_val_t sym_define = { .type = LISP_SYMBOL, .u.sym = "define" };
static const lisp_val_t sym_else = { .type = LISP_SYMBOL, .u.sym = "else" };
static const lisp_val_t sym_quasiquote = { .type = LISP_SYMBOL, .u.sym = "quasiquote" };
static const lisp_val_t sym_unquote = { .type = LISP_SYMBOL, .u.sym = "unquote" };
static const lisp_val_t sym_unquote_splicing = { .type = LISP_SYMBOL, .u.sym = "unquote-splicing" };

#define SYM_HASH_SIZE 128
static lisp_val_t *sym_hash_table[SYM_HASH_SIZE];

static lisp_val_t *global_env = &nil_val;

static int streq(const char *s1, const char *s2) {
    if (s1 == s2) return 1;
    if (*s1 != *s2) return 0;
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

/* 37.3a: collection inside a form (gc_collect_in_form(), with the
 * collector below), and how often it has run -- `(gc-stats)`. */
static bool gc_collect_in_form(void);
static int gc_free_nodes(void);
static unsigned long gc_in_form_count;

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
        node_free_count--;
        v->type = type;
        return v;
    }
    /* 37.3a: the pool is dry -- collect right here, the stack's values
     * counted as roots. If that frees less than a thirty-second of the pool,
     * live data has all but filled it, and carrying on would collect again
     * after every few allocations -- quadratic, measured at 25 s on RV64 for
     * a list growing into 4096 nodes -- so the form is out of memory: the
     * exhaustion flag is set, and lisp_eval() refuses to descend further.
     *
     * The cells that collection *did* free are still handed out, and that
     * is load-bearing: the form makes a few more allocations while it
     * unwinds, and without them each would take the clamp below, which
     * reuses the pool's last slot -- live data, once the pool is full of it
     * (it is the oldest-allocated end of a filled pool, not scratch). That
     * overwrote a live node about one run in six and hung the shell. Once
     * the flag is set, no further collection runs inside the form: it
     * could only find the same nothing. */
    if (node_pool_idx >= NODE_POOL_SIZE && !node_pool_exhausted_warned && gc_collect_in_form()) {
        if (gc_free_nodes() < NODE_POOL_SIZE / 32) {
            printk("[Lisp Error] Node pool exhausted! Further evaluation will "
                   "produce wrong results until the shell restarts.\n");
            node_pool_exhausted_warned = true;
        }
        if (node_free_list) {
            lisp_val_t *v = node_free_list;
            node_free_list = v->u.pair.cdr;
            node_free_count--;
            v->type = type;
            return v;
        }
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
 * savings this pool split exists to capture.
 *
 * Deliberately NOT scaled off the *current* NODE_POOL_SIZE on RP2350
 * (unlike every other board): the two constants used to move together
 * (384 when NODE_POOL_SIZE was 768, 512 once S4 raised it to 1024), but
 * only node_pool's own growth was ever actually validated against a real
 * exhaustion failure (test_rp2350.py's C2, "Node pool exhausted" -- see
 * NODE_POOL_SIZE's own comment) -- string_pool's matching growth was
 * along for the ride on the ratio, not because anything demonstrated it
 * needed to be bigger, and every extra slot is 128 bytes straight out of
 * the same shrinking heap chess's own on-demand pools (user/chess/src/
 * search.c) need room in too. 384 is what this constant was already
 * proven sufficient at, so it stays fixed at that regardless of how much
 * further NODE_POOL_SIZE grows for its own, separately-justified reasons. */
#if defined(CONFIG_LISP_STRING_POOL)
/* 38.5: a board file may size it, as it may size the node pool. The LCD-7
 * keeps both in PSRAM and takes eight times the default (sign-off S2). */
#define STRING_POOL_SIZE CONFIG_LISP_STRING_POOL
#elif defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32P4)
/* Fixed at 384 on both real boards, and deliberately not the NODE_POOL_SIZE/2
 * ratio -- see the paragraph above for why that ratio was the bug rather than
 * the rule. */
#define STRING_POOL_SIZE 384
#else
#define STRING_POOL_SIZE (NODE_POOL_SIZE / 2)
#endif
/* The longest string, symbol or bignum limb slot this engine can hold.
 * 256 bytes accommodates 64-limb bignums (2048 bits / ~616 decimal digits). */
#define STRING_SLOT_LEN 256

/* ## Two tiers, because almost nothing needs the big one
 *
 * Every slot used to be STRING_SLOT_LEN wide, so `car`, `define` and `lambda`
 * each occupied 128 bytes. At 384 slots that was 48 KB -- the single largest
 * object in the RP2350 image, and on that board .bss and the heap are the
 * same budget.
 *
 * Measured before splitting it, by instrumenting every allocation (added and
 * fully removed): across ~1200 interned strings driven through the stdlib and
 * an evaluator stress run, **1187 were under 16 characters, 3 more under 32,
 * 2 under 64, and none at all reached 128**. Peak *live* occupancy of slots
 * needing more than 32 bytes was zero.
 *
 * So: a wide majority tier of 32-byte slots, and a small reserve of full-width
 * ones. The slot *count* is unchanged -- STRING_POOL_SIZE still bounds how
 * many strings can be interned at once -- and a short string spills into the
 * large tier when the small one is full, so this cannot intern fewer strings
 * than before. Only the bytes per slot changed.
 *
 * Tiered on every target, not just RP2350. It is not a capability traded for
 * RAM (nothing gets shorter, and spill preserves capacity), and keeping one
 * layout everywhere means the QEMU suites exercise the same index mapping,
 * the same free lists and the same spill path the board runs. */
#define STRING_SMALL_LEN   32u
#define STRING_LARGE_SLOTS (STRING_POOL_SIZE / 6)
#define STRING_SMALL_SLOTS (STRING_POOL_SIZE - STRING_LARGE_SLOTS)

_Static_assert(STRING_SMALL_LEN >= sizeof(int),
               "a free slot stores the next free index in its own bytes");
_Static_assert(STRING_LARGE_SLOTS >= 1 && STRING_SMALL_SLOTS >= 1,
               "both tiers must exist for the spill path to mean anything");

static char string_small[STRING_SMALL_SLOTS][STRING_SMALL_LEN] BULK_BSS;
static char string_large[STRING_LARGE_SLOTS][STRING_SLOT_LEN] BULK_BSS;

/* Bump indices, one per tier. */
static int string_small_idx = 0;
static int string_large_idx = 0;
static bool string_pool_exhausted_warned = false;

/* Slots are addressed by one flat index across both tiers -- [0,
 * STRING_SMALL_SLOTS) is the small tier, the rest the large -- so the GC's
 * mark bitmap, its free lists and its sweep are indexed exactly as they were
 * when there was one array. */
static inline bool slot_is_small(int idx) { return idx < (int)STRING_SMALL_SLOTS; }

static inline char *slot_at(int idx) {
    return slot_is_small(idx) ? string_small[idx]
                              : string_large[idx - (int)STRING_SMALL_SLOTS];
}

static inline uint32_t slot_capacity(int idx) {
    return slot_is_small(idx) ? STRING_SMALL_LEN : STRING_SLOT_LEN;
}

/* S3: free-list head for reclaimed string_pool slots, threaded through the
 * slot's own leading bytes (reinterpreted as an int index, -1 = end of
 * list) rather than a separate side array -- a slot is either on this
 * list (its content is the "next free" index) or handed out (its content
 * is whatever string was written into it); nothing reads a slot's bytes as
 * both at once. -1 means empty. */
static int string_free_head = -1;        /* small tier */
static int string_large_free_head = -1;  /* large tier */

/* Claims a slot from one tier, or -1. */
static int take_small(void) {
    if (string_free_head >= 0) {
        int idx = string_free_head;
        int next;
        memcpy(&next, slot_at(idx), sizeof(next));
        string_free_head = next;
        string_small_free_count--;
        return idx;
    }
    if (string_small_idx < (int)STRING_SMALL_SLOTS) return string_small_idx++;
    return -1;
}

static int take_large(void) {
    if (string_large_free_head >= 0) {
        int idx = string_large_free_head;
        int next;
        memcpy(&next, slot_at(idx), sizeof(next));
        string_large_free_head = next;
        string_large_free_count--;
        return idx;
    }
    if (string_large_idx < (int)STRING_LARGE_SLOTS) {
        return (int)STRING_SMALL_SLOTS + string_large_idx++;
    }
    return -1;
}

/* Interns `src`, choosing the narrowest tier that fits it and copying with
 * that tier's own bound.
 *
 * Copying here rather than returning a bare pointer is what keeps the two
 * widths from leaking to callers: make_str()/make_sym() used to pass
 * STRING_SLOT_LEN to strncpy_local() themselves, which would silently
 * overrun a 32-byte slot. */
static char *intern_string(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);

    /* Small first when it fits; spill to large when the small tier is out,
     * so the pool still holds STRING_POOL_SIZE strings whatever their mix
     * of sizes. A long string has only the one option. */
    int idx = (len < STRING_SMALL_LEN) ? take_small() : -1;
    if (idx < 0) idx = take_large();
    if (idx < 0 && len < STRING_SMALL_LEN) idx = take_small();
    if (idx < 0 && gc_collect_in_form()) {       /* 37.3a: collect, then try again */
        idx = (len < STRING_SMALL_LEN) ? take_small() : -1;
        if (idx < 0) idx = take_large();
    }

    if (idx < 0) {
        /* Same clamp-not-wrap policy as alloc_node() (see B6 in
         * plan/completed/2026-08-07_review_and_remediation.md): reusing slot 0 would
         * silently corrupt whatever still-live value points at it, e.g. the
         * name of a primitive bound in global_env. */
        if (!string_pool_exhausted_warned) {
            /* Which tier, because "the string pool" being full and *this*
             * string having nowhere to go are different facts: a string of
             * STRING_SMALL_LEN characters or more can only come from the
             * large tier, so this fires with most of the pool still free.
             * The plain message sent one investigation looking for a leak
             * that was not there. */
            printk("[Lisp Error] String pool exhausted -- no %s-tier slot for a "
                   "%u-character string! Further strings/symbols will alias.\n",
                   (len < STRING_SMALL_LEN) ? "small- or large" : "large",
                   (unsigned)len);
            string_pool_exhausted_warned = true;
        }
        idx = (len < STRING_SMALL_LEN) ? (int)STRING_SMALL_SLOTS - 1
                                       : (int)STRING_POOL_SIZE - 1;
    }

    char *slot = slot_at(idx);
    strncpy_local(slot, src, (int)slot_capacity(idx));
    return slot;
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
static lisp_val_t *gc_work_stack[NODE_POOL_SIZE] BULK_BSS;

static int string_slot_index(const char *slot) {
    if (!slot) return -1;
    const char *small_base = &string_small[0][0];
    if (slot >= small_base &&
        slot < small_base + (size_t)STRING_SMALL_SLOTS * STRING_SMALL_LEN) {
        return (int)((slot - small_base) / STRING_SMALL_LEN);
    }
    const char *large_base = &string_large[0][0];
    if (slot >= large_base &&
        slot < large_base + (size_t)STRING_LARGE_SLOTS * STRING_SLOT_LEN) {
        return (int)STRING_SMALL_SLOTS + (int)((slot - large_base) / STRING_SLOT_LEN);
    }
    return -1;
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

static void gc_drain(int *sp) {
    while (*sp > 0) {
        lisp_val_t *v = gc_work_stack[--*sp];
        switch (v->type) {
            case LISP_STRING:
            case LISP_SYMBOL:
                gc_mark_string_slot(v->u.str);
                break;
            case LISP_BIGNUM:
                if (v->u.bignum.limbs) gc_mark_string_slot((const char *)v->u.bignum.limbs);
                break;
            case LISP_RATIO:
                gc_push(sp, v->u.ratio.num);
                gc_push(sp, v->u.ratio.den);
                break;
            case LISP_PAIR:
                gc_push(sp, v->u.pair.car);
                gc_push(sp, v->u.pair.cdr);
                break;
            case LISP_LAMBDA:
                gc_push(sp, v->u.lambda.params);
                gc_push(sp, v->u.lambda.body);
                /* NULL means "resolve against live global_env at call
                 * time" (B3) -- nothing extra to mark, global_env is
                 * already this collection's root. */
                if (v->u.lambda.env) gc_push(sp, v->u.lambda.env);
                break;
            default:
                break; /* LISP_INT, LISP_PRIMITIVE, LISP_NIL: no children */
        }
    }
}

/* 37.3a: one word from the C stack, taken as a root if it could be a
 * pointer into a pool -- including into the *middle* of a node or a string
 * slot, which is what an optimised caller may hold (&v->u.pair.cdr, or
 * s + start inside a string). An integer that happens to look like such a
 * pointer keeps its target alive one collection longer: wasteful, never
 * wrong, which is the whole bargain of a conservative scan. */
static void gc_push_word(int *sp, uintptr_t w) {
    uintptr_t lo = (uintptr_t)node_pool, hi = (uintptr_t)(node_pool + NODE_POOL_SIZE);
    if (w >= lo && w < hi) {
        gc_push(sp, &node_pool[(w - lo) / sizeof(lisp_val_t)]);
        return;
    }
    gc_mark_string_slot((const char *)w);
}

/* 37.3a: every word of the current stack, from this frame up, as a possible
 * root. The callee-saved registers s0..s11 are stored into `regs` first,
 * which is in this frame, so a pointer that a caller keeps only in one of
 * them is on the stack by the time the scan reaches it; a caller-saved
 * register live across a call is spilled by its caller anyway.
 *
 * Explicit stores rather than __builtin_unwind_init(): on RV64 the kernel is
 * built with the D extension, so the builtin also saves fs0..fs11 -- and the
 * FPU is off in the kernel, so that is an illegal instruction (the first
 * QEMU run of this collector). Lisp has no floats; no node pointer is ever in
 * an FP register. noinline, so "this frame" is a real frame below every
 * caller's. */
#if __riscv_xlen == 64
#define GC_STORE "sd"
#else
#define GC_STORE "sw"
#endif
__attribute__((noinline)) static void gc_push_stack(int *sp, uintptr_t top) {
    volatile uintptr_t regs[12];
    __asm__ volatile(
        GC_STORE " s0, %c[w0](%[r])\n" GC_STORE " s1, %c[w1](%[r])\n"
        GC_STORE " s2, %c[w2](%[r])\n" GC_STORE " s3, %c[w3](%[r])\n"
        GC_STORE " s4, %c[w4](%[r])\n" GC_STORE " s5, %c[w5](%[r])\n"
        GC_STORE " s6, %c[w6](%[r])\n" GC_STORE " s7, %c[w7](%[r])\n"
        GC_STORE " s8, %c[w8](%[r])\n" GC_STORE " s9, %c[w9](%[r])\n"
        GC_STORE " s10, %c[w10](%[r])\n" GC_STORE " s11, %c[w11](%[r])\n"
        : : [r] "r"(regs),
            [w0] "i"(0 * sizeof(uintptr_t)), [w1] "i"(1 * sizeof(uintptr_t)),
            [w2] "i"(2 * sizeof(uintptr_t)), [w3] "i"(3 * sizeof(uintptr_t)),
            [w4] "i"(4 * sizeof(uintptr_t)), [w5] "i"(5 * sizeof(uintptr_t)),
            [w6] "i"(6 * sizeof(uintptr_t)), [w7] "i"(7 * sizeof(uintptr_t)),
            [w8] "i"(8 * sizeof(uintptr_t)), [w9] "i"(9 * sizeof(uintptr_t)),
            [w10] "i"(10 * sizeof(uintptr_t)), [w11] "i"(11 * sizeof(uintptr_t))
        : "memory");
    for (unsigned i = 0; i < 12u; i++) gc_push_word(sp, regs[i]);
    uintptr_t here;
    __asm__ volatile("mv %0, sp" : "=r"(here));
    here &= ~(uintptr_t)(sizeof(uintptr_t) - 1u);
    for (const uintptr_t *p = (const uintptr_t *)here; (uintptr_t)p < top; p++) gc_push_word(sp, *p);
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
 * What this pass bought in S3 was the *next* top-level form no longer
 * inheriting a permanently degraded shell. A form that itself ran a pool dry
 * still failed, because there was no safe point inside it -- until 37.3a,
 * which adds the stack as a root (gc_collect_in_form() below) and calls this
 * with the stack's top from inside the allocator.
 *
 * Frees nothing by itself if nothing is garbage: node_pool_exhausted_warned
 * / string_pool_exhausted_warned are cleared below only if this pass
 * actually grew the corresponding free list, so a session with genuinely
 * no reclaimable garbage still degrades to nil exactly as it did before
 * S3, rather than retrying a hopeless collection on every later form. */
/* 38.5: the longest collection so far and how many there were, for
 * `(gc-stats)` -- with the pool in PSRAM, a full sweep is the one pause a
 * user can feel, so it is measured rather than estimated. */
static uint32_t gc_max_pause_us;
static uint32_t gc_total_ms_x1000;   /* total time collecting, in microseconds */
static unsigned long gc_collections;

static void gc_collect_from_timed(uintptr_t stack_top);

static void gc_collect_from(uintptr_t stack_top) {
    uint64_t t0 = time_get_us();
    gc_collect_from_timed(stack_top);
    uint32_t us = (uint32_t)(time_get_us() - t0);
    if (us > gc_max_pause_us) gc_max_pause_us = us;
    gc_total_ms_x1000 += us;
    gc_collections++;
}

static void gc_collect_from_timed(uintptr_t stack_top) {
    memset(node_mark_bits, 0, sizeof(node_mark_bits));
    memset(string_mark_bits, 0, sizeof(string_mark_bits));

    /* Cells already on a free list are unreachable by definition, but must
     * not be re-added to that list during the sweep below -- doing so
     * would grow it a cycle or a duplicate entry. Pre-marking them here
     * makes the sweep treat "already free" exactly like "still live":
     * either way, leave it alone. */
    int walked_nodes = 0, walked_small = 0, walked_large = 0;
    for (lisp_val_t *c = node_free_list; c; c = c->u.pair.cdr) {
        long idx = c - node_pool;
        node_mark_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
        walked_nodes++;
    }
    for (int idx = string_free_head; idx >= 0; walked_small++) {
        string_mark_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
        int next;
        memcpy(&next, slot_at(idx), sizeof(next));
        idx = next;
    }
    for (int idx = string_large_free_head; idx >= 0; walked_large++) {
        string_mark_bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
        int next;
        memcpy(&next, slot_at(idx), sizeof(next));
        idx = next;
    }
    /* The walk above is needed anyway; it costs nothing to check the
     * counters that replaced the per-command walks (38.5) against it. */
    if (walked_nodes != node_free_count || walked_small != string_small_free_count ||
        walked_large != string_large_free_count) {
        printk("[Lisp BUG] free counts %d/%d/%d, lists hold %d/%d/%d -- corrected\n",
               node_free_count, string_small_free_count, string_large_free_count,
               walked_nodes, walked_small, walked_large);
        node_free_count = walked_nodes;
        string_small_free_count = walked_small;
        string_large_free_count = walked_large;
    }

    int sp = 0;
    gc_push(&sp, global_env);
    if (stack_top) gc_push_stack(&sp, stack_top);
    gc_drain(&sp);

    /* Weak symbol pruning: unlink any unmarked symbol from the hash table */
    for (int i = 0; i < SYM_HASH_SIZE; i++) {
        lisp_val_t **prev = &sym_hash_table[i];
        while (*prev) {
            lisp_val_t *s = *prev;
            if (s < node_pool || s >= node_pool + NODE_POOL_SIZE) {
                prev = &s->u.sym_next;
                continue;
            }
            long idx = s - node_pool;
            uint8_t bit = (uint8_t)(1u << (idx % 8));
            if (!(node_mark_bits[idx / 8] & bit)) {
                *prev = s->u.sym_next;
            } else {
                prev = &s->u.sym_next;
            }
        }
    }

    for (int i = 0; i < node_pool_idx; i++) {
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (!(node_mark_bits[i / 8] & bit)) {
            node_pool[i].u.pair.cdr = node_free_list;
            node_free_list = &node_pool[i];
            node_free_count++;
        }
    }
    if (node_free_list) node_pool_exhausted_warned = false;

    /* Each tier's reclaimed slots go back on that tier's own free list, so a
     * 32-byte slot can never be handed out for a 128-byte string. */
    for (int i = 0; i < string_small_idx; i++) {
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (!(string_mark_bits[i / 8] & bit)) {
            memcpy(slot_at(i), &string_free_head, sizeof(string_free_head));
            string_free_head = i;
            string_small_free_count++;
        }
    }
    for (int k = 0; k < string_large_idx; k++) {
        int i = (int)STRING_SMALL_SLOTS + k;
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (!(string_mark_bits[i / 8] & bit)) {
            memcpy(slot_at(i), &string_large_free_head, sizeof(string_large_free_head));
            string_large_free_head = i;
            string_large_free_count++;
        }
    }
    if (string_free_head >= 0 || string_large_free_head >= 0) {
        string_pool_exhausted_warned = false;
    }
}

/* The precise collection, from global_env alone: exact between top-level
 * forms, which is the only place it runs (lisp_gc_safepoint()). */
static void gc_collect(void) {
    gc_collect_from(0);
}

/* 37.3a, plan/phase37_screen_layouts_and_apps.md: collection *inside* a
 * form, when a pool has run dry. The precise collector cannot run there --
 * the values a half-evaluated form is working on live in C locals, not in
 * global_env (gc_collect_from()'s comment above). So this one also takes
 * every word of the evaluating task's stack that points into a pool as a
 * root (gc_push_stack()): Boehm's conservative scan, which is safe for the
 * reason the comment above says the precise one is not -- whatever a C
 * frame holds, it holds on that stack or in a register the scan saves.
 *
 * Only the evaluating task's stack, and that is complete: Lisp is only ever
 * evaluated from the shell (kernel/shell.c, all on one stack), and nothing
 * else in the tree holds a node in a static (checked 2026-10-01: global_env
 * and the free list are the only ones). Refuses -- returning false, so the
 * caller fails as before -- when the stack's bounds are not known, or when
 * a collection is already running.
 *
 * Interior pointers count (gc_push_word()), and a freshly allocated node
 * whose fields are not yet filled is harmless: its stale fields point at
 * nodes or slots that are either still free (pre-marked, so not followed)
 * or valid, so marking through them only keeps garbage a little longer. */
static bool gc_in_progress;

static bool gc_collect_in_form(void) {
    uintptr_t lo, hi, here = (uintptr_t)__builtin_frame_address(0);
    if (gc_in_progress || !sched_current_stack(&lo, &hi) || here < lo || here >= hi) return false;
    gc_in_progress = true;
    gc_collect_from(hi);
    gc_in_progress = false;
    gc_in_form_count++;
    return true;
}

/* How much must still be claimable for the next top-level form to be given a
 * clear run at it. Written when there was no safe point inside a form, so the
 * reserve had to cover the *largest* ordinary form; since 37.3a a form that
 * runs short collects in place instead, and this reserve only makes that
 * rarer and cheaper (a collection between forms needs no stack scan).
 *
 * Per tier, and that is the whole point rather than a detail. The tiers are
 * not interchangeable: a string of 32 characters or more can only ever come
 * from the large tier, which is a sixth of the slots (64 of them on RP2350
 * and the ESP32-P4). So the large tier runs dry while five sixths of the pool
 * is still free, and any check on the pool as a whole -- which is what this
 * was written as first -- sees nothing wrong right up to the failure. That is
 * measured, not argued: 340 shell `write`s of a 33-character payload exhaust
 * rv64's 341-slot large tier with 837 slots still free overall. */
#define GC_NODE_HEADROOM        (NODE_POOL_SIZE / 8)
#define GC_STRING_SMALL_HEADROOM (STRING_SMALL_SLOTS / 8)
#define GC_STRING_LARGE_HEADROOM (STRING_LARGE_SLOTS / 4)

/* Counted, not walked (38.5): see node_free_count. These were walks while the
 * pools were SRAM, on the argument that one place to read beats five places
 * to keep in agreement; PSRAM made the walk the slow part of every command. */
static int gc_free_nodes(void) {
    return node_free_count;
}

static bool gc_headroom_low(void) {
    int nodes = (NODE_POOL_SIZE - node_pool_idx) + gc_free_nodes();
    int small = ((int)STRING_SMALL_SLOTS - string_small_idx) + string_small_free_count;
    int large = ((int)STRING_LARGE_SLOTS - string_large_idx) + string_large_free_count;
    return nodes  < GC_NODE_HEADROOM
        || small  < (int)GC_STRING_SMALL_HEADROOM
        || large  < (int)GC_STRING_LARGE_HEADROOM;
}

/* Public wrapper (declared in lisp.h) -- see its own comment there for who
 * calls this and why each call site is a genuine safe point.
 *
 * Collecting on the exhaustion flags alone was reactive by one form: the
 * command that ran the pool dry still failed, and only the one after it got
 * a collected heap. Every shell command is a Lisp form (kernel/shell.c
 * translates it to an S-expression), so that made roughly one shell command
 * in sixty fail outright once the pool filled -- measured on the ESP32-P4 as
 * write 61 of a 120-write soak dying on `[Lisp Error] String pool exhausted!`
 * with the filesystem entirely innocent (plan/open_issues.md). Collecting
 * while there is still headroom means the form that would have failed never
 * runs short in the first place.
 *
 * The flags are still tested, and first: they mean a form has *already*
 * failed, which is worth a collection whatever the headroom arithmetic
 * says. */
void lisp_gc_safepoint(void) {
    /* Between two commands is also where a Ctrl-C ends: it stops the
     * command it was pressed during, not every one after it. lisp_eval()
     * cannot decide that by itself at eval_depth 0, because inside lsh the
     * depth never returns to 0 -- (shell) is the boot script's own form --
     * and a Ctrl-C there used to turn every later line into nil. */
    lisp_interrupted = false;
    eval_depth_exceeded_warned = false;
    if (node_pool_exhausted_warned || string_pool_exhausted_warned ||
        gc_headroom_low()) {
        gc_collect();
    }
}

/* 37.3a: the integers -16..255 are shared constant nodes (in flash on the
 * RP2350), not allocated: a loop counter, a list of 0s and 1s, a colour or
 * a coordinate under 256 then costs the pool nothing. Safe because nothing
 * writes an integer node after make_int() made it, and the collector leaves
 * nodes outside the pool alone. */
#define SMALL_INT_MIN (-16)
#define SMALL_INT_MAX (255)
#define SI(v) { .type = LISP_INT, .u.i = (v) }
static const lisp_val_t small_ints[SMALL_INT_MAX - SMALL_INT_MIN + 1] = {
    SI(-16), SI(-15), SI(-14), SI(-13), SI(-12), SI(-11), SI(-10), SI(-9), SI(-8), SI(-7),
    SI(-6), SI(-5), SI(-4), SI(-3), SI(-2), SI(-1), SI(0), SI(1), SI(2), SI(3), SI(4), SI(5),
    SI(6), SI(7), SI(8), SI(9), SI(10), SI(11), SI(12), SI(13), SI(14), SI(15), SI(16), SI(17),
    SI(18), SI(19), SI(20), SI(21), SI(22), SI(23), SI(24), SI(25), SI(26), SI(27), SI(28),
    SI(29), SI(30), SI(31), SI(32), SI(33), SI(34), SI(35), SI(36), SI(37), SI(38), SI(39),
    SI(40), SI(41), SI(42), SI(43), SI(44), SI(45), SI(46), SI(47), SI(48), SI(49), SI(50),
    SI(51), SI(52), SI(53), SI(54), SI(55), SI(56), SI(57), SI(58), SI(59), SI(60), SI(61),
    SI(62), SI(63), SI(64), SI(65), SI(66), SI(67), SI(68), SI(69), SI(70), SI(71), SI(72),
    SI(73), SI(74), SI(75), SI(76), SI(77), SI(78), SI(79), SI(80), SI(81), SI(82), SI(83),
    SI(84), SI(85), SI(86), SI(87), SI(88), SI(89), SI(90), SI(91), SI(92), SI(93), SI(94),
    SI(95), SI(96), SI(97), SI(98), SI(99), SI(100), SI(101), SI(102), SI(103), SI(104),
    SI(105), SI(106), SI(107), SI(108), SI(109), SI(110), SI(111), SI(112), SI(113), SI(114),
    SI(115), SI(116), SI(117), SI(118), SI(119), SI(120), SI(121), SI(122), SI(123), SI(124),
    SI(125), SI(126), SI(127), SI(128), SI(129), SI(130), SI(131), SI(132), SI(133), SI(134),
    SI(135), SI(136), SI(137), SI(138), SI(139), SI(140), SI(141), SI(142), SI(143), SI(144),
    SI(145), SI(146), SI(147), SI(148), SI(149), SI(150), SI(151), SI(152), SI(153), SI(154),
    SI(155), SI(156), SI(157), SI(158), SI(159), SI(160), SI(161), SI(162), SI(163), SI(164),
    SI(165), SI(166), SI(167), SI(168), SI(169), SI(170), SI(171), SI(172), SI(173), SI(174),
    SI(175), SI(176), SI(177), SI(178), SI(179), SI(180), SI(181), SI(182), SI(183), SI(184),
    SI(185), SI(186), SI(187), SI(188), SI(189), SI(190), SI(191), SI(192), SI(193), SI(194),
    SI(195), SI(196), SI(197), SI(198), SI(199), SI(200), SI(201), SI(202), SI(203), SI(204),
    SI(205), SI(206), SI(207), SI(208), SI(209), SI(210), SI(211), SI(212), SI(213), SI(214),
    SI(215), SI(216), SI(217), SI(218), SI(219), SI(220), SI(221), SI(222), SI(223), SI(224),
    SI(225), SI(226), SI(227), SI(228), SI(229), SI(230), SI(231), SI(232), SI(233), SI(234),
    SI(235), SI(236), SI(237), SI(238), SI(239), SI(240), SI(241), SI(242), SI(243), SI(244),
    SI(245), SI(246), SI(247), SI(248), SI(249), SI(250), SI(251), SI(252), SI(253), SI(254),
    SI(255),
};
#undef SI

lisp_val_t *make_int(long val) {
    if (val >= SMALL_INT_MIN && val <= SMALL_INT_MAX)
        return (lisp_val_t *)&small_ints[val - SMALL_INT_MIN];
    lisp_val_t *v = alloc_node(LISP_INT);
    v->u.i = val;
    return v;
}

lisp_val_t *make_str(const char *str) {
    lisp_val_t *v = alloc_node(LISP_STRING);
    char *slot = intern_string(str);
    v->u.str = slot;
    return v;
}

static inline uint32_t sym_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

lisp_val_t *make_sym(const char *sym) {
    if (!sym) sym = "";

    /* Fast check for static Flash singletons */
    switch (sym[0]) {
        case 'q':
            if (streq(sym, "quote")) return (lisp_val_t *)&sym_quote;
            if (streq(sym, "quasiquote")) return (lisp_val_t *)&sym_quasiquote;
            break;
        case 'u':
            if (streq(sym, "unquote")) return (lisp_val_t *)&sym_unquote;
            if (streq(sym, "unquote-splicing")) return (lisp_val_t *)&sym_unquote_splicing;
            break;
        case 'l':
            if (streq(sym, "lambda")) return (lisp_val_t *)&sym_lambda;
            if (streq(sym, "let")) return (lisp_val_t *)&sym_let;
            if (streq(sym, "let*")) return (lisp_val_t *)&sym_let_star;
            break;
        case 'i': if (streq(sym, "if")) return (lisp_val_t *)&sym_if; break;
        case 'b': if (streq(sym, "begin")) return (lisp_val_t *)&sym_begin; break;
        case 'w': if (streq(sym, "while")) return (lisp_val_t *)&sym_while; break;
        case 'c': if (streq(sym, "cond")) return (lisp_val_t *)&sym_cond; break;
        case 's': if (streq(sym, "set!")) return (lisp_val_t *)&sym_set_bang; break;
        case 'd': if (streq(sym, "define")) return (lisp_val_t *)&sym_define; break;
        case 'e': if (streq(sym, "else")) return (lisp_val_t *)&sym_else; break;
        case '#':
            if (streq(sym, "#t")) return &true_val;
            if (streq(sym, "#f")) return &false_val;
            break;
        default: break;
    }

    uint32_t h = sym_hash(sym);
    int bucket = (int)(h % SYM_HASH_SIZE);

    for (lisp_val_t *curr = sym_hash_table[bucket]; curr; curr = curr->u.sym_next) {
        if (streq(curr->u.sym, sym)) {
            return curr;
        }
    }

    lisp_val_t *v = alloc_node(LISP_SYMBOL);
    char *slot = intern_string(sym);
    v->u.sym = slot;
    v->u.sym_next = sym_hash_table[bucket];
    sym_hash_table[bucket] = v;
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
/* Fast pointer-equality symbol lookup in the environment */
static lisp_val_t *env_binding_sym(lisp_val_t *env, const lisp_val_t *sym) {
    for (lisp_val_t *curr = env; curr && curr->type == LISP_PAIR; curr = curr->u.pair.cdr) {
        lisp_val_t *binding = curr->u.pair.car;
        if (binding && binding->type == LISP_PAIR) {
            lisp_val_t *k = binding->u.pair.car;
            if (k == sym || (k && k->type == LISP_SYMBOL && (k->u.sym == sym->u.sym || streq(k->u.sym, sym->u.sym)))) {
                return binding;
            }
        }
    }
    return NULL;
}

/* The innermost (name . value) pair for `sym`, or NULL. `set!` writes its
 * cdr; env_get() reads it. Built out with env_get() (see there). */
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
static lisp_val_t *env_binding(lisp_val_t *env, const char *sym) {
    for (lisp_val_t *curr = env; curr && curr->type == LISP_PAIR; curr = curr->u.pair.cdr) {
        lisp_val_t *binding = curr->u.pair.car;
        if (binding && binding->type == LISP_PAIR) {
            lisp_val_t *k = binding->u.pair.car;
            if (k && k->type == LISP_SYMBOL && (k->u.sym == sym || streq(k->u.sym, sym))) {
                return binding;
            }
        }
    }
    return NULL;
}
#endif

static lisp_val_t *env_get_sym(lisp_val_t *env, const lisp_val_t *sym) {
    lisp_val_t *binding = env_binding_sym(env, sym);
    return binding ? binding->u.pair.cdr : NULL;
}

/* By name, for the canvas redraw hook only -- which the ST7735 build does
 * not have (its #else branch below), so these two are built out with it. */
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
static lisp_val_t *env_get(lisp_val_t *env, const char *sym) {
    lisp_val_t *binding = env_binding(env, sym);
    return binding ? binding->u.pair.cdr : NULL;
}
#endif

static void env_set_sym(lisp_val_t **env, lisp_val_t *sym, lisp_val_t *val) {
    lisp_val_t *binding = make_pair(sym, val);
    *env = make_pair(binding, *env);
}

#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
static void env_set(lisp_val_t **env, const char *sym, lisp_val_t *val) {
    env_set_sym(env, make_sym(sym), val);
}
#endif

/* 37.3a: the built-in names, outside the node pool. They used to be ordinary
 * global_env bindings, four nodes and a string slot each -- some 680 of the
 * RP2350's 1024 nodes and 170 of its 384 string slots, taken at boot and
 * live for good, which left a program about 400 nodes: `(load
 * "/sd0/demos/ca.lisp")` alone ran the pool dry on the board. Now each value
 * is a `static const` node (in flash on the RP2350, outside the pool, so the
 * collector neither marks nor sweeps it) and the name and value sit in this
 * table, which symbol lookup consults after the environment -- so a program's
 * own `define` of a built-in name still wins, exactly as before.
 *
 * Nothing mutates a primitive's value node; the cast that drops `const` in
 * builtin_get() is for the evaluator's signature only. A build with more
 * built-ins than BUILTIN_MAX binds the rest in global_env the old way. */
typedef struct {
    const char       *name;
    const lisp_val_t *val;
} builtin_t;

static lisp_val_t *builtin_get(const char *sym);


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

/* =========================================================================
 * Phase 42.7: Arbitrary-Precision Bignums and Exact Rationals From Scratch
 * ========================================================================= */

#define BIGNUM_MAX_LIMBS 64

static inline bool is_number_val(const lisp_val_t *v) {
    return v && (v->type == LISP_INT || v->type == LISP_BIGNUM || v->type == LISP_RATIO);
}

static inline int bn_normalize(const uint32_t *limbs, int len) {
    while (len > 0 && limbs[len - 1] == 0) len--;
    return len;
}

static uint32_t *alloc_limbs(int num_limbs, int *out_capacity) {
    if (num_limbs > BIGNUM_MAX_LIMBS) {
        printk("[Lisp Error] Bignum capacity exceeded: %d limbs > max %d limbs (%d bits)\n",
               num_limbs, BIGNUM_MAX_LIMBS, BIGNUM_MAX_LIMBS * 32);
        return NULL;
    }
    int idx = -1;
    int cap = 8;
    if (num_limbs <= 8) {
        idx = take_small();
        if (idx < 0) {
            idx = take_large();
            cap = BIGNUM_MAX_LIMBS;
        }
    } else if (num_limbs <= BIGNUM_MAX_LIMBS) {
        idx = take_large();
        cap = BIGNUM_MAX_LIMBS;
    } else {
        return NULL;
    }

    if (idx < 0 && gc_collect_in_form()) {
        if (num_limbs <= 8) {
            idx = take_small();
            cap = 8;
            if (idx < 0) {
                idx = take_large();
                cap = BIGNUM_MAX_LIMBS;
            }
        } else if (num_limbs <= BIGNUM_MAX_LIMBS) {
            idx = take_large();
            cap = BIGNUM_MAX_LIMBS;
        }
    }

    if (idx < 0) return NULL;
    if (out_capacity) *out_capacity = cap;
    uint32_t *limbs = (uint32_t *)(void *)slot_at(idx);
    memset(limbs, 0, cap * sizeof(uint32_t));
    return limbs;
}

static lisp_val_t *make_bignum_raw(int sign, int len, int capacity, uint32_t *limbs) {
    lisp_val_t *v = alloc_node(LISP_BIGNUM);
    v->u.bignum.sign = (int16_t)sign;
    v->u.bignum.len = (uint16_t)len;
    v->u.bignum.capacity = (uint16_t)capacity;
    v->u.bignum.limbs = limbs;
    return v;
}

static lisp_val_t *bn_demote_if_possible(int sign, int len, int capacity, uint32_t *limbs) {
    len = bn_normalize(limbs, len);
    if (len == 0) return make_int(0);

#if __SIZEOF_LONG__ == 8
    if (len <= 2) {
        uint64_t v = (len == 1) ? (uint64_t)limbs[0] : (((uint64_t)limbs[1] << 32) | limbs[0]);
        if (sign > 0 && v <= (uint64_t)LONG_MAX) {
            return make_int((long)v);
        }
        if (sign < 0 && v <= (uint64_t)LONG_MAX + 1ULL) {
            if (v == (uint64_t)LONG_MAX + 1ULL) return make_int(LONG_MIN);
            return make_int(-(long)v);
        }
    }
#else
    if (len == 1) {
        uint32_t v = limbs[0];
        if (sign > 0 && v <= (uint32_t)LONG_MAX) {
            return make_int((long)v);
        }
        if (sign < 0 && v <= (uint32_t)LONG_MAX + 1UL) {
            if (v == (uint32_t)LONG_MAX + 1UL) return make_int(LONG_MIN);
            return make_int(-(long)v);
        }
    }
#endif

    return make_bignum_raw(sign, len, capacity, limbs);
}

static void int_to_limbs(long val, int *out_sign, int *out_len, uint32_t *limbs) {
    if (val == 0) {
        *out_sign = 1;
        *out_len = 0;
        return;
    }
    if (val < 0) {
        *out_sign = -1;
#if __SIZEOF_LONG__ == 8
        uint64_t uval = (val == LONG_MIN) ? ((uint64_t)LONG_MAX + 1ULL) : (uint64_t)(-val);
        limbs[0] = (uint32_t)(uval & 0xFFFFFFFFUL);
        limbs[1] = (uint32_t)(uval >> 32);
        *out_len = (limbs[1] ? 2 : 1);
#else
        uint32_t uval = (val == LONG_MIN) ? ((uint32_t)LONG_MAX + 1UL) : (uint32_t)(-val);
        limbs[0] = uval;
        *out_len = 1;
#endif
    } else {
        *out_sign = 1;
#if __SIZEOF_LONG__ == 8
        uint64_t uval = (uint64_t)val;
        limbs[0] = (uint32_t)(uval & 0xFFFFFFFFUL);
        limbs[1] = (uint32_t)(uval >> 32);
        *out_len = (limbs[1] ? 2 : 1);
#else
        limbs[0] = (uint32_t)val;
        *out_len = 1;
#endif
    }
}

static void get_num_limbs(const lisp_val_t *v, int *out_sign, int *out_len, const uint32_t **out_limbs, uint32_t temp_limbs[2]) {
    if (v->type == LISP_INT) {
        int_to_limbs(v->u.i, out_sign, out_len, temp_limbs);
        *out_limbs = temp_limbs;
    } else if (v->type == LISP_BIGNUM) {
        *out_sign = v->u.bignum.sign;
        *out_len = v->u.bignum.len;
        *out_limbs = v->u.bignum.limbs;
    } else {
        *out_sign = 1;
        *out_len = 0;
        *out_limbs = NULL;
    }
}

static int bn_cmp_abs(const uint32_t *a, int a_len, const uint32_t *b, int b_len) {
    a_len = bn_normalize(a, a_len);
    b_len = bn_normalize(b, b_len);
    if (a_len != b_len) return a_len > b_len ? 1 : -1;
    for (int i = a_len - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static int bn_add_abs(uint32_t *res, const uint32_t *a, int a_len, const uint32_t *b, int b_len) {
    a_len = bn_normalize(a, a_len);
    b_len = bn_normalize(b, b_len);
    if (a_len < b_len) return bn_add_abs(res, b, b_len, a, a_len);
    uint64_t carry = 0;
    int i = 0;
    for (; i < b_len; i++) {
        uint64_t sum = (uint64_t)a[i] + b[i] + carry;
        res[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    for (; i < a_len; i++) {
        uint64_t sum = (uint64_t)a[i] + carry;
        res[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    if (carry) {
        res[i++] = (uint32_t)carry;
    }
    return i;
}

static int bn_sub_abs(uint32_t *res, const uint32_t *a, int a_len, const uint32_t *b, int b_len) {
    a_len = bn_normalize(a, a_len);
    b_len = bn_normalize(b, b_len);
    int64_t borrow = 0;
    int i = 0;
    for (; i < b_len; i++) {
        int64_t diff = (int64_t)a[i] - b[i] - borrow;
        if (diff < 0) {
            diff += 0x100000000ULL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        res[i] = (uint32_t)diff;
    }
    for (; i < a_len; i++) {
        int64_t diff = (int64_t)a[i] - borrow;
        if (diff < 0) {
            diff += 0x100000000ULL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        res[i] = (uint32_t)diff;
    }
    while (i > 0 && res[i - 1] == 0) i--;
    return i;
}

static int bn_mul_abs(uint32_t *res, const uint32_t *a, int a_len, const uint32_t *b, int b_len) {
    a_len = bn_normalize(a, a_len);
    b_len = bn_normalize(b, b_len);
    if (a_len == 0 || b_len == 0) return 0;
    memset(res, 0, (a_len + b_len) * sizeof(uint32_t));
    for (int i = 0; i < a_len; i++) {
        if (a[i] == 0) continue;
        uint64_t carry = 0;
        for (int j = 0; j < b_len; j++) {
            uint64_t cur = (uint64_t)res[i + j] + (uint64_t)a[i] * (uint64_t)b[j] + carry;
            res[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        res[i + b_len] += (uint32_t)carry;
    }
    int len = a_len + b_len;
    while (len > 0 && res[len - 1] == 0) len--;
    return len;
}

static void bn_div_single(uint32_t *q, int *q_len, uint32_t *rem, const uint32_t *a, int a_len, uint32_t d) {
    a_len = bn_normalize(a, a_len);
    uint64_t r = 0;
    for (int i = a_len - 1; i >= 0; i--) {
        uint64_t cur = (r << 32) | a[i];
        q[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    int len = a_len;
    while (len > 0 && q[len - 1] == 0) len--;
    *q_len = len;
    *rem = (uint32_t)r;
}

static void bn_div_multi(uint32_t *q, int *q_len, uint32_t *r, int *r_len,
                         const uint32_t *a, int a_len, const uint32_t *b, int b_len) {
    a_len = bn_normalize(a, a_len);
    b_len = bn_normalize(b, b_len);
    memset(q, 0, (a_len + 1) * sizeof(uint32_t));
    memset(r, 0, (b_len + 1) * sizeof(uint32_t));
    int curr_r_len = 0;

    if (a_len == 0 || bn_cmp_abs(a, a_len, b, b_len) < 0) {
        *q_len = 0;
        memcpy(r, a, a_len * sizeof(uint32_t));
        *r_len = a_len;
        return;
    }

    int high_bit = 31;
    while (high_bit > 0 && !(a[a_len - 1] & (1U << high_bit))) high_bit--;
    int total_bits = (a_len - 1) * 32 + high_bit + 1;

    for (int bit = total_bits - 1; bit >= 0; bit--) {
        uint32_t carry = 0;
        for (int i = 0; i < curr_r_len; i++) {
            uint32_t next_carry = (r[i] >> 31) & 1;
            r[i] = (r[i] << 1) | carry;
            carry = next_carry;
        }
        if (carry) {
            r[curr_r_len++] = carry;
        }

        int a_word = bit / 32;
        int a_bit = bit % 32;
        int in_bit = (a[a_word] >> a_bit) & 1;
        if (in_bit) {
            r[0] |= 1;
            if (curr_r_len == 0) curr_r_len = 1;
        }

        if (bn_cmp_abs(r, curr_r_len, b, b_len) >= 0) {
            curr_r_len = bn_sub_abs(r, r, curr_r_len, b, b_len);
            q[bit / 32] |= (1U << (bit % 32));
        }
    }

    int len = a_len;
    while (len > 0 && q[len - 1] == 0) len--;
    *q_len = len;
    *r_len = bn_normalize(r, curr_r_len);
}

static int bn_shift_right_1(uint32_t *x, int len) {
    uint32_t carry = 0;
    for (int i = len - 1; i >= 0; i--) {
        uint32_t next_carry = (x[i] & 1) ? 0x80000000U : 0;
        x[i] = (x[i] >> 1) | carry;
        carry = next_carry;
    }
    while (len > 0 && x[len - 1] == 0) len--;
    return len;
}

static int bn_shift_left(uint32_t *res, const uint32_t *x, int len, int k) {
    if (len == 0 || k == 0) {
        if (res != x) memcpy(res, x, len * sizeof(uint32_t));
        return len;
    }
    int word_shift = k / 32;
    int bit_shift = k % 32;
    int new_len = len + word_shift + (bit_shift ? 1 : 0);
    memset(res, 0, new_len * sizeof(uint32_t));
    uint32_t carry = 0;
    for (int i = 0; i < len; i++) {
        uint64_t cur = ((uint64_t)x[i] << bit_shift) | carry;
        res[i + word_shift] = (uint32_t)cur;
        carry = (uint32_t)(cur >> 32);
    }
    if (carry) {
        res[len + word_shift] = carry;
    }
    while (new_len > 0 && res[new_len - 1] == 0) new_len--;
    return new_len;
}

static int bn_gcd_abs(uint32_t *res, const uint32_t *u_in, int u_len, const uint32_t *v_in, int v_len) {
    u_len = bn_normalize(u_in, u_len);
    v_len = bn_normalize(v_in, v_len);
    if (u_len == 0) { memcpy(res, v_in, v_len * sizeof(uint32_t)); return v_len; }
    if (v_len == 0) { memcpy(res, u_in, u_len * sizeof(uint32_t)); return u_len; }

    if (u_len > BIGNUM_MAX_LIMBS) u_len = BIGNUM_MAX_LIMBS;
    if (v_len > BIGNUM_MAX_LIMBS) v_len = BIGNUM_MAX_LIMBS;
    uint32_t u[BIGNUM_MAX_LIMBS + 4], v[BIGNUM_MAX_LIMBS + 4];
    memcpy(u, u_in, u_len * sizeof(uint32_t));
    memcpy(v, v_in, v_len * sizeof(uint32_t));

    int k = 0;
    while ((u[0] & 1) == 0 && (v[0] & 1) == 0) {
        u_len = bn_shift_right_1(u, u_len);
        v_len = bn_shift_right_1(v, v_len);
        k++;
    }

    while (u_len > 0 && (u[0] & 1) == 0) {
        u_len = bn_shift_right_1(u, u_len);
    }

    while (v_len > 0) {
        while ((v[0] & 1) == 0) {
            v_len = bn_shift_right_1(v, v_len);
        }
        if (bn_cmp_abs(u, u_len, v, v_len) > 0) {
            uint32_t tmp[BIGNUM_MAX_LIMBS + 4];
            memcpy(tmp, u, u_len * sizeof(uint32_t));
            int tmp_len = u_len;
            memcpy(u, v, v_len * sizeof(uint32_t));
            u_len = v_len;
            memcpy(v, tmp, tmp_len * sizeof(uint32_t));
            v_len = tmp_len;
        }
        v_len = bn_sub_abs(v, v, v_len, u, u_len);
    }

    return bn_shift_left(res, u, u_len, k);
}

static void format_uint_digits(char **p, char *end, uint32_t val, int pad_to) {
    char temp[12];
    int i = 0;
    if (val == 0) temp[i++] = '0';
    while (val > 0) {
        temp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i < pad_to) {
        temp[i++] = '0';
    }
    while (i > 0 && *p < end) {
        *(*p)++ = temp[--i];
    }
}

static void bn_to_string(const lisp_val_t *v, char *out, size_t max_out) {
    if (!v || v->type != LISP_BIGNUM) {
        if (max_out > 0) out[0] = '\0';
        return;
    }
    int len = bn_normalize(v->u.bignum.limbs, v->u.bignum.len);
    if (len == 0) {
        if (max_out > 1) { out[0] = '0'; out[1] = '\0'; }
        return;
    }
    if (len > BIGNUM_MAX_LIMBS) len = BIGNUM_MAX_LIMBS;
    uint32_t temp[BIGNUM_MAX_LIMBS + 4];
    memcpy(temp, v->u.bignum.limbs, len * sizeof(uint32_t));
    uint32_t rems[80];
    int num_rems = 0;
    while (len > 0) {
        if (num_rems >= (int)(sizeof(rems) / sizeof(rems[0]))) {
            break;
        }
        uint32_t rem = 0;
        bn_div_single(temp, &len, &rem, temp, len, 1000000000U);
        rems[num_rems++] = rem;
    }
    if (num_rems == 0) rems[num_rems++] = 0;

    char *p = out;
    char *end = out + max_out - 1;
    if (v->u.bignum.sign < 0 && p < end) *p++ = '-';
    format_uint_digits(&p, end, rems[num_rems - 1], 0);
    for (int i = num_rems - 2; i >= 0; i--) {
        format_uint_digits(&p, end, rems[i], 9);
    }
    *p = '\0';
}

static lisp_val_t *bn_from_string(const char *s, int len, int sign) {
    uint32_t res[BIGNUM_MAX_LIMBS + 4];
    memset(res, 0, sizeof(res));
    int res_len = 0;
    int idx = 0;
    while (idx < len) {
        int chunk_len = (len - idx) % 9;
        if (chunk_len == 0) chunk_len = 9;
        uint32_t chunk_val = 0;
        uint32_t mul = 1;
        for (int i = 0; i < chunk_len; i++) {
            chunk_val = chunk_val * 10 + (s[idx + i] - '0');
            mul *= 10;
        }
        idx += chunk_len;

        uint64_t carry = 0;
        for (int i = 0; i < res_len; i++) {
            uint64_t cur = (uint64_t)res[i] * mul + carry;
            res[i] = (uint32_t)cur;
            carry = cur >> 32;
        }
        if (carry) {
            if (res_len < (int)(sizeof(res) / sizeof(res[0]))) {
                res[res_len++] = (uint32_t)carry;
            } else {
                printk("[Lisp Error] Bignum string exceeds maximum capacity\n");
                return &nil_val;
            }
        }

        carry = chunk_val;
        for (int i = 0; i < res_len; i++) {
            uint64_t cur = (uint64_t)res[i] + carry;
            res[i] = (uint32_t)cur;
            carry = cur >> 32;
            if (!carry) break;
        }
        if (carry) {
            if (res_len < (int)(sizeof(res) / sizeof(res[0]))) {
                res[res_len++] = (uint32_t)carry;
            } else {
                printk("[Lisp Error] Bignum string exceeds maximum capacity\n");
                return &nil_val;
            }
        }
        if (res_len == 0 && chunk_val > 0) {
            if (res_len < (int)(sizeof(res) / sizeof(res[0]))) {
                res[res_len++] = chunk_val;
            }
        }
    }
    res_len = bn_normalize(res, res_len);
    if (res_len == 0) return make_int(0);

    int cap = 8;
    uint32_t *limbs = alloc_limbs(res_len, &cap);
    if (!limbs) return &nil_val;
    memcpy(limbs, res, res_len * sizeof(uint32_t));
    return bn_demote_if_possible(sign, res_len, cap, limbs);
}

static lisp_val_t *bn_add(lisp_val_t *a, lisp_val_t *b) {
    int a_sign, b_sign, a_len, b_len;
    const uint32_t *a_limbs, *b_limbs;
    uint32_t a_temp[2], b_temp[2];
    get_num_limbs(a, &a_sign, &a_len, &a_limbs, a_temp);
    get_num_limbs(b, &b_sign, &b_len, &b_limbs, b_temp);

    uint32_t res[BIGNUM_MAX_LIMBS + 4];
    int res_len, res_sign;

    if (a_sign == b_sign) {
        res_sign = a_sign;
        res_len = bn_add_abs(res, a_limbs, a_len, b_limbs, b_len);
    } else {
        int cmp = bn_cmp_abs(a_limbs, a_len, b_limbs, b_len);
        if (cmp == 0) return make_int(0);
        if (cmp > 0) {
            res_sign = a_sign;
            res_len = bn_sub_abs(res, a_limbs, a_len, b_limbs, b_len);
        } else {
            res_sign = b_sign;
            res_len = bn_sub_abs(res, b_limbs, b_len, a_limbs, a_len);
        }
    }
    res_len = bn_normalize(res, res_len);
    if (res_len == 0) return make_int(0);

    int cap = 8;
    uint32_t *limbs = alloc_limbs(res_len, &cap);
    if (!limbs) return &nil_val;
    memcpy(limbs, res, res_len * sizeof(uint32_t));
    return bn_demote_if_possible(res_sign, res_len, cap, limbs);
}

static lisp_val_t *bn_sub(lisp_val_t *a, lisp_val_t *b) {
    int a_sign, b_sign, a_len, b_len;
    const uint32_t *a_limbs, *b_limbs;
    uint32_t a_temp[2], b_temp[2];
    get_num_limbs(a, &a_sign, &a_len, &a_limbs, a_temp);
    get_num_limbs(b, &b_sign, &b_len, &b_limbs, b_temp);
    b_sign = -b_sign;

    uint32_t res[BIGNUM_MAX_LIMBS + 4];
    int res_len, res_sign;

    if (a_sign == b_sign) {
        res_sign = a_sign;
        res_len = bn_add_abs(res, a_limbs, a_len, b_limbs, b_len);
    } else {
        int cmp = bn_cmp_abs(a_limbs, a_len, b_limbs, b_len);
        if (cmp == 0) return make_int(0);
        if (cmp > 0) {
            res_sign = a_sign;
            res_len = bn_sub_abs(res, a_limbs, a_len, b_limbs, b_len);
        } else {
            res_sign = b_sign;
            res_len = bn_sub_abs(res, b_limbs, b_len, a_limbs, a_len);
        }
    }
    res_len = bn_normalize(res, res_len);
    if (res_len == 0) return make_int(0);

    int cap = 8;
    uint32_t *limbs = alloc_limbs(res_len, &cap);
    if (!limbs) return &nil_val;
    memcpy(limbs, res, res_len * sizeof(uint32_t));
    return bn_demote_if_possible(res_sign, res_len, cap, limbs);
}

static lisp_val_t *bn_mul(lisp_val_t *a, lisp_val_t *b) {
    int a_sign, b_sign, a_len, b_len;
    const uint32_t *a_limbs, *b_limbs;
    uint32_t a_temp[2], b_temp[2];
    get_num_limbs(a, &a_sign, &a_len, &a_limbs, a_temp);
    get_num_limbs(b, &b_sign, &b_len, &b_limbs, b_temp);

    if (a_len == 0 || b_len == 0) return make_int(0);

    uint32_t res[BIGNUM_MAX_LIMBS * 2 + 8];
    int res_len = bn_mul_abs(res, a_limbs, a_len, b_limbs, b_len);
    int res_sign = a_sign * b_sign;

    res_len = bn_normalize(res, res_len);
    if (res_len == 0) return make_int(0);
    if (res_len > BIGNUM_MAX_LIMBS) {
        printk("[Lisp Error] Bignum multiplication overflow (%d limbs > %d)\n",
               res_len, BIGNUM_MAX_LIMBS);
        return &nil_val;
    }

    int cap = 8;
    uint32_t *limbs = alloc_limbs(res_len, &cap);
    if (!limbs) return &nil_val;
    memcpy(limbs, res, res_len * sizeof(uint32_t));
    return bn_demote_if_possible(res_sign, res_len, cap, limbs);
}

static void bn_div_rem(lisp_val_t *a, lisp_val_t *b, lisp_val_t **out_q, lisp_val_t **out_r) {
    int a_sign, b_sign, a_len, b_len;
    const uint32_t *a_limbs, *b_limbs;
    uint32_t a_temp[2], b_temp[2];
    get_num_limbs(a, &a_sign, &a_len, &a_limbs, a_temp);
    get_num_limbs(b, &b_sign, &b_len, &b_limbs, b_temp);

    if (b_len == 0) {
        if (out_q) *out_q = &nil_val;
        if (out_r) *out_r = &nil_val;
        return;
    }

    uint32_t q_buf[BIGNUM_MAX_LIMBS + 4], r_buf[BIGNUM_MAX_LIMBS + 4];
    int q_len = 0, r_len = 0;

    if (b_len == 1) {
        uint32_t rem = 0;
        bn_div_single(q_buf, &q_len, &rem, a_limbs, a_len, b_limbs[0]);
        if (rem) {
            r_buf[0] = rem;
            r_len = 1;
        } else {
            r_len = 0;
        }
    } else {
        bn_div_multi(q_buf, &q_len, r_buf, &r_len, a_limbs, a_len, b_limbs, b_len);
    }

    int q_sign = a_sign * b_sign;
    int r_sign = a_sign;

    q_len = bn_normalize(q_buf, q_len);
    r_len = bn_normalize(r_buf, r_len);

    if (out_q) {
        if (q_len == 0) {
            *out_q = make_int(0);
        } else {
            int cap = 8;
            uint32_t *q_limbs = alloc_limbs(q_len, &cap);
            if (!q_limbs) { *out_q = &nil_val; }
            else {
                memcpy(q_limbs, q_buf, q_len * sizeof(uint32_t));
                *out_q = bn_demote_if_possible(q_sign, q_len, cap, q_limbs);
            }
        }
    }

    if (out_r) {
        if (r_len == 0) {
            *out_r = make_int(0);
        } else {
            int cap = 8;
            uint32_t *r_limbs = alloc_limbs(r_len, &cap);
            if (!r_limbs) { *out_r = &nil_val; }
            else {
                memcpy(r_limbs, r_buf, r_len * sizeof(uint32_t));
                *out_r = bn_demote_if_possible(r_sign, r_len, cap, r_limbs);
            }
        }
    }
}

static lisp_val_t *bn_gcd(lisp_val_t *a, lisp_val_t *b) {
    int a_sign, b_sign, a_len, b_len;
    const uint32_t *a_limbs, *b_limbs;
    uint32_t a_temp[2], b_temp[2];
    get_num_limbs(a, &a_sign, &a_len, &a_limbs, a_temp);
    get_num_limbs(b, &b_sign, &b_len, &b_limbs, b_temp);

    uint32_t res[BIGNUM_MAX_LIMBS + 4];
    int res_len = bn_gcd_abs(res, a_limbs, a_len, b_limbs, b_len);
    res_len = bn_normalize(res, res_len);
    if (res_len == 0) return make_int(0);

    int cap = 8;
    uint32_t *limbs = alloc_limbs(res_len, &cap);
    if (!limbs) return &nil_val;
    memcpy(limbs, res, res_len * sizeof(uint32_t));
    return bn_demote_if_possible(1, res_len, cap, limbs);
}

static bool num_is_zero(const lisp_val_t *v) {
    if (!v) return false;
    if (v->type == LISP_INT) return v->u.i == 0;
    if (v->type == LISP_BIGNUM) return v->u.bignum.len == 0;
    if (v->type == LISP_RATIO) return num_is_zero(v->u.ratio.num);
    return false;
}

static bool num_is_negative(const lisp_val_t *v) {
    if (!v) return false;
    if (v->type == LISP_INT) return v->u.i < 0;
    if (v->type == LISP_BIGNUM) return v->u.bignum.sign < 0;
    if (v->type == LISP_RATIO) return num_is_negative(v->u.ratio.num);
    return false;
}

static lisp_val_t *num_neg(lisp_val_t *v);

static lisp_val_t *make_ratio(lisp_val_t *num, lisp_val_t *den) {
    if (!num || !den) return &nil_val;
    if (num_is_zero(den)) {
        printk("[Lisp Error] Division by zero\n");
        return &nil_val;
    }
    if (num_is_zero(num)) {
        return make_int(0);
    }

    if (num_is_negative(den)) {
        num = num_neg(num);
        den = num_neg(den);
    }

    lisp_val_t *g = bn_gcd(num, den);
    if (g && (g->type != LISP_INT || g->u.i != 1)) {
        lisp_val_t *q_num = NULL, *q_den = NULL;
        bn_div_rem(num, g, &q_num, NULL);
        bn_div_rem(den, g, &q_den, NULL);
        if (q_num) num = q_num;
        if (q_den) den = q_den;
    }

    if (den->type == LISP_INT && den->u.i == 1) {
        return num;
    }

    lisp_val_t *v = alloc_node(LISP_RATIO);
    v->u.ratio.num = num;
    v->u.ratio.den = den;
    return v;
}

static lisp_val_t *num_neg(lisp_val_t *v) {
    if (!v) return &nil_val;
    if (v->type == LISP_INT) {
        if (v->u.i == LONG_MIN) {
            uint32_t limbs[2];
            int sign, len;
            int_to_limbs(v->u.i, &sign, &len, limbs);
            int cap = 8;
            uint32_t *nl = alloc_limbs(len, &cap);
            if (!nl) return &nil_val;
            memcpy(nl, limbs, len * sizeof(uint32_t));
            return make_bignum_raw(-sign, len, cap, nl);
        }
        return make_int(-v->u.i);
    }
    if (v->type == LISP_BIGNUM) {
        int cap = 8;
        uint32_t *nl = alloc_limbs(v->u.bignum.len, &cap);
        if (!nl) return &nil_val;
        memcpy(nl, v->u.bignum.limbs, v->u.bignum.len * sizeof(uint32_t));
        return make_bignum_raw(-v->u.bignum.sign, v->u.bignum.len, cap, nl);
    }
    if (v->type == LISP_RATIO) {
        return make_ratio(num_neg(v->u.ratio.num), v->u.ratio.den);
    }
    return v;
}

static lisp_val_t *ratio_add(lisp_val_t *a, lisp_val_t *b) {
    lisp_val_t *n1 = (a->type == LISP_RATIO) ? a->u.ratio.num : a;
    lisp_val_t *d1 = (a->type == LISP_RATIO) ? a->u.ratio.den : make_int(1);
    lisp_val_t *n2 = (b->type == LISP_RATIO) ? b->u.ratio.num : b;
    lisp_val_t *d2 = (b->type == LISP_RATIO) ? b->u.ratio.den : make_int(1);

    lisp_val_t *g = bn_gcd(d1, d2);
    if (g->type == LISP_INT && g->u.i == 1) {
        lisp_val_t *t1 = bn_mul(n1, d2);
        lisp_val_t *t2 = bn_mul(n2, d1);
        lisp_val_t *num = bn_add(t1, t2);
        lisp_val_t *den = bn_mul(d1, d2);
        return make_ratio(num, den);
    } else {
        lisp_val_t *d1_prime = NULL, *d2_prime = NULL;
        bn_div_rem(d1, g, &d1_prime, NULL);
        bn_div_rem(d2, g, &d2_prime, NULL);
        lisp_val_t *t1 = bn_mul(n1, d2_prime);
        lisp_val_t *t2 = bn_mul(n2, d1_prime);
        lisp_val_t *num = bn_add(t1, t2);
        lisp_val_t *den = bn_mul(d1_prime, d2);
        return make_ratio(num, den);
    }
}

static lisp_val_t *ratio_sub(lisp_val_t *a, lisp_val_t *b) {
    lisp_val_t *n1 = (a->type == LISP_RATIO) ? a->u.ratio.num : a;
    lisp_val_t *d1 = (a->type == LISP_RATIO) ? a->u.ratio.den : make_int(1);
    lisp_val_t *n2 = (b->type == LISP_RATIO) ? b->u.ratio.num : b;
    lisp_val_t *d2 = (b->type == LISP_RATIO) ? b->u.ratio.den : make_int(1);

    lisp_val_t *g = bn_gcd(d1, d2);
    if (g->type == LISP_INT && g->u.i == 1) {
        lisp_val_t *t1 = bn_mul(n1, d2);
        lisp_val_t *t2 = bn_mul(n2, d1);
        lisp_val_t *num = bn_sub(t1, t2);
        lisp_val_t *den = bn_mul(d1, d2);
        return make_ratio(num, den);
    } else {
        lisp_val_t *d1_prime = NULL, *d2_prime = NULL;
        bn_div_rem(d1, g, &d1_prime, NULL);
        bn_div_rem(d2, g, &d2_prime, NULL);
        lisp_val_t *t1 = bn_mul(n1, d2_prime);
        lisp_val_t *t2 = bn_mul(n2, d1_prime);
        lisp_val_t *num = bn_sub(t1, t2);
        lisp_val_t *den = bn_mul(d1_prime, d2);
        return make_ratio(num, den);
    }
}

static lisp_val_t *ratio_mul(lisp_val_t *a, lisp_val_t *b) {
    lisp_val_t *n1 = (a->type == LISP_RATIO) ? a->u.ratio.num : a;
    lisp_val_t *d1 = (a->type == LISP_RATIO) ? a->u.ratio.den : make_int(1);
    lisp_val_t *n2 = (b->type == LISP_RATIO) ? b->u.ratio.num : b;
    lisp_val_t *d2 = (b->type == LISP_RATIO) ? b->u.ratio.den : make_int(1);

    lisp_val_t *g1 = bn_gcd(n1, d2);
    lisp_val_t *g2 = bn_gcd(n2, d1);

    lisp_val_t *n1_p = n1, *d2_p = d2;
    lisp_val_t *n2_p = n2, *d1_p = d1;
    if (g1->type != LISP_INT || g1->u.i != 1) {
        bn_div_rem(n1, g1, &n1_p, NULL);
        bn_div_rem(d2, g1, &d2_p, NULL);
    }
    if (g2->type != LISP_INT || g2->u.i != 1) {
        bn_div_rem(n2, g2, &n2_p, NULL);
        bn_div_rem(d1, g2, &d1_p, NULL);
    }

    lisp_val_t *num = bn_mul(n1_p, n2_p);
    lisp_val_t *den = bn_mul(d1_p, d2_p);
    return make_ratio(num, den);
}

static lisp_val_t *ratio_div(lisp_val_t *a, lisp_val_t *b) {
    lisp_val_t *n2 = (b->type == LISP_RATIO) ? b->u.ratio.num : b;
    lisp_val_t *d2 = (b->type == LISP_RATIO) ? b->u.ratio.den : make_int(1);
    if (num_is_zero(n2)) {
        printk("[Lisp Error] Division by zero\n");
        return &nil_val;
    }
    lisp_val_t *inv_b = make_ratio(d2, n2);
    return ratio_mul(a, inv_b);
}

static int num_cmp(const lisp_val_t *a, const lisp_val_t *b) {
    if (a->type == LISP_INT && b->type == LISP_INT) {
        return a->u.i > b->u.i ? 1 : (a->u.i < b->u.i ? -1 : 0);
    }
    if (a->type == LISP_RATIO || b->type == LISP_RATIO) {
        lisp_val_t *n1 = (a->type == LISP_RATIO) ? a->u.ratio.num : (lisp_val_t *)a;
        lisp_val_t *d1 = (a->type == LISP_RATIO) ? a->u.ratio.den : make_int(1);
        lisp_val_t *n2 = (b->type == LISP_RATIO) ? b->u.ratio.num : (lisp_val_t *)b;
        lisp_val_t *d2 = (b->type == LISP_RATIO) ? b->u.ratio.den : make_int(1);
        lisp_val_t *cross1 = bn_mul(n1, d2);
        lisp_val_t *cross2 = bn_mul(n2, d1);
        return num_cmp(cross1, cross2);
    }
    int a_sign, b_sign, a_len, b_len;
    const uint32_t *a_limbs, *b_limbs;
    uint32_t a_temp[2], b_temp[2];
    get_num_limbs(a, &a_sign, &a_len, &a_limbs, a_temp);
    get_num_limbs(b, &b_sign, &b_len, &b_limbs, b_temp);
    if (a_len == 0 && b_len == 0) return 0;
    if (a_len == 0) return b_sign > 0 ? -1 : 1;
    if (b_len == 0) return a_sign > 0 ? 1 : -1;
    if (a_sign != b_sign) return a_sign > b_sign ? 1 : -1;
    int cmp_abs = bn_cmp_abs(a_limbs, a_len, b_limbs, b_len);
    return a_sign > 0 ? cmp_abs : -cmp_abs;
}

static lisp_val_t *num_add(lisp_val_t *a, lisp_val_t *b) {
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (a->type == LISP_INT && b->type == LISP_INT) {
        long res;
        if (!__builtin_add_overflow(a->u.i, b->u.i, &res)) {
            return make_int(res);
        }
    }
    if (a->type == LISP_RATIO || b->type == LISP_RATIO) {
        return ratio_add(a, b);
    }
    return bn_add(a, b);
}

static lisp_val_t *num_sub(lisp_val_t *a, lisp_val_t *b) {
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (a->type == LISP_INT && b->type == LISP_INT) {
        long res;
        if (!__builtin_sub_overflow(a->u.i, b->u.i, &res)) {
            return make_int(res);
        }
    }
    if (a->type == LISP_RATIO || b->type == LISP_RATIO) {
        return ratio_sub(a, b);
    }
    return bn_sub(a, b);
}

static lisp_val_t *num_mul(lisp_val_t *a, lisp_val_t *b) {
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (a->type == LISP_INT && b->type == LISP_INT) {
        long res;
        if (!__builtin_mul_overflow(a->u.i, b->u.i, &res)) {
            return make_int(res);
        }
    }
    if (a->type == LISP_RATIO || b->type == LISP_RATIO) {
        return ratio_mul(a, b);
    }
    return bn_mul(a, b);
}

static lisp_val_t *num_div(lisp_val_t *a, lisp_val_t *b) {
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (num_is_zero(b)) {
        return &nil_val;
    }
    if (a->type == LISP_RATIO || b->type == LISP_RATIO) {
        return ratio_div(a, b);
    }
    if (a->type == LISP_INT && b->type == LISP_INT) {
        if (b->u.i == 0) return &nil_val;
        if (a->u.i % b->u.i == 0) {
            return make_int(a->u.i / b->u.i);
        }
    }
    return make_ratio(a, b);
}

static void ratio_to_string(const lisp_val_t *v, char *out, size_t max_out) {
    if (!v || v->type != LISP_RATIO) {
        if (max_out > 0) out[0] = '\0';
        return;
    }
    char *p = out;
    char *end = out + max_out - 1;
    if (v->u.ratio.num->type == LISP_BIGNUM) {
        size_t avail = (p < end) ? (size_t)(end - p + 1) : 0;
        bn_to_string(v->u.ratio.num, p, avail);
        while (*p) p++;
    } else {
        long n = v->u.ratio.num->u.i;
        if (n < 0 && p < end) { *p++ = '-'; n = -n; }
        format_uint_digits(&p, end, (uint32_t)n, 0);
    }
    if (p < end) *p++ = '/';
    if (v->u.ratio.den->type == LISP_BIGNUM) {
        size_t avail = (p < end) ? (size_t)(end - p + 1) : 0;
        bn_to_string(v->u.ratio.den, p, avail);
        while (*p) p++;
    } else {
        long d = v->u.ratio.den->u.i;
        format_uint_digits(&p, end, (uint32_t)d, 0);
    }
    *p = '\0';
}

static bool is_delimiter(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '(' || c == ')' || c == ';' || c == '"' || c == '\'' ||
           c == '`' || c == ',';
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
    if (is_delimiter(*p) || *p < '0' || *p > '9') return false;

    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (*p == '/') {
        p++;
        if (is_delimiter(*p) || *p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    return is_delimiter(*p);
}

static lisp_val_t *parse_number_token(const char **str) {
    if ((**str == '0') && ((*str)[1] == 'x' || (*str)[1] == 'X')) {
        const char *hstart = *str + 2;
        (*str) += 2;
        uint32_t limbs[BIGNUM_MAX_LIMBS + 4];
        memset(limbs, 0, sizeof(limbs));
        int len = 0;
        while ((**str >= '0' && **str <= '9') || (**str >= 'a' && **str <= 'f') || (**str >= 'A' && **str <= 'F')) {
            char c = **str;
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);

            uint64_t carry = digit;
            for (int i = 0; i < len; i++) {
                uint64_t cur = ((uint64_t)limbs[i] << 4) | carry;
                limbs[i] = (uint32_t)cur;
                carry = cur >> 32;
            }
            if (carry) {
                if (len < (int)(sizeof(limbs) / sizeof(limbs[0]))) {
                    limbs[len++] = (uint32_t)carry;
                }
            }
            if (len == 0 && digit > 0) {
                if (len < (int)(sizeof(limbs) / sizeof(limbs[0]))) {
                    limbs[len++] = digit;
                }
            }
            (*str)++;
        }
        if (*str == hstart) return NULL;
        len = bn_normalize(limbs, len);
        if (len == 0) return make_int(0);
        int cap = 8;
        uint32_t *nl = alloc_limbs(len, &cap);
        if (!nl) return &nil_val;
        memcpy(nl, limbs, len * sizeof(uint32_t));
        return bn_demote_if_possible(1, len, cap, nl);
    }

    int sign = 1;
    if (**str == '-') {
        sign = -1;
        (*str)++;
    } else if (**str == '+') {
        (*str)++;
    }

    const char *start_num = *str;
    while (**str >= '0' && **str <= '9') (*str)++;
    int num_len = (int)(*str - start_num);
    if (num_len == 0) return NULL;

    lisp_val_t *numerator = NULL;
    if (num_len <= 9) {
        long v = 0;
        for (int i = 0; i < num_len; i++) v = v * 10 + (start_num[i] - '0');
        numerator = make_int(sign * v);
    } else {
        numerator = bn_from_string(start_num, num_len, sign);
    }

    if (**str == '/' && (*str)[1] >= '0' && (*str)[1] <= '9') {
        (*str)++; // skip '/'
        const char *start_den = *str;
        while (**str >= '0' && **str <= '9') (*str)++;
        int den_len = (int)(*str - start_den);
        if (den_len == 0) return NULL;
        lisp_val_t *denominator = NULL;
        if (den_len <= 9) {
            long v = 0;
            for (int i = 0; i < den_len; i++) v = v * 10 + (start_den[i] - '0');
            denominator = make_int(v);
        } else {
            denominator = bn_from_string(start_den, den_len, 1);
        }
        return make_ratio(numerator, denominator);
    }

    return numerator;
}

static bool lisp_values_equal(lisp_val_t *a, lisp_val_t *b) {
    if (!a || !b) return false;
    if (is_number_val(a) && is_number_val(b)) {
        return num_cmp(a, b) == 0;
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case LISP_STRING:
            return strcmp(a->u.str, b->u.str) == 0;
        case LISP_SYMBOL:
            return streq(a->u.sym, b->u.sym);
        default:
            return false;
    }
}

static bool lisp_equal_p(lisp_val_t *a, lisp_val_t *b) {
    while (a && b && a->type == LISP_PAIR && b->type == LISP_PAIR) {
        if (a == b) return true;
        if (!lisp_equal_p(a->u.pair.car, b->u.pair.car)) return false;
        a = a->u.pair.cdr;
        b = b->u.pair.cdr;
    }
    if (a == b) return true;
    if (!a || !b) return false;
    if (is_number_val(a) && is_number_val(b)) {
        return num_cmp(a, b) == 0;
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case LISP_NIL:
            return true;
        case LISP_STRING:
            return strcmp(a->u.str, b->u.str) == 0;
        case LISP_SYMBOL:
            return a == b || streq(a->u.sym, b->u.sym);
        default:
            return a == b;
    }
}

static lisp_val_t *prim_equal_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    if (!a || !b) return &false_val;
    return lisp_equal_p(a, b) ? &true_val : &false_val;
}

static lisp_val_t *prim_eq_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    if (!a || !b) return &false_val;
    if (a == b) return &true_val;
    if (a->type != b->type) return &false_val;
    if (a->type == LISP_INT) return (a->u.i == b->u.i) ? &true_val : &false_val;
    if (a->type == LISP_SYMBOL) return streq(a->u.sym, b->u.sym) ? &true_val : &false_val;
    return &false_val;
}

static lisp_val_t *prim_eq(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    lisp_val_t *first = args->u.pair.car;
    lisp_val_t *c = args->u.pair.cdr;
    if (!c || c->type != LISP_PAIR) return &false_val;
    for (; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        if (!lisp_values_equal(first, c->u.pair.car)) return &false_val;
    }
    return &true_val;
}

static lisp_val_t *prim_chain_compare(lisp_val_t *args, int (*op)(int cmp)) {
    if (!args || args->type != LISP_PAIR) return &false_val;
    lisp_val_t *prev = args->u.pair.car;
    if (!is_number_val(prev)) return &false_val;
    lisp_val_t *c = args->u.pair.cdr;
    if (!c || c->type != LISP_PAIR) return &false_val;
    for (; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        lisp_val_t *cur = c->u.pair.car;
        if (!is_number_val(cur)) return &false_val;
        int cmp = num_cmp(prev, cur);
        if (!op(cmp)) return &false_val;
        prev = cur;
    }
    return &true_val;
}

static int cmp_op_lt(int cmp) { return cmp < 0; }
static int cmp_op_gt(int cmp) { return cmp > 0; }
static int cmp_op_le(int cmp) { return cmp <= 0; }
static int cmp_op_ge(int cmp) { return cmp >= 0; }

static lisp_val_t *prim_lt(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_op_lt); }
static lisp_val_t *prim_gt(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_op_gt); }
static lisp_val_t *prim_le(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_op_le); }
static lisp_val_t *prim_ge(lisp_val_t *args, lisp_val_t *env) { (void)env; return prim_chain_compare(args, cmp_op_ge); }

static lisp_val_t *prim_ne(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int n = lisp_list_len(args);
    if (n < 2) return &false_val;
    for (int i = 0; i < n; i++) {
        lisp_val_t *a = lisp_list_ref(args, i);
        if (!is_number_val(a)) return &false_val;
        for (int j = i + 1; j < n; j++) {
            lisp_val_t *b = lisp_list_ref(args, j);
            if (!is_number_val(b) || num_cmp(a, b) == 0) return &false_val;
        }
    }
    return &true_val;
}

static lisp_val_t *prim_add(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *sum = make_int(0);
    int guard = 0;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (!is_number_val(c->u.pair.car)) {
            return &nil_val;
        }
        sum = num_add(sum, c->u.pair.car);
        if (sum == &nil_val) return &nil_val;
    }
    return sum;
}

static lisp_val_t *prim_sub(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    lisp_val_t *first = args->u.pair.car;
    if (!is_number_val(first)) return &nil_val;
    lisp_val_t *c = args->u.pair.cdr;
    if (!c || c->type != LISP_PAIR) {
        return num_neg(first);
    }
    lisp_val_t *res = first;
    int guard = 0;
    for (; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
        if (!is_number_val(c->u.pair.car)) return &nil_val;
        res = num_sub(res, c->u.pair.car);
        if (res == &nil_val) return &nil_val;
    }
    return res;
}

static lisp_val_t *prim_mul(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *prod = make_int(1);
    int guard = 0;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (!is_number_val(c->u.pair.car)) {
            return &nil_val;
        }
        prod = num_mul(prod, c->u.pair.car);
        if (prod == &nil_val) return &nil_val;
    }
    return prod;
}

static lisp_val_t *prim_div(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    lisp_val_t *first = args->u.pair.car;
    if (!is_number_val(first)) return &nil_val;
    lisp_val_t *c = args->u.pair.cdr;
    if (!c || c->type != LISP_PAIR) {
        return num_div(make_int(1), first);
    }
    lisp_val_t *res = first;
    int guard = 0;
    for (; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE; c = c->u.pair.cdr, guard++) {
        if (!is_number_val(c->u.pair.car)) return &nil_val;
        res = num_div(res, c->u.pair.car);
        if (res == &nil_val) return &nil_val;
    }
    return res;
}

static lisp_val_t *prim_quotient(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (num_is_zero(b)) return &nil_val;
    lisp_val_t *q = NULL;
    bn_div_rem(a, b, &q, NULL);
    return q ? q : &nil_val;
}

static lisp_val_t *prim_remainder(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (num_is_zero(b)) return &nil_val;
    lisp_val_t *r = NULL;
    bn_div_rem(a, b, NULL, &r);
    return r ? r : &nil_val;
}

static lisp_val_t *prim_modulo(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    lisp_val_t *b = lisp_list_ref(args, 1);
    if (!is_number_val(a) || !is_number_val(b)) return &nil_val;
    if (num_is_zero(b)) return &nil_val;
    lisp_val_t *r = NULL;
    bn_div_rem(a, b, NULL, &r);
    if (!r) return &nil_val;
    if (!num_is_zero(r) && (num_is_negative(r) != num_is_negative(b))) {
        r = num_add(r, b);
    }
    return r;
}

static lisp_val_t *prim_abs(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!is_number_val(a)) return make_int(0);
    return num_is_negative(a) ? num_neg(a) : a;
}

static lisp_val_t *prim_min(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    lisp_val_t *best = args->u.pair.car;
    if (!is_number_val(best)) return make_int(0);
    int guard = 0;
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (is_number_val(c->u.pair.car) && num_cmp(c->u.pair.car, best) < 0) {
            best = c->u.pair.car;
        }
    }
    return best;
}

static lisp_val_t *prim_max(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    lisp_val_t *best = args->u.pair.car;
    if (!is_number_val(best)) return make_int(0);
    int guard = 0;
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR && guard < NODE_POOL_SIZE;
         c = c->u.pair.cdr, guard++) {
        if (is_number_val(c->u.pair.car) && num_cmp(c->u.pair.car, best) > 0) {
            best = c->u.pair.car;
        }
    }
    return best;
}

/* --- Predicates & Numeric Introspection --- */

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
    return (a && (a->type == LISP_INT || a->type == LISP_BIGNUM)) ? &true_val : &false_val;
}

static lisp_val_t *prim_rational_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return is_number_val(a) ? &true_val : &false_val;
}

static lisp_val_t *prim_exact_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return is_number_val(a) ? &true_val : &false_val;
}

static lisp_val_t *prim_bignum_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_BIGNUM) ? &true_val : &false_val;
}

static lisp_val_t *prim_ratio_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && a->type == LISP_RATIO) ? &true_val : &false_val;
}

static lisp_val_t *prim_number_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return is_number_val(a) ? &true_val : &false_val;
}

static lisp_val_t *prim_procedure_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && (a->type == LISP_PRIMITIVE || a->type == LISP_LAMBDA)) ? &true_val : &false_val;
}

static lisp_val_t *prim_zero_p(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    return (a && num_is_zero(a)) ? &true_val : &false_val;
}

static lisp_val_t *prim_numerator(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a) return &nil_val;
    if (a->type == LISP_RATIO) return a->u.ratio.num;
    if (a->type == LISP_INT || a->type == LISP_BIGNUM) return a;
    return &nil_val;
}

static lisp_val_t *prim_denominator(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a) return &nil_val;
    if (a->type == LISP_RATIO) return a->u.ratio.den;
    if (a->type == LISP_INT || a->type == LISP_BIGNUM) return make_int(1);
    return &nil_val;
}

static lisp_val_t *prim_gcd(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    lisp_val_t *res = args->u.pair.car;
    if (!is_number_val(res)) return make_int(0);
    if (num_is_negative(res)) res = num_neg(res);
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        if (is_number_val(c->u.pair.car)) {
            res = bn_gcd(res, c->u.pair.car);
        }
    }
    return res;
}

static lisp_val_t *prim_lcm(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(1);
    lisp_val_t *res = args->u.pair.car;
    if (!is_number_val(res)) return make_int(1);
    if (num_is_negative(res)) res = num_neg(res);
    for (lisp_val_t *c = args->u.pair.cdr; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        lisp_val_t *arg = c->u.pair.car;
        if (is_number_val(arg)) {
            if (num_is_zero(res) || num_is_zero(arg)) return make_int(0);
            lisp_val_t *g = bn_gcd(res, arg);
            lisp_val_t *q = NULL;
            bn_div_rem(res, g, &q, NULL);
            res = bn_mul(q, arg);
            if (num_is_negative(res)) res = num_neg(res);
        }
    }
    return res;
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

static inline lisp_val_t *val_car(lisp_val_t *v) {
    return (v && v->type == LISP_PAIR) ? v->u.pair.car : &nil_val;
}

static inline lisp_val_t *val_cdr(lisp_val_t *v) {
    return (v && v->type == LISP_PAIR) ? v->u.pair.cdr : &nil_val;
}

static lisp_val_t *prim_caar(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_car(lisp_list_ref(args, 0))); }
static lisp_val_t *prim_cadr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_cdr(lisp_list_ref(args, 0))); }
static lisp_val_t *prim_cdar(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_cdr(val_car(lisp_list_ref(args, 0))); }
static lisp_val_t *prim_cddr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_cdr(val_cdr(lisp_list_ref(args, 0))); }

static lisp_val_t *prim_caaar(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_car(val_car(lisp_list_ref(args, 0)))); }
static lisp_val_t *prim_caadr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_car(val_cdr(lisp_list_ref(args, 0)))); }
static lisp_val_t *prim_cadar(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_cdr(val_car(lisp_list_ref(args, 0)))); }
static lisp_val_t *prim_caddr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_cdr(val_cdr(lisp_list_ref(args, 0)))); }

static lisp_val_t *prim_cdaar(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_cdr(val_car(val_car(lisp_list_ref(args, 0)))); }
static lisp_val_t *prim_cdadr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_cdr(val_car(val_cdr(lisp_list_ref(args, 0)))); }
static lisp_val_t *prim_cddar(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_cdr(val_cdr(val_car(lisp_list_ref(args, 0)))); }
static lisp_val_t *prim_cdddr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_cdr(val_cdr(val_cdr(lisp_list_ref(args, 0)))); }

static lisp_val_t *prim_cadddr(lisp_val_t *args, lisp_val_t *env) { (void)env; return val_car(val_cdr(val_cdr(val_cdr(lisp_list_ref(args, 0))))); }

static lisp_val_t *lisp_assoc(lisp_val_t *key, lisp_val_t *alist) {
    if (!key || !alist) return NULL;
    for (lisp_val_t *c = alist; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        lisp_val_t *item = c->u.pair.car;
        if (item && item->type == LISP_PAIR) {
            if (lisp_equal_p(key, item->u.pair.car)) {
                return item;
            }
        }
    }
    return NULL;
}

static lisp_val_t *prim_assoc(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *key = lisp_list_ref(args, 0);
    lisp_val_t *alist = lisp_list_ref(args, 1);
    lisp_val_t *res = lisp_assoc(key, alist);
    return res ? res : &false_val;
}

static lisp_val_t *prim_assq(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *key = lisp_list_ref(args, 0);
    lisp_val_t *alist = lisp_list_ref(args, 1);
    if (!key || !alist) return &false_val;
    for (lisp_val_t *c = alist; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        lisp_val_t *item = c->u.pair.car;
        if (item && item->type == LISP_PAIR) {
            lisp_val_t *k = item->u.pair.car;
            if (k == key || (k && key && k->type == key->type &&
                ((k->type == LISP_INT && k->u.i == key->u.i) ||
                 (k->type == LISP_SYMBOL && streq(k->u.sym, key->u.sym))))) {
                return item;
            }
        }
    }
    return &false_val;
}

static lisp_val_t *prim_member(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *key = lisp_list_ref(args, 0);
    lisp_val_t *lst = lisp_list_ref(args, 1);
    if (!key || !lst) return &false_val;
    for (lisp_val_t *c = lst; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        if (lisp_equal_p(key, c->u.pair.car)) {
            return c;
        }
    }
    return &false_val;
}

static lisp_val_t *prim_memq(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *key = lisp_list_ref(args, 0);
    lisp_val_t *lst = lisp_list_ref(args, 1);
    if (!key || !lst) return &false_val;
    for (lisp_val_t *c = lst; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        lisp_val_t *k = c->u.pair.car;
        if (k == key || (k && key && k->type == key->type &&
            ((k->type == LISP_INT && k->u.i == key->u.i) ||
             (k->type == LISP_SYMBOL && streq(k->u.sym, key->u.sym))))) {
            return c;
        }
    }
    return &false_val;
}

static lisp_val_t *lisp_subst(lisp_val_t *new_val, lisp_val_t *old_val, lisp_val_t *tree) {
    if (!tree) return &nil_val;
    if (lisp_equal_p(tree, old_val)) {
        return new_val;
    }
    if (tree->type == LISP_PAIR) {
        lisp_val_t *new_car = lisp_subst(new_val, old_val, tree->u.pair.car);
        lisp_val_t *new_cdr = lisp_subst(new_val, old_val, tree->u.pair.cdr);
        if (new_car == tree->u.pair.car && new_cdr == tree->u.pair.cdr) {
            return tree;
        }
        return make_pair(new_car, new_cdr);
    }
    return tree;
}

static lisp_val_t *prim_subst(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *new_val = lisp_list_ref(args, 0);
    lisp_val_t *old_val = lisp_list_ref(args, 1);
    lisp_val_t *tree = lisp_list_ref(args, 2);
    if (!new_val || !old_val || !tree) return &nil_val;
    return lisp_subst(new_val, old_val, tree);
}

static bool is_pattern_var(const lisp_val_t *v) {
    if (!v || v->type != LISP_SYMBOL) return false;
    const char *s = v->u.sym;
    return (s[0] == '?' && s[1] != '\0');
}

static bool is_anonymous_wildcard(const lisp_val_t *v) {
    if (!v || v->type != LISP_SYMBOL) return false;
    const char *s = v->u.sym;
    return (strcmp(s, "?") == 0 || strcmp(s, "_") == 0);
}

static lisp_val_t *lisp_match(lisp_val_t *pattern, lisp_val_t *expr, lisp_val_t *bindings) {
    if (is_anonymous_wildcard(pattern)) {
        return bindings;
    }
    if (is_pattern_var(pattern)) {
        lisp_val_t *entry = lisp_assoc(pattern, bindings);
        if (entry) {
            if (lisp_equal_p(entry->u.pair.cdr, expr)) {
                return bindings;
            }
            return NULL;
        }
        return make_pair(make_pair(pattern, expr), bindings);
    }
    if (pattern && pattern->type == LISP_PAIR) {
        if (!expr || expr->type != LISP_PAIR) return NULL;
        bindings = lisp_match(pattern->u.pair.car, expr->u.pair.car, bindings);
        if (!bindings) return NULL;
        return lisp_match(pattern->u.pair.cdr, expr->u.pair.cdr, bindings);
    }
    if (lisp_equal_p(pattern, expr)) {
        return bindings;
    }
    return NULL;
}

static lisp_val_t *prim_match(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *pattern = lisp_list_ref(args, 0);
    lisp_val_t *expr = lisp_list_ref(args, 1);
    if (!pattern || !expr) return &false_val;
    lisp_val_t *bindings = lisp_list_ref(args, 2);
    if (!bindings || bindings->type == LISP_NIL) {
        bindings = &nil_val;
    }
    lisp_val_t *res = lisp_match(pattern, expr, bindings);
    return res ? res : &false_val;
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

/* 37.2, plan/phase37_screen_layouts_and_apps.md §3.3: strings are UTF-8, and
 * the functions that count or index *characters* count code points: a
 * character starts at every byte that is not a continuation byte
 * (10xxxxxx). A stray continuation byte therefore belongs to the character
 * before it, and one at the very start is a character of its own -- count
 * and index agree, and no byte is ever lost or skipped. string-bytes is the
 * byte count, for file and protocol work. */
static long utf8_count(const char *s, long bytes) {
    long n = 0;
    for (long i = 0; i < bytes; i++)
        if (((unsigned char)s[i] & 0xc0u) != 0x80u || i == 0) n++;
    return n;
}

/* The byte offset of character `k` (clamped to the end). */
static long utf8_offset(const char *s, long bytes, long k) {
    long i = 0;
    while (k > 0 && i < bytes) {
        i++;
        while (i < bytes && ((unsigned char)s[i] & 0xc0u) == 0x80u) i++;
        k--;
    }
    return i;
}

static lisp_val_t *prim_string_length(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type != LISP_STRING) return make_int(0);
    return make_int(utf8_count(a->u.str, (long)strlen(a->u.str)));
}

/* (gc-stats) -> (collections-inside-a-form free-nodes longest-pause-us
 * collections total-ms): 37.3a's collector, made visible -- how often a form ran a
 * pool dry and was rescued, and how much of the node pool is free right now;
 * since 38.5 also the longest collection so far in microseconds, how many
 * collections there were in all, and the milliseconds spent in them. New
 * figures go on the end. */
static lisp_val_t *prim_gc_stats(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    long free_nodes = (long)(NODE_POOL_SIZE - node_pool_idx) + (long)gc_free_nodes();
    lisp_val_t *tail = make_pair(make_int((long)(gc_total_ms_x1000 / 1000u)), &nil_val);
    tail = make_pair(make_int((long)gc_collections), tail);
    tail = make_pair(make_int((long)gc_max_pause_us), tail);
    tail = make_pair(make_int(free_nodes), tail);
    return make_pair(make_int((long)gc_in_form_count), tail);
}

/* 37.5a: (clipboard) -> the clipboard's text, "" when empty;
 * (clipboard-set s) -> #t, or #f past its 8 KB. The same clipboard every
 * text input cuts to and /dev/clipboard is. A string here is bounded by
 * STRING_SLOT_LEN, so (clipboard) returns at most that much; the file has
 * all of it. */
static lisp_val_t *prim_clipboard(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    uint32_t n;
    const char *d = clipboard_data(&n);
    char buf[STRING_SLOT_LEN];
    if (n > STRING_SLOT_LEN - 1u) {
        n = STRING_SLOT_LEN - 1u;
        while (n > 0 && ((unsigned char)d[n] & 0xc0u) == 0x80u) n--;
    }
    if (n) memcpy(buf, d, n);
    buf[n] = '\0';
    return make_str(buf);
}

static lisp_val_t *prim_clipboard_set(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *s = get_str_val(lisp_list_ref(args, 0));
    return clipboard_set(s, (uint32_t)strlen(s)) ? &true_val : &false_val;
}

/* (screenshot [path]) -> the file written, or #f: 37.5a's PBM of the
 * screen (kernel/screenshot.h), to /sd0/screenshots/ by default. */
static lisp_val_t *prim_screenshot(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    const char *path = (a && a->type == LISP_STRING) ? a->u.str : NULL;
    char saved[48];
    if (screenshot_save(path, saved, sizeof(saved)) != 0) return &false_val;
    return make_str(saved);
}

static lisp_val_t *prim_string_bytes(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type != LISP_STRING) return make_int(0);
    return make_int((long)strlen(a->u.str));
}

/* (substring str start end) -- in characters; end defaults to the string's
 * own length. */
static lisp_val_t *prim_substring(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a || a->type != LISP_STRING) return make_str("");
    const char *s = a->u.str;
    long bytes = (long)strlen(s);
    long len = utf8_count(s, bytes);
    long start = arg_int(args, 1, 0);
    long end = arg_int(args, 2, len);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start > end) start = end;
    long b0 = utf8_offset(s, bytes, start);
    long b1 = utf8_offset(s, bytes, end);
    long n = b1 - b0;
    char buf[STRING_SLOT_LEN];
    if (n >= (long)sizeof(buf)) {
        n = sizeof(buf) - 1;
        while (n > 0 && ((unsigned char)s[b0 + n] & 0xc0u) == 0x80u) n--;   /* not mid-character */
    }
    memcpy(buf, s + b0, (size_t)n);
    buf[n] = '\0';
    return make_str(buf);
}

static lisp_val_t *prim_string_to_number(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *s = get_str_val(lisp_list_ref(args, 0));
    if (!s || !*s) return &false_val;
    if (!is_number_token(s)) return &false_val;
    const char *p = s;
    lisp_val_t *num = parse_number_token(&p);
    if (!num || *p != '\0') return &false_val;
    return num;
}

static lisp_val_t *prim_number_to_string(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *a = lisp_list_ref(args, 0);
    if (!a) return make_str("");
    if (a->type == LISP_INT) {
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
    if (a->type == LISP_BIGNUM) {
        char buf[700];
        bn_to_string(a, buf, sizeof(buf));
        return make_str(buf);
    }
    if (a->type == LISP_RATIO) {
        char buf[1400];
        ratio_to_string(a, buf, sizeof(buf));
        return make_str(buf);
    }
    return make_str("");
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

/* 37.3b's hooks have nothing to do on the ST7735: it has no layouts. */
void lisp_canvas_poll(void) { }
void lisp_canvas_reset(void) { }
#else
/* 37.3b, plan/phase37_screen_layouts_and_apps.md: the canvas on every build
 * without the ST7735 -- the RP2350-LCD-7's panel, or a screen in RAM on the
 * others (drivers/ramscreen.c), which is what makes these testable on QEMU.
 * Each call is one request in drivers/screen.h's protocol, sent through
 * console_canvas(); coordinates are the canvas tile's own, and everything
 * clips to it. Colours: 0 white, 1 black (the default), 2 grey; any other
 * value is black, so a program written for the ST7735's RGB565 still draws.
 *
 * Damage (§2.2): every reply carries the canvas's damage count. Drawing
 * calls remember it, canvas-window does not -- so after a layout change,
 * or a `lcd repaint`, the next prompt sees a count the program has not seen
 * and calls its (canvas-on-redraw f). */
static uint16_t g_canvas_seen;      /* the damage the program has drawn after */

static void put16(uint8_t *b, long v) {
    b[0] = (uint8_t)((unsigned long)v & 0xffu);
    b[1] = (uint8_t)(((unsigned long)v >> 8) & 0xffu);
}

static uint16_t reply16(const uint8_t *r, unsigned at) {
    return (uint16_t)(r[at] | (r[at + 1u] << 8));
}

/* One request; #t if it was done, #f if refused or there is no canvas. */
static bool canvas_call(const uint8_t *req, uint32_t n, uint8_t *reply, bool drawn) {
    if (!console_canvas(req, n, reply)) return false;
    if (drawn) g_canvas_seen = reply16(reply, 6);
    return reply[0] == 0;
}

/* An op with `count` int16 arguments from args, defaulting the colour (the
 * last) to black. */
static lisp_val_t *canvas_op(char op, lisp_val_t *args, int count, bool colour) {
    uint8_t req[1 + 2 * 6], reply[SCREEN_REPLY_LEN];
    req[0] = (uint8_t)op;
    for (int i = 0; i < count; i++) {
        long dflt = (colour && i == count - 1) ? 1 : 0;
        put16(req + 1 + 2 * i, arg_int(args, i, dflt));
    }
    return canvas_call(req, 1u + 2u * (uint32_t)count, reply, true) ? &true_val : &false_val;
}

/* (canvas-fill [c]) -- the whole canvas, white by default. */
static lisp_val_t *prim_canvas_fill(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t req[3], reply[SCREEN_REPLY_LEN];
    req[0] = 'F';
    put16(req + 1, arg_int(args, 0, 0));
    return canvas_call(req, sizeof(req), reply, true) ? &true_val : &false_val;
}

/* (canvas-pixel x y [c]) */
static lisp_val_t *prim_canvas_pixel(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return canvas_op('p', args, 3, true);
}

/* (canvas-rect x y w h [c]) -- filled, as on the ST7735 */
static lisp_val_t *prim_canvas_rect(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return canvas_op('r', args, 5, true);
}

/* (canvas-frame x y w h [c]) -- the outline */
static lisp_val_t *prim_canvas_frame(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return canvas_op('o', args, 5, true);
}

/* (canvas-line x0 y0 x1 y1 [c]) */
static lisp_val_t *prim_canvas_line(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return canvas_op('l', args, 5, true);
}

/* (canvas-circle x y r [c]) */
static lisp_val_t *prim_canvas_circle(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return canvas_op('c', args, 4, true);
}

/* (canvas-invert x y w h) */
static lisp_val_t *prim_canvas_invert(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    return canvas_op('i', args, 4, false);
}

/* (canvas-text x y str [c] [size]) -- black on the canvas, the glyph's
 * top-left at x y; c and size are the ST7735's and are accepted and
 * ignored, except that a size above 1 draws bold. */
static lisp_val_t *prim_canvas_text(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t req[6 + STRING_SLOT_LEN], reply[SCREEN_REPLY_LEN];
    const char *str = get_str_val(lisp_list_ref(args, 2));
    uint32_t len = (uint32_t)strlen(str);
    if (len > STRING_SLOT_LEN) len = STRING_SLOT_LEN;
    req[0] = 't';
    put16(req + 1, arg_int(args, 0, 0));
    put16(req + 3, arg_int(args, 1, 0));
    req[5] = arg_int(args, 4, 1) > 1 ? 1 : 0;
    memcpy(req + 6, str, len);
    return canvas_call(req, 6u + len, reply, true) ? &true_val : &false_val;
}

/* (canvas-row x y cells [scale]) -- one row of pixels from a list, each
 * element 0 (white) or anything else (black), each `scale` x `scale`
 * pixels: one request per row, the shape of a cellular automaton's
 * generation or an image's scan line. */
#define CANVAS_ROW_MAX 1600
static lisp_val_t *prim_canvas_row(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t req[9 + CANVAS_ROW_MAX / 8], reply[SCREEN_REPLY_LEN];
    for (unsigned i = 9; i < sizeof(req); i++) req[i] = 0;
    long n = 0;
    for (lisp_val_t *c = lisp_list_ref(args, 2); c && c->type == LISP_PAIR && n < CANVAS_ROW_MAX;
         c = c->u.pair.cdr, n++) {
        lisp_val_t *v = c->u.pair.car;
        if (v && !(v->type == LISP_INT && v->u.i == 0)) req[9 + n / 8] |= (uint8_t)(1u << (n % 8));
    }
    req[0] = 'b';
    put16(req + 1, arg_int(args, 0, 0));
    put16(req + 3, arg_int(args, 1, 0));
    put16(req + 5, n);
    put16(req + 7, arg_int(args, 3, 1));
    return canvas_call(req, 9u + (uint32_t)(n + 7) / 8u, reply, true) ? &true_val : &false_val;
}

/* (canvas-get x y) -> 0 or 1 */
static lisp_val_t *prim_canvas_get(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t req[5], reply[SCREEN_REPLY_LEN];
    req[0] = 'g';
    put16(req + 1, arg_int(args, 0, 0));
    put16(req + 3, arg_int(args, 1, 0));
    if (!canvas_call(req, sizeof(req), reply, false)) return make_int(0);
    return make_int(reply[1]);
}

/* (canvas-size) -> (w h), (0 0) while there is no canvas tile */
static lisp_val_t *prim_canvas_size(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    uint8_t req[1] = { 'S' }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 1, reply)) return make_pair(make_int(0), make_pair(make_int(0), &nil_val));
    lisp_val_t *h = make_pair(make_int(reply16(reply, 4)), &nil_val);
    return make_pair(make_int(reply16(reply, 2)), h);
}

/* (canvas-window 'text | 'canvas | 'split | 'split-half | 'split-narrow)
 * -> #t, or #f if refused. The splits by their text tile: 'split 38 columns
 * (the widest canvas), 'split-half 48, 'split-narrow 64 (§1.2, 37.5a);
 * Super+[ and Super+] step between them on the panel. */
static lisp_val_t *prim_canvas_window(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *w = get_str_val(lisp_list_ref(args, 0));
    uint8_t req[2] = { 'L', 0xff }, reply[SCREEN_REPLY_LEN];
    if (streq(w, "text")) req[1] = SCREEN_LAYOUT_TEXT;
    else if (streq(w, "canvas")) req[1] = SCREEN_LAYOUT_CANVAS;
    else if (streq(w, "split") || streq(w, "split-wide")) req[1] = SCREEN_LAYOUT_SPLIT_WIDE;
    else if (streq(w, "split-half")) req[1] = SCREEN_LAYOUT_SPLIT_HALF;
    else if (streq(w, "split-narrow")) req[1] = SCREEN_LAYOUT_SPLIT_NARROW;   /* 37.5a */
    return canvas_call(req, sizeof(req), reply, false) ? &true_val : &false_val;
}

/* (canvas-swap) -- 37.5a: canvas and text change sides in a split, or with
 * one pane full the other one is shown full (Super+\\ on the panel); #f
 * while a program has locked the layout. */
static lisp_val_t *prim_canvas_swap(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    uint8_t req[1] = { 'X' }, reply[SCREEN_REPLY_LEN];
    return canvas_call(req, 1, reply, false) ? &true_val : &false_val;
}

/* (canvas-title str) -- the canvas tile's title bar */
static lisp_val_t *prim_canvas_title(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t req[1 + SCREEN_CTITLE_MAX], reply[SCREEN_REPLY_LEN];
    const char *t = get_str_val(lisp_list_ref(args, 0));
    uint32_t len = (uint32_t)strlen(t);
    if (len > SCREEN_CTITLE_MAX - 1u) len = SCREEN_CTITLE_MAX - 1u;
    req[0] = 'T';
    memcpy(req + 1, t, len);
    return canvas_call(req, 1u + len, reply, false) ? &true_val : &false_val;
}

/* (canvas-on-redraw f) -- f, a procedure of no arguments, is called at the
 * next prompt after the canvas was lost (a layout change, `lcd repaint`);
 * (canvas-on-redraw '()) forgets it. Kept in the global environment, so the
 * collector sees it. */
#define CANVAS_REDRAW_SYM "*canvas-redraw*"
static lisp_val_t *prim_canvas_on_redraw(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *f = lisp_list_ref(args, 0);
    /* In place when it is already bound: a program that registers on every
     * run (and the demos do, redraws included) must not grow global_env,
     * whose shadowed bindings the collector could never reclaim. */
    lisp_val_t *binding = NULL;
    for (lisp_val_t *e = global_env; e && e->type == LISP_PAIR; e = e->u.pair.cdr) {
        lisp_val_t *b = e->u.pair.car;
        if (b && b->type == LISP_PAIR && b->u.pair.car && b->u.pair.car->type == LISP_SYMBOL &&
            streq(b->u.pair.car->u.sym, CANVAS_REDRAW_SYM)) {
            binding = b;
            break;
        }
    }
    if (binding) binding->u.pair.cdr = f ? f : &nil_val;
    else env_set(&global_env, CANVAS_REDRAW_SYM, f ? f : &nil_val);
    uint8_t req[1] = { 'S' }, reply[SCREEN_REPLY_LEN];
    if (console_canvas(req, 1, reply)) g_canvas_seen = reply16(reply, 6);
    return &true_val;
}

/* 38.8: set while the redraw function runs, between 'D' 1 and 'D' 0, so
 * that its drawing does not make the other layouts' stored canvases stale.
 * A redraw cut short (Ctrl-C, an error) is closed at the next prompt. */
static bool g_canvas_redrawing;

void lisp_canvas_poll(void) {
    uint8_t reply[SCREEN_REPLY_LEN];
    if (g_canvas_redrawing) {
        uint8_t end[2] = { 'D', 0 };
        (void)console_canvas(end, 2, reply);
        g_canvas_redrawing = false;
    }
    lisp_val_t *f = env_get(global_env, CANVAS_REDRAW_SYM);
    if (!f || (f->type != LISP_LAMBDA && f->type != LISP_PRIMITIVE)) return;
    uint8_t req[1] = { 'S' };
    if (!console_canvas(req, 1, reply) || reply[8] == SCREEN_LAYOUT_TEXT) return;
    if (reply16(reply, 6) == g_canvas_seen) return;
    g_canvas_seen = reply16(reply, 6);
    uint8_t begin[2] = { 'D', 1 }, end[2] = { 'D', 0 };
    g_canvas_redrawing = console_canvas(begin, 2, reply);
    (void)lisp_eval(make_pair(f, &nil_val), global_env);
    if (g_canvas_redrawing) {
        (void)console_canvas(end, 2, reply);
        g_canvas_redrawing = false;
    }
}

void lisp_canvas_reset(void) {
    uint8_t req[2] = { 'L', SCREEN_LAYOUT_TEXT }, reply[SCREEN_REPLY_LEN];
    uint8_t ask[1] = { 'S' }, forget[1] = { 'Z' };
    if (console_canvas(ask, 1, reply) && reply[8] != SCREEN_LAYOUT_TEXT) (void)console_canvas(req, 2, reply);
    /* 38.8: after the switch, which stores the canvas it leaves -- the
     * program that drew it has ended, and the next must start blank. */
    (void)console_canvas(forget, 1, reply);
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
/* SAN/PGN notation self-test (14b). Separate from chess-selftest, which
 * exercises the *search*: this exercises notation, has no time budget, and is
 * the regression guard for a class of bug that is silent by nature -- a
 * mis-disambiguated SAN string is still a legal-looking move. */
static lisp_val_t *prim_chess_san_selftest(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return pgn_selftest() == 0 ? &true_val : &false_val;
}

/* `(chess-selftest [cores])` -- `cores` added by X8b
 * (plan/phase23_multicore_scheduling.md), defaulting to 1, which is exactly
 * the pre-X8b path. The command reports cores requested, harts online and
 * helper nodes alongside the usual result, so a run that silently fell back
 * to one core cannot be mistaken for a two-core one. */
/* (chess-board-selftest) -- 37.4: the canvas chess board, drawn from a
 * known position and checked at its pixels (user/chess/src/chess_ui.c). */
static lisp_val_t *prim_chess_board_selftest(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return chess_canvas_selftest() == 0 ? &true_val : &false_val;
}

static lisp_val_t *prim_chess_selftest(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    /* Optional third-of-nothing: `(chess-selftest cores [tt_kb])`. The table
     * size is exposed here only because X8b needed to test its own
     * explanation for why two cores bought nothing -- if the TT is the
     * bound, enlarging it must change the answer, and asserting that without
     * measuring it would be the kind of claim this project does not make. */
    int kb    = (int)arg_int(args, 1, 0);
    int depth = (int)arg_int(args, 2, 0);
    if (kb > 0) tt_embedded_bytes = (uint32_t)kb * 1024u;
    chess_selftest_bench((int)arg_int(args, 0, 1), depth);
    if (kb > 0) tt_embedded_bytes = tt_default_bytes;
    return &true_val;
}

/* `(perft [n] [cores])` (J4, plan/phase10_chess_completion.md; `cores` added
 * by X8, plan/phase23_multicore_scheduling.md) -- move-generation correctness
 * suite, `n` defaulting to 0 (chess_perft()'s own "<=0 means the documented
 * default depth" convention, matching upstream's own `run_perft_tests()`
 * no-argument wrapper) when omitted.
 *
 * `cores` defaults to 1, which is exactly the pre-X8 behaviour. Asking for
 * more than are online is not an error: run_perft_cores() clamps and reports
 * what it actually used, because the interesting comparison is one run
 * against another on the same board rather than a refusal. */
static lisp_val_t *prim_perft(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int max_depth = (int)arg_int(args, 0, 0);
    int cores     = (int)arg_int(args, 1, 1);
    chess_perft_cores(max_depth, cores);
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
    console_set_title("Chess");
    chess_console_run(); /* returns on 'quit' */
    lisp_canvas_reset();    /* 37.3b */
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
    (void)env;
    /* `(chess [cores])`, X8b: how many cores the engine may search on for
     * this session. 1 is the default and the pre-X8b behaviour; the setting
     * is cleared when the session ends, so it never leaks into the next. */
    g_search_cores = (int)arg_int(args, 0, 1);
    if (g_search_cores < 1) g_search_cores = 1;
    console_set_title("Chess");
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638
    chess_run(); /* returns on Ctrl-C or the TM1638 STOP key (J2) */
#else
    chess_console_run(); /* returns on 'quit' */
#endif
    g_search_cores = 1;
    lisp_canvas_reset();    /* 37.3b */
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
 * Not needed for normal use; exists to check the LDR's polarity and the
 * auto-brightness thresholds (`AUTO_DARKER_AT[]`) against real ambient light
 * on real hardware. */
/* `(clock-keys [secs])` -- C1 diagnostic: print every button event as it
 * happens, with the idle pin levels first, so a miswired button is one line
 * of output rather than a menu that mysteriously does nothing. */
static lisp_val_t *prim_clock_keys(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 30);
    pico_clock_green_keys((unsigned)(secs < 1 ? 1 : secs));
    return &true_val;
}

/* `(clock-leds)` -- C1 diagnostic: walk every weekday LED and every indicator
 * LED in turn, naming each on the console as it lights. */
static lisp_val_t *prim_clock_leds(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    pico_clock_green_led_walk();
    return &true_val;
}

/* `(clock-text "STR" [secs])` -- C2 diagnostic: render a string on the panel,
 * centred if it fits and scrolling if it does not. The only way to find out
 * whether a 5x7 letter is actually legible on this hardware. */
static lisp_val_t *prim_clock_text(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    lisp_val_t *a = args->u.pair.car;
    if (a->type != LISP_STRING && a->type != LISP_SYMBOL) return &false_val;
    const char *str = (a->type == LISP_STRING) ? a->u.str : a->u.sym;
    long secs = arg_int(args, 1, 5);
    pico_clock_green_show_text(str, (unsigned)(secs < 1 ? 1 : secs));
    return &true_val;
}

#if CONFIG_ENABLE_DCF77
/* `(dcf-status)` -- D4: what the receiver has been doing, without waiting for
 * it to do anything. The same numbers as /proc/dcf77, laid out for a human
 * rather than for a parser. */
static lisp_val_t *prim_dcf_status(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    dcf_status_t st;
    dcf77_service_status(&st);

    static const char *const STATE[] = { "idle (listening)", "SYNCING",
                                         "last sync succeeded", "last sync FAILED" };
    cprintf("\nDCF-77 service: %s\n",
            STATE[(unsigned)st.state <= 3 ? (unsigned)st.state : 0]);
    if (st.state == DCF_SYNCING)
        cprintf("  giving up in %u s\n", (unsigned)st.timeout_left_s);

    char iso[32];
    uint32_t age = dcf77_service_age_s();
    if (st.ever_synced) {
        time_format_iso(&st.last_sync_utc, iso, sizeof(iso));
        cprintf("  clock last set : %s UTC, %u min ago\n", iso, (unsigned)(age / 60));
    } else {
        cprintf("  clock last set : never from the radio\n");
    }
    /* Deliberately separate from the line above: the radio can be decoding
     * perfectly while the clock has not been touched, and that distinction is
     * the whole design. */
    if (st.have_radio_time) {
        time_format_iso(&st.radio_utc, iso, sizeof(iso));
        cprintf("  radio says     : %s UTC (decoded %u s ago)\n", iso,
                (unsigned)((time_get_ms() - st.radio_at_ms) / 1000));
    } else {
        cprintf("  radio says     : nothing decoded yet\n");
    }
    cprintf("  nightly sync   : %s, at %02d:%02d local\n",
            dcf77_service_auto() ? "on" : "off",
            CONFIG_DCF77_AUTO_HOUR, CONFIG_DCF77_AUTO_MIN);
    cprintf("  attempts       : %u, of which %u succeeded\n",
            (unsigned)st.attempts, (unsigned)st.successes);
    cprintf("  pulses         : %u (%u bad), %u glitches, %u sync losses\n",
            (unsigned)st.decoder.pulses_seen, (unsigned)st.decoder.pulses_bad,
            (unsigned)st.decoder.glitches, (unsigned)st.decoder.sync_losses);
    cprintf("  frames         : %u seen, %u accepted\n",
            (unsigned)st.decoder.frames_seen, (unsigned)st.decoder.frames_accepted);
    cprintf("  longest run    : %u s (59 consecutive needed for one frame)\n",
            (unsigned)st.decoder.clean_run_max);
    {
        unsigned m10 = st.decoder.quality_total
            ? (unsigned)((st.decoder.quality_sum * 10u) / st.decoder.quality_total) : 0;
        cprintf("  mean quality   : %u.%u / 7 over %u seconds\n",
                m10 / 10, m10 % 10, (unsigned)st.decoder.quality_total);
    }
    cprintf("  last 24 s      : ");
    for (unsigned i = 0; i < st.decoder.quality_count; i++)
        cprintf("%c", '0' + (st.decoder.quality[i] > 7 ? 7 : st.decoder.quality[i]));
    cprintf("\n");
    return &true_val;
}

/* `(dcf-monitor [secs] [dark])` -- D3/D5: the signal-quality bar chart on the
 * panel, with a console commentary, for tuning an antenna by hand. With
 * `dark` non-zero the panel is blanked and only the console reports, which
 * makes a pair of runs an actual interference measurement. */
static lisp_val_t *prim_dcf_monitor(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 120);
    long dark = arg_int(args, 1, 0);
    pico_clock_green_dcf_monitor((unsigned)(secs < 1 ? 1 : secs), dark != 0);
    return &true_val;
}
#endif

/* `(beep n)` -- the boot beacon from Lisp, so init.lisp can mark its own
 * progress on a board with no console. `n` clicks and `n` LEDs, latched, so a
 * script that hangs leaves its last mark lit on the panel. */
static lisp_val_t *prim_beep(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long n = arg_int(args, 0, 1);
#if CONFIG_CLOCK_BOOT_BEACON
    pico_clock_green_boot_mark((unsigned)(n < 1 ? 1 : n));
#else
    (void)n;
#endif
    return &true_val;
}

static lisp_val_t *prim_clock_light(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    return make_int((long)pico_clock_green_read_light());
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
/* `(dcf-raw [secs])` (D2, plan/phase17_clock_ui_and_dcf77.md): power the
 * DCF-77 receiver up, work out its PON and OUT polarities from what the pin
 * actually does, and print every pulse it sees for `secs` seconds (default
 * 30). The bring-up diagnostic -- it answers "is the module wired right and
 * is there a signal here at all", which has to be true before a frame
 * decoder is worth writing. Run from the shell, where the LED matrix is not
 * scanning, this is also the quiet-reference measurement for the
 * interference work (phase17 D5 step 1). */
static lisp_val_t *prim_dcf_raw(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 30);
    if (secs < 1) secs = 1;
    dcf77_probe((unsigned)secs);
    return &true_val;
}

/* `(dcf-pins)` (D2, plan/phase17_clock_ui_and_dcf77.md): the electrical
 * diagnostic behind dcf77_pin_report() -- what voltage each DCF-77 pin is
 * actually sitting at, and what that implies about the wiring. Runs in a
 * couple of seconds and decodes nothing; it answers "is this thing connected
 * and powered", which is the question that comes before any radio. */
static lisp_val_t *prim_dcf_pins(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    dcf77_pin_report();
    return &true_val;
}

/* `(dcf-hunt [secs])`: sweep PON through all three states (low/high/float)
 * and watch OUT in each, digitally and with the ADC. The test that needs no
 * radio signal -- it asks whether the module reacts to PON at all -- plus a
 * swapped-wires check in the floating state, where nothing is driven. */
static lisp_val_t *prim_dcf_hunt(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 20);
    dcf77_hunt((unsigned)(secs < 1 ? 1 : secs));
    return &true_val;
}

/* `(dcf-pinscan)`: is the load on the OUT pin the module, or the baseboard?
 * Compares against the persona's other unused pins, pulls only. */
static lisp_val_t *prim_dcf_pinscan(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    dcf77_free_pin_survey();
    return &true_val;
}

/* `(dcf-listen [secs] [load])`: watch the receiver for minutes, printing every
 * pulse and a progress line every 10 s, so an antenna can be moved around
 * while someone watches. A non-zero second argument lights one matrix row as
 * a pure DC load -- the M0 experiment: more current on 3V3 may push the
 * Pico's regulator out of power-save mode, whose ripple is a documented
 * problem for this module. */
/* `(dcf-mirror [secs])`: light a matrix row whenever the DCF-77 OUT line is
 * high, so the signal can be watched on the display. The answer to "can I put
 * an LED on OUT" -- not on that pin (a micropower output cannot drive one),
 * but the matrix can show the same thing without loading anything. */
static lisp_val_t *prim_dcf_mirror(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 60);
    dcf77_mirror((unsigned)(secs < 1 ? 1 : secs));
    return &true_val;
}

/* `(dcf-poweron [secs])`: assert PON and watch OUT from that instant, with no
 * warm-up delay -- the window dcf77_hunt() skips, and the one an RC8000's
 * documented startup transient lives in. */
static lisp_val_t *prim_dcf_poweron(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 30);
    dcf77_power_on_capture((unsigned)(secs < 1 ? 1 : secs));
    return &true_val;
}

/* `(dcf-drivetest)`: drive the OUT pin and read it back, to tell a dead or
 * shorted pad apart from a healthy one looking at a module that is driving
 * low. The only DCF-77 diagnostic that drives that pin -- 2 mA, 50 ms. */
static lisp_val_t *prim_dcf_drivetest(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    dcf77_drive_test();
    return &true_val;
}

static lisp_val_t *prim_dcf_listen(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 120);
    long load = arg_int(args, 1, 0);
    dcf77_listen((unsigned)(secs < 1 ? 1 : secs), load != 0);
    return &true_val;
}

/* `(dcf-sync [secs] [set])`: decode an actual time off the radio, as opposed
 * to every other dcf- primitive here, which measures the signal. `set` is
 * off by default -- a diagnostic that silently changes the clock is a bad
 * diagnostic, and nothing is written on failure either way. */
static lisp_val_t *prim_dcf_sync(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long secs = arg_int(args, 0, 300);
    long set  = arg_int(args, 1, 0);
    dcf77_sync((unsigned)(secs < 0 ? 0 : secs), set != 0);
    return &true_val;
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
    scratch_t sc;
    if (!scratch_acquire(&sc, 4096)) {
        cprintf("cat: out of memory\n");
        return &nil_val;
    }
    char *buf = (char *)sc.base;
    int len = vfs_read(path, buf, 4096 - 1);
    if (len >= 0) {
        buf[len] = '\0';
        cprintf("%s\n", buf);
    } else {
        cprintf("cat: cannot read path '%s'\n", path);
    }
    scratch_release(&sc);
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
    /* 37.1: the status bar names the program, by its file name. */
    const char *name = safe_path;
    for (const char *q = safe_path; *q; q++) if (*q == '/') name = q + 1;
    console_set_title(name);
    int res = elf_load_and_run(safe_path);
    lisp_canvas_reset();    /* 37.3b: whatever it left, the shell gets text back */
    return make_int(res);
}

/* /proc/<name> files are real byte streams now (A1, vfs_open()/vfs_pread()),
 * not a printk() side effect -- read and print the actual content instead
 * of relying on vfs_read()/vfs_ls() to have printed it themselves. */
/* Streamed rather than read in one gulp, which is how it was written until
 * X5 (plan/phase23_multicore_scheduling.md).
 *
 * A single vfs_read() into a fixed 512-byte buffer silently truncated every
 * /proc file that outgrew it -- and did so invisibly: no error, no marker,
 * just a table that stops in the middle of a row. `ps` reached that point at
 * eight tasks, so it went unnoticed on a machine that normally runs six, and
 * surfaced the moment X5's background load added four more. The generator
 * had 896 bytes available (fs/vfs_server.c) and the reader took 512 of them,
 * which is the sort of mismatch that goes on being wrong quietly.
 *
 * Reading through one handle also makes the result a consistent snapshot:
 * /proc content is generated once at open() time, so successive preads
 * cannot show half of one task table and half of the next. */
static void print_proc_file(const char *path) {
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        cprintf("(no data for '%s')\n", path);
        return;
    }
    static char buf[257];
    uint64_t off = 0;
    for (;;) {
        int n = vfs_pread(fd, buf, sizeof(buf) - 1, off);
        if (n <= 0) break;
        buf[n] = '\0';
        cprintf("%s", buf);
        off += (uint64_t)n;
    }
    vfs_close(fd);
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
    scratch_t sc;
    if (!scratch_acquire(&sc, 8192)) {
        cprintf("load: out of memory\n");
        return &false_val;
    }
    char *buf = (char *)sc.base;
    int len = vfs_read(path, buf, 8192 - 1);
    if (len < 0) {
        cprintf("load: cannot open file '%s'\n", path);
        scratch_release(&sc);
        return &false_val;
    }
    buf[len] = '\0';
    /* Held across the evaluation, which is the whole point: the text being
     * evaluated lives here, and a nested load/cat gets its own buffer. */
    lisp_eval_string(buf);
    scratch_release(&sc);
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
    scratch_t sc;
    if (!scratch_acquire(&sc, 4096)) return make_str("");
    char *buf = (char *)sc.base;
    int len = vfs_read(path, buf, 4096 - 1);
    lisp_val_t *out = &nil_val;
    if (len < 0) {
        out = make_str("");
    } else {
        buf[len] = '\0';
        out = make_str(buf);
    }
    scratch_release(&sc);
    return out;
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

/* (psram) -- bytes of PSRAM this board brought up, 0 where there is none.
 * init.lisp asks it to size /ram0 (38.6, plan/phase38_psram.md): with PSRAM
 * the RAM disk costs no heap, so it is mounted large and unconditionally. */
static lisp_val_t *prim_psram(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PSRAM_BYTES)
    return make_int((long)psram_bytes());
#else
    return make_int(0);
#endif
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
    time_get_local(&tm);
    char buf[32];
    time_format_iso(&tm, buf, sizeof(buf));
    return make_str(buf);
}

/* `(date-utc)`: the clock as the kernel actually keeps it. `(date)` is local
 * time, which is a rendering of this and not a second clock. */
static lisp_val_t *prim_date_utc(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    rtc_time_t tm;
    time_get_utc(&tm);
    char buf[32];
    time_format_iso(&tm, buf, sizeof(buf));
    return make_str(buf);
}

/* `(tz)` -> the rule in force; `(tz "CET-1CEST,M3.5.0,M10.5.0/3")` sets it,
 * returning #f and keeping the old rule if it does not parse. */
static lisp_val_t *prim_tz(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (args && args->type == LISP_PAIR &&
        (args->u.pair.car->type == LISP_STRING || args->u.pair.car->type == LISP_SYMBOL)) {
        const char *spec = (args->u.pair.car->type == LISP_STRING)
                             ? args->u.pair.car->u.str : args->u.pair.car->u.sym;
        return tz_set(spec) ? &true_val : &false_val;
    }
    return make_str(tz_get());
}

static lisp_val_t *prim_set_date(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    rtc_time_t tm;
    if (args->u.pair.car->type == LISP_STRING || args->u.pair.car->type == LISP_SYMBOL) {
        const char *str = (args->u.pair.car->type == LISP_STRING) ? args->u.pair.car->u.str : args->u.pair.car->u.sym;
        if (time_parse_iso(str, &tm)) {
            /* Typed by a human: local time in, UTC stored. */
            rtc_time_t utc;
            tz_local_to_utc(&tm, &utc);
            time_set_utc(&utc);
            i2c_rtc_write_time(&utc);
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
        rtc_time_t utc;
        tz_local_to_utc(&tm, &utc);
        time_set_utc(&utc);
        i2c_rtc_write_time(&utc);
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
/* Dotted-quad parsing lives in net/ipv4.c now -- `netcfg` in the shell
 * needs the identical check, and two copies would agree only until one is
 * fixed. */

/* `(net-config "ip" "mask" ["gateway"])` -- phase 18 N4 §3, wired to the IP
 * stack by phase 19's R2.
 *
 * The address comes from the SD card using the boot path that already exists:
 * /sd0/system/etc/usr_init.lisp is loaded at boot when present, so the
 * network configuration is one line in it and needs no new file format, no
 * parser and no second convention. The gateway argument may be omitted on an
 * isolated segment with no router.
 *
 * On a board with no interface this fails and says so rather than raising:
 * an unbound symbol in a boot script would abort the whole script over a
 * peripheral that is merely absent.
 *
 * The gateway's address, from the SD card, using the boot path that already
 * exists: /sd0/system/etc/usr_init.lisp is loaded at boot when present, so
 * the network configuration is one line in it and needs no new file format,
 * no parser and no second convention. The gateway argument may be omitted on
 * an isolated segment with no router.
 *
 * Applying an address re-runs the socket setup, so this is safe to call on a
 * running board -- which is what makes it usable interactively while
 * bringing a network up. */
static lisp_val_t *prim_net_config(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t ip[4], mask[4], gw[4] = { 0, 0, 0, 0 };
    if (!args || args->type != LISP_PAIR) return &false_val;
    if (!ipv4_parse(get_str_val(args->u.pair.car), ip)) return &false_val;

    lisp_val_t *rest = args->u.pair.cdr;
    if (!rest || rest->type != LISP_PAIR) return &false_val;
    if (!ipv4_parse(get_str_val(rest->u.pair.car), mask)) return &false_val;

    rest = rest->u.pair.cdr;
    if (rest && rest->type == LISP_PAIR) {
        if (!ipv4_parse(get_str_val(rest->u.pair.car), gw)) return &false_val;
    }

    if (net_set_address(ip, mask, gw) != 0) {
        cprintf("[Net] %u.%u.%u.%u/%u.%u.%u.%u parsed, but this board has no "
                "network interface\n",
                ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3]);
        return &false_val;
    }
    cprintf("[Net] %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n",
            ip[0], ip[1], ip[2], ip[3], mask[0], mask[1], mask[2], mask[3],
            gw[0], gw[1], gw[2], gw[3]);
    return &true_val;
}

/* `(mqttd-file "name" "path" "field" [decimals [max-age-s]])` -- phase 40,
 * item 10: publish a value read from a file, typically a mounted node's
 * /proc/sensors, so a gateway's usr_init.lisp can republish its nodes. The
 * same as the shell's `mqttd file`; see mqttd_add_file_source(). */
static lisp_val_t *prim_mqttd_file(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    lisp_val_t *name = lisp_list_ref(args, 0), *path = lisp_list_ref(args, 1);
    lisp_val_t *field = lisp_list_ref(args, 2);
    if (!name || !path || !field || name->type != LISP_STRING ||
        path->type != LISP_STRING || field->type != LISP_STRING)
        return &false_val;
    long decimals = arg_int(args, 3, 0), max_age = arg_int(args, 4, 0);
    if (decimals < 0 || decimals > 9 || max_age < 0 || max_age > 65535) return &false_val;
    return mqttd_add_file_source(get_str_val(name), get_str_val(path), get_str_val(field),
                                 (uint8_t)decimals, (uint16_t)max_age, NULL) == 0
        ? &true_val : &false_val;
}

/* `(ntp-sync ["server"])` -- R6: set this board's clock from the segment.
 *
 * The point of the primitive rather than only the shell command: this is what
 * a persona runs at boot. `/sd0/system/etc/usr_init.lisp` is already the
 * place a board's network line lives, and a clock that sets itself belongs on
 * the line after it.
 *
 * With no argument it asks the gateway, like `ntp` does. Returns the offset
 * in milliseconds as a number -- not #t -- so a script can tell "the clock
 * was already right" from "the clock was two minutes out", which is the
 * difference between a healthy board and one whose oscillator or last sync
 * needs looking at. #f on any failure, never an error: a boot script must not
 * abort because a time server was switched off.
 *
 * There is no retry and no schedule here. Phase 24 owns a clock that keeps
 * itself disciplined; this sets it once, when asked. */
static lisp_val_t *prim_ntp_sync(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    uint8_t server[4];

    if (args && args->type == LISP_PAIR) {
        if (!ipv4_parse(get_str_val(args->u.pair.car), server)) {
            cprintf("[NTP] not a dotted quad\n");
            return &false_val;
        }
    } else {
        const net_state_t *st = net_state();
        if (!st || !st->configured) {
            cprintf("[NTP] no address configured\n");
            return &false_val;
        }
        memcpy(server, st->gw, 4);
        if (!server[0] && !server[1] && !server[2] && !server[3]) {
            cprintf("[NTP] no gateway to ask, and no server given\n");
            return &false_val;
        }
    }

    ntp_result_t r;
    int rc = ntp_sync(server, 3000u, &r);
    if (rc != 0) {
        cprintf("[NTP] %u.%u.%u.%u: %s\n", server[0], server[1], server[2], server[3],
                ntp_err_str(rc));
        return &false_val;
    }
    ntp_print_result(&r, true);
    /* Saturated, not truncated. A Lisp integer is a `long`, which is 32 bits
     * on RV32 and RP2350, and the first sync of a board that has never been
     * told the time is an offset of decades -- which wraps to a small,
     * plausible, wrong number. A script asking "was the clock far out?" is
     * then told "no". Clamping keeps the answer true at the only resolution
     * this type can carry. */
    /* Milliseconds, still: a Lisp integer is a `long` and this is a boot
     * script's sanity check, not a measurement. The microseconds are in the
     * report ntp_print_result() just printed, and in ntp_result_t for a
     * caller that wants them. */
    int64_t off = r.offset_us / 1000;
    if (off >  (int64_t)LONG_MAX) off = LONG_MAX;
    if (off <  (int64_t)LONG_MIN) off = LONG_MIN;
    return make_int((long)off);
}

/* `(net-status)` -- the same report `net` prints, for a script that wants to
 * see it after configuring. */
static lisp_val_t *prim_net_status(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    net_print_status();
    return net_configured() ? &true_val : &false_val;
}

/* `(net-mount "name" "1.2.3.4" [port])` -- dial a peer and mount its
 * namespace at /name (R3b, plan/phase19_ip_stack_and_ethernet.md).
 *
 * The distributed story phase 5 designed, finally over a network rather than
 * a cable: `mount-remote` has always worked over any p9_link_t, and since
 * R3b an outbound TCP connection is one. Authentication is not a separate
 * step -- p9_remote_mount_open() authenticates with this node's own key when
 * it has one, and the peer's policy decides whether that was needed.
 *
 * The handshake is waited for here rather than inside tcp_connect(), because
 * this call runs on the shell's task while `netsrv` completes it: yielding is
 * what lets that happen, and blocking inside the stack would not. */
/* Which mount name owns which outbound connection.
 *
 * Without this, `(unmount "peer")` frees the VFS entry and leaves the TCP
 * connection ESTABLISHED forever: an idle connection with nothing
 * outstanding has no retransmission timer to expire, so nothing would ever
 * reclaim it -- and with two connection slots in total, two unmounts would
 * leave a node unable to dial anyone until it rebooted. Two entries, matching
 * net/tcp.c's own limit. */
#define NET_MOUNT_MAX 2
static struct {
    bool in_use;
    char name[24];
    p9_link_t *link;
} g_net_mounts[NET_MOUNT_MAX];

static void net_mount_remember(const char *name, p9_link_t *link) {
    for (uint32_t i = 0; i < NET_MOUNT_MAX; i++) {
        if (!g_net_mounts[i].in_use) {
            g_net_mounts[i].in_use = true;
            strncpy(g_net_mounts[i].name, name, sizeof(g_net_mounts[i].name) - 1);
            g_net_mounts[i].name[sizeof(g_net_mounts[i].name) - 1] = '\0';
            g_net_mounts[i].link = link;
            return;
        }
    }
}

/* Returns the link this name dialled, and forgets it. NULL for a mount that
 * came from somewhere else -- a cable, a chardev -- which must be left alone. */
static p9_link_t *net_mount_forget(const char *name) {
    for (uint32_t i = 0; i < NET_MOUNT_MAX; i++) {
        if (g_net_mounts[i].in_use && strcmp(g_net_mounts[i].name, name) == 0) {
            g_net_mounts[i].in_use = false;
            return g_net_mounts[i].link;
        }
    }
    return NULL;
}

/* `(net-identity)` reports; `(net-identity "clock-01")` sets.
 *
 * The runtime end of kernel/identity.h's resolution order, and the one an
 * operator actually reaches: a line in /sd0/system/etc/usr_init.lisp beside
 * (net-config), so a board's name is configured the same way its address is.
 * The MAC deliberately does not follow a rename -- see identity.h. */
static lisp_val_t *prim_net_identity(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (args && args->type == LISP_PAIR) {
        const char *want = get_str_val(args->u.pair.car);
        if (node_set_name(want) != 0) {
            cprintf("[Node] '%s' is not a usable name (letters, digits, '-', '.', "
                    "under %u characters)\n", want ? want : "", NODE_NAME_MAX);
            return &false_val;
        }
    }
    char mac[18];
    netif_mac_str(node_mac(), mac);
    cprintf("[Node] %s (%s), mac %s (%s)\n", node_name(), node_name_source(),
            mac, node_mac_source());
    return &true_val;
}

/* --- `identity`, `identity-name`, `identity-provision`, `identity-key` --
 * I3, plan/phase21_identity_and_authentication.md §6: the same four
 * subcommands as kernel/shell.c's `identity` command, over the same typed
 * functions in kernel/identity.c, so /sd0/system/etc/usr_init.lisp can drive
 * provisioning the same way it already drives (net-identity ...). */

static void identity_print_report_lisp(void) {
    cprintf("name: %s (%s)\n", node_name(), node_name_source());
    char mac[18];
    netif_mac_str(node_mac(), mac);
    cprintf("mac: %s (%s)\n", mac, node_mac_source());

    static const char hex[] = "0123456789abcdef";
    uint8_t uid[NODE_UID_LEN];
    if (node_uid(uid)) {
        char uidhex[NODE_UID_LEN * 2 + 1];
        for (unsigned i = 0; i < NODE_UID_LEN; i++) {
            uidhex[i * 2]     = hex[uid[i] >> 4];
            uidhex[i * 2 + 1] = hex[uid[i] & 0x0f];
        }
        uidhex[NODE_UID_LEN * 2] = '\0';
        cprintf("uid: %s (%s)\n", uidhex, node_uid_source());
    } else {
        cprintf("uid: none (%s)\n", node_uid_source());
    }

    uint8_t key[NODE_DEVKEY_MAX];
    uint32_t key_len = 0;
    if (node_devkey(key, sizeof(key), &key_len)) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(key, key_len, fp);
        cprintf("key fingerprint: %s\n", fp);
    } else {
        cprintf("key fingerprint: none\n");
    }
    memset(key, 0, sizeof(key));
}

/* `(identity)` -- report. */
static lisp_val_t *prim_identity(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    identity_print_report_lisp();
    return &true_val;
}

/* `(identity-name "clock-01")` -- persists, unlike (net-identity "..."). */
static lisp_val_t *prim_identity_name(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);
    node_id_result_t rc = node_identity_rename_persistent(name);
    if (rc != NODE_ID_OK) { cprintf("identity-name: %s\n", node_id_result_str(rc)); return &false_val; }
    cprintf("identity: renamed to '%s' (persisted)\n", node_name());
    return &true_val;
}

/* `(identity-provision)` or `(identity-provision "--force")`. */
static lisp_val_t *prim_identity_provision(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    bool force = false;
    if (args && args->type == LISP_PAIR) {
        const char *arg = get_str_val(args->u.pair.car);
        force = arg && strcmp(arg, "--force") == 0;
    }
    node_id_result_t rc = node_identity_provision(force);
    if (rc != NODE_ID_OK) { cprintf("identity-provision: %s\n", node_id_result_str(rc)); return &false_val; }
    cprintf("identity: provisioned\n");
    identity_print_report_lisp();
    return &true_val;
}

/* `(identity-key "aabbcc...")` or `(identity-key "--generate")`. */
static lisp_val_t *prim_identity_key(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) { cprintf("usage: (identity-key \"<hex>\"|\"--generate\")\n"); return &false_val; }
    const char *arg = get_str_val(args->u.pair.car);

    node_id_result_t rc;
    if (arg && strcmp(arg, "--generate") == 0) {
        rc = node_identity_generate_key();
    } else {
        uint8_t key[NODE_DEVKEY_MAX];
        uint32_t len = 0;
        for (const char *h = arg; h && h[0] && h[1] && len < sizeof(key); h += 2) {
            int hi = -1, lo = -1;
            for (int p = 0; p < 2; p++) {
                char c = h[p];
                int v = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (p == 0) hi = v; else lo = v;
            }
            if (hi < 0 || lo < 0) break;
            key[len++] = (uint8_t)((hi << 4) | lo);
        }
        if (len == 0) { cprintf("identity-key: expected an even-length hex string, or \"--generate\"\n"); return &false_val; }
        rc = node_identity_set_key(key, len);
        memset(key, 0, sizeof(key));
    }

    if (rc != NODE_ID_OK) { cprintf("identity-key: %s\n", node_id_result_str(rc)); return &false_val; }
    cprintf("identity: key installed\n");
    uint8_t k[NODE_DEVKEY_MAX];
    uint32_t klen = 0;
    if (node_devkey(k, sizeof(k), &klen)) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(k, klen, fp);
        cprintf("key fingerprint: %s\n", fp);
    }
    memset(k, 0, sizeof(k));
    return &true_val;
}

/* --- `peers`, `peers-add`, `peers-remove` -- I5, §5.2/§6 --------------- */

static void peers_print_report_lisp(void) {
    p9_grant_t entries[P9_GRANTS_MAX];
    uint32_t count = p9_grants_list(entries, P9_GRANTS_MAX);
    if (count == 0) { cprintf("peers: no grants configured\n"); return; }
    cprintf("%-16s %-16s %-20s mode\n", "name", "fingerprint", "aname");
    for (uint32_t i = 0; i < count; i++) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(entries[i].key, entries[i].key_len, fp);
        cprintf("%-16s %-16s %-20s %s\n", entries[i].name, fp, entries[i].aname,
                entries[i].read_only ? "ro" : "rw");
    }
}

/* `(peers)` -- report. */
static lisp_val_t *prim_peers(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    peers_print_report_lisp();
    return &true_val;
}

/* `(peers-add "name" "hex" ["aname"] ["ro"|"rw"])`. */
static lisp_val_t *prim_peers_add(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *name = get_str_val(lisp_list_ref(args, 0));
    const char *hex  = get_str_val(lisp_list_ref(args, 1));
    const char *tok3 = get_str_val(lisp_list_ref(args, 2));
    const char *tok4 = get_str_val(lisp_list_ref(args, 3));
    if (!name || !name[0] || !hex || !hex[0]) {
        cprintf("usage: (peers-add \"name\" \"hex\" [\"aname\"] [\"ro\"|\"rw\"])\n");
        return &false_val;
    }

    char aname[P9_MAX_NAME_LEN];
    strncpy(aname, "/", sizeof(aname) - 1);
    aname[sizeof(aname) - 1] = '\0';
    bool read_only = false;
    if (tok3 && strcmp(tok3, "ro") == 0)      read_only = true;
    else if (tok3 && strcmp(tok3, "rw") == 0) read_only = false;
    else if (tok3 && tok3[0])                 strncpy(aname, tok3, sizeof(aname) - 1);
    if (tok4 && strcmp(tok4, "ro") == 0)      read_only = true;
    else if (tok4 && strcmp(tok4, "rw") == 0) read_only = false;

    uint8_t key[P9_AUTH_KEY_MAX];
    uint32_t klen = 0;
    for (const char *h = hex; h[0] && h[1] && klen < sizeof(key); h += 2) {
        int hi = -1, lo = -1;
        for (int p = 0; p < 2; p++) {
            char c = h[p];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (p == 0) hi = v; else lo = v;
        }
        if (hi < 0 || lo < 0) break;
        key[klen++] = (uint8_t)((hi << 4) | lo);
    }
    if (klen == 0) { cprintf("peers-add: expected an even-length hex string for the key\n"); return &false_val; }

    p9_grant_result_t rc = p9_grants_add(name, key, klen, aname, read_only);
    memset(key, 0, sizeof(key));
    if (rc != P9_GRANT_OK) { cprintf("peers-add: %s\n", p9_grant_result_str(rc)); return &false_val; }
    cprintf("peers: granted '%s' at %s (%s)\n", name, aname, read_only ? "ro" : "rw");
    return &true_val;
}

/* `(peers-remove "name")`. */
static lisp_val_t *prim_peers_remove(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *name = get_str_val(lisp_list_ref(args, 0));
    if (!name || !name[0]) { cprintf("usage: (peers-remove \"name\")\n"); return &false_val; }
    p9_grant_result_t rc = p9_grants_remove(name);
    if (rc != P9_GRANT_OK) { cprintf("peers-remove: %s\n", p9_grant_result_str(rc)); return &false_val; }
    cprintf("peers: removed '%s'\n", name);
    return &true_val;
}

/* --- `wlan`, `wlan-set` -- I6, §5.3/§6 ---------------------------------- */

static void wlan_print_report_lisp(void) {
    char ssid[NODE_WLAN_SSID_MAX + 1];
    uint8_t psk[NODE_WLAN_PSK_LEN];
    bool have_ssid = node_wlan_ssid(ssid, sizeof(ssid));
    bool have_psk = node_wlan_psk(psk);
    cprintf("ssid: %s\n", have_ssid ? ssid : "none");
    if (have_psk) {
        char fp[KEY_FINGERPRINT_HEX_LEN + 1];
        key_fingerprint_hex(psk, sizeof(psk), fp);
        cprintf("psk fingerprint: %s\n", fp);
    } else {
        cprintf("psk fingerprint: none\n");
    }
    memset(psk, 0, sizeof(psk));
}

/* `(wlan)` -- report. */
static lisp_val_t *prim_wlan(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    wlan_print_report_lisp();
    return &true_val;
}

/* `(wlan-set "ssid" "psk-hex")` -- the hex must be the *derived* PSK
 * (tools/provision.py's derive_wpa2_psk()), never a passphrase. */
static lisp_val_t *prim_wlan_set(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    const char *ssid = get_str_val(lisp_list_ref(args, 0));
    const char *hex   = get_str_val(lisp_list_ref(args, 1));
    if (!ssid || !ssid[0] || !hex || !hex[0]) {
        cprintf("usage: (wlan-set \"ssid\" \"psk-hex\")\n");
        return &false_val;
    }
    if (strlen(hex) != NODE_WLAN_PSK_LEN * 2) {
        cprintf("wlan-set: expected exactly %u hex characters (a derived WPA2 PSK is always 256 "
                "bits) -- derive one with tools/provision.py, not by hand\n", (unsigned)(NODE_WLAN_PSK_LEN * 2));
        return &false_val;
    }

    uint8_t psk[NODE_WLAN_PSK_LEN];
    uint32_t len = 0;
    for (const char *h = hex; h[0] && h[1] && len < sizeof(psk); h += 2) {
        int hi = -1, lo = -1;
        for (int p = 0; p < 2; p++) {
            char c = h[p];
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (p == 0) hi = v; else lo = v;
        }
        if (hi < 0 || lo < 0) break;
        psk[len++] = (uint8_t)((hi << 4) | lo);
    }
    if (len != NODE_WLAN_PSK_LEN) {
        cprintf("wlan-set: expected an even-length hex string\n");
        memset(psk, 0, sizeof(psk));
        return &false_val;
    }

    node_id_result_t rc = node_identity_set_wlan(ssid, (uint32_t)strlen(ssid), psk, len);
    memset(psk, 0, sizeof(psk));
    if (rc != NODE_ID_OK) { cprintf("wlan-set: %s\n", node_id_result_str(rc)); return &false_val; }
    cprintf("wlan: credential installed\n");
    wlan_print_report_lisp();
    return &true_val;
}

static lisp_val_t *prim_net_mount(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *name = get_str_val(args->u.pair.car);

    lisp_val_t *rest = args->u.pair.cdr;
    if (!rest || rest->type != LISP_PAIR) return &false_val;
    uint8_t ip[4];
    if (!ipv4_parse(get_str_val(rest->u.pair.car), ip)) return &false_val;

    uint16_t port = 564;
    rest = rest->u.pair.cdr;
    if (rest && rest->type == LISP_PAIR && rest->u.pair.car &&
        rest->u.pair.car->type == LISP_INT) {
        port = (uint16_t)rest->u.pair.car->u.i;
    }

    p9_link_t *link = tcp_connect(ip, port);
    if (!link) {
        cprintf("[Net] no free connection, or no address configured\n");
        return &false_val;
    }

    /* Bounded, for the same reason phase 18's N5 client wait is: a peer that
     * is switched off must cost a few seconds, not the session. */
    int ready = 0;
    for (uint32_t spin = 0; spin < 400000u; spin++) {
        ready = tcp_link_ready(link);
        if (ready != 0) break;
        sched_yield();
    }
    if (ready != 1) {
        cprintf("[Net] %u.%u.%u.%u:%u did not answer\n", ip[0], ip[1], ip[2], ip[3], port);
        tcp_close(link);
        return &false_val;
    }

    if (vfs_mount_remote(name, link) != 0) {
        cprintf("[Net] connected, but /%s could not be mounted\n", name);
        tcp_close(link);
        return &false_val;
    }
    net_mount_remember(name, link);
    cprintf("[Net] /%s mounted from %u.%u.%u.%u:%u\n", name,
            ip[0], ip[1], ip[2], ip[3], port);
    return &true_val;
}

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
    int rc = vfs_unmount(name);
    /* If this name was dialled by (net-mount), the connection under it is
     * ours to close -- a graceful FIN, which is also the only thing in this
     * system that reaches TCP's active-close path. Mounts that arrived over a
     * cable are not ours and are left alone. */
    p9_link_t *link = net_mount_forget(name);
    if (link) tcp_close(link);
    return (rc == 0) ? &true_val : &false_val;
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

static lisp_val_t *prim_help(lisp_val_t *args, lisp_val_t *env);
#include "builtins_table.h"

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
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        cprintf("  %s -- %s\n", builtins[i].name,
                builtins[i].val->type == LISP_PRIMITIVE ? "primitive" : "value");
        count++;
    }
    cprintf("-------------------------------------------------\n");
    cprintf("%d bound symbols. Special forms (not primitives, so not listed\n"
           "above): define, lambda, quote / ', if, begin, let, named let\n"
           "(let name ((v init)...) body...), let*, while, cond, set!.\n\n", count);
    return &nil_val;
}

void lisp_init(void) {
    memset(sym_hash_table, 0, sizeof(sym_hash_table));
    node_pool_idx = 0;
    node_pool_exhausted_warned = false;
    string_small_idx = 0;
    string_large_idx = 0;
    string_free_head = -1;
    string_large_free_head = -1;
    string_small_free_count = 0;
    string_large_free_count = 0;
    string_pool_exhausted_warned = false;
    eval_depth = 0;
    eval_depth_exceeded_warned = false;
    global_env = &nil_val;


    printk("[Lisp Engine] Online.\n");


    /* Automatically load system boot scripts if present.
     *
     * Found through the search path (C1) rather than by trying /sd0 and then
     * /flash0 by hand, which is what this did before. Same idea, generalised:
     * the two hardcoded volumes *were* a search path, just one that only this
     * function knew about and that no board could reorder. Now a board that
     * mounts something else, or wants a different precedence, says so in one
     * place and every lookup follows. */
    /* On-demand like the three primitives above (§2.5). This one runs once,
     * at boot, and the pages go straight back afterwards -- the buffer used
     * to sit in .bss for the life of the board to serve two reads during
     * init. */
    scratch_t boot_sc;
    if (!scratch_acquire(&boot_sc, 8192)) {
        printk("[Lisp Boot] No memory for the boot script buffer; skipping stdlib/init\n");
        return;
    }
    char *boot_buf = (char *)boot_sc.base;
    char script[128];
    int len = 0;

    if (path_resolve("etc", "stdlib.lisp", "", script, sizeof(script)) == 0) {
        len = vfs_read(script, boot_buf, 8192 - 1);
    }
    if (len > 0) {
        boot_buf[len] = '\0';
        lisp_eval_string(boot_buf);
        printk("[Lisp Boot] Loaded %s\n", script);
    }

    len = 0;
    if (path_resolve("etc", "init.lisp", "", script, sizeof(script)) == 0) {
        len = vfs_read(script, boot_buf, 8192 - 1);
    }
    if (len > 0) {
        boot_buf[len] = '\0';
        lisp_eval_string(boot_buf);
        printk("[Lisp Boot] Executed %s\n", script);
    }

    scratch_release(&boot_sc);
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
        case LISP_BIGNUM: {
            char buf[700];
            bn_to_string(val, buf, sizeof(buf));
            cprintf("%s", buf);
            break;
        }
        case LISP_RATIO: {
            char buf[1400];
            ratio_to_string(val, buf, sizeof(buf));
            cprintf("%s", buf);
            break;
        }
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

    /* Quasiquote Syntax `expr */
    if (**str == '`') {
        (*str)++; // skip '`'
        lisp_val_t *val = lisp_read(str);
        if (!val) val = &nil_val;
        return make_pair(make_sym("quasiquote"), make_pair(val, &nil_val));
    }

    /* Unquote Syntax ,@expr and ,expr */
    if (**str == ',') {
        (*str)++; // skip ','
        const char *op_name = "unquote";
        if (**str == '@') {
            (*str)++; // skip '@'
            op_name = "unquote-splicing";
        }
        lisp_val_t *val = lisp_read(str);
        if (!val) val = &nil_val;
        return make_pair(make_sym(op_name), make_pair(val, &nil_val));
    }

    /* Double Quoted Strings "..." */
    if (**str == '"') {
        (*str)++; // skip opening quote
        char buf[STRING_SLOT_LEN];
        int i = 0;
        while (**str != '"' && **str != '\0' && i < (int)sizeof(buf) - 1) {
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
            if (**str == '.' && is_delimiter((*str)[1])) {
                /* Dotted pair tail */
                if (!head) {
                    printk("[Lisp Syntax Error] Unexpected '.' at start of list\n");
                    while (**str != ')' && **str != '\0') (*str)++;
                    if (**str == ')') (*str)++;
                    return &nil_val;
                }
                (*str)++; // skip '.'
                skip_whitespace(str);
                lisp_val_t *cdr_val = lisp_read(str);
                if (!cdr_val) cdr_val = &nil_val;
                tail->u.pair.cdr = cdr_val;
                skip_whitespace(str);
                if (**str != ')') {
                    printk("[Lisp Syntax Error] Expected ')' after dotted pair tail\n");
                    while (**str != ')' && **str != '\0') (*str)++;
                }
                if (**str == ')') (*str)++;
                return head;
            }

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

        lisp_val_t *res = parse_number_token(str);
        return res ? res : &nil_val;
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
        if (!is_number_token(*str)) {
            char bad_tok[32];
            int i = 0;
            while (**str != '\0' && !is_delimiter(**str)) {
                if (i < 31) bad_tok[i++] = **str;
                (*str)++;
            }
            bad_tok[i] = '\0';
            printk("[Lisp Syntax Error] Invalid number: '%s'\n", bad_tok);
            return &nil_val;
        }
        lisp_val_t *res = parse_number_token(str);
        return res ? res : &nil_val;
    }

    /* Symbols */
    char buf[32];
    int i = 0;
    while (**str != '\0' && !is_delimiter(**str)) {
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
                env_set_sym(&local_env, p->u.pair.car, a->u.pair.car);
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
/* Quasiquote evaluator: handles nested quasiquoting, unquote (,), and unquote-splicing (,@) */
static lisp_val_t *eval_quasiquote(lisp_val_t *form, lisp_val_t *env, int depth) {
    if (!form || form->type != LISP_PAIR) {
        return form ? form : &nil_val;
    }

    lisp_val_t *head = form->u.pair.car;
    if (head && head->type == LISP_SYMBOL) {
        if (head == &sym_quasiquote || streq(head->u.sym, "quasiquote")) {
            lisp_val_t *inner = eval_quasiquote(form->u.pair.cdr, env, depth + 1);
            return make_pair((lisp_val_t *)&sym_quasiquote, inner);
        }
        if (head == &sym_unquote || streq(head->u.sym, "unquote")) {
            if (depth == 0) {
                lisp_val_t *expr = (form->u.pair.cdr && form->u.pair.cdr->type == LISP_PAIR)
                    ? form->u.pair.cdr->u.pair.car : &nil_val;
                return lisp_eval(expr, env);
            } else {
                lisp_val_t *inner = eval_quasiquote(form->u.pair.cdr, env, depth - 1);
                return make_pair((lisp_val_t *)&sym_unquote, inner);
            }
        }
        if (head == &sym_unquote_splicing || streq(head->u.sym, "unquote-splicing")) {
            if (depth == 0) {
                printk("[Lisp Error] unquote-splicing not in list context\n");
                return &nil_val;
            } else {
                lisp_val_t *inner = eval_quasiquote(form->u.pair.cdr, env, depth - 1);
                return make_pair((lisp_val_t *)&sym_unquote_splicing, inner);
            }
        }
    }

    lisp_val_t *first = form->u.pair.car;
    lisp_val_t *rest = form->u.pair.cdr;

    /* Check if first is `(unquote-splicing expr)` at depth 0 */
    if (depth == 0 && first && first->type == LISP_PAIR &&
        first->u.pair.car && first->u.pair.car->type == LISP_SYMBOL &&
        (first->u.pair.car == &sym_unquote_splicing || streq(first->u.pair.car->u.sym, "unquote-splicing"))) {
        lisp_val_t *expr = (first->u.pair.cdr && first->u.pair.cdr->type == LISP_PAIR)
            ? first->u.pair.cdr->u.pair.car : &nil_val;
        lisp_val_t *splice_val = lisp_eval(expr, env);
        lisp_val_t *rest_val = eval_quasiquote(rest, env, depth);

        if (!splice_val || splice_val->type == LISP_NIL) {
            return rest_val;
        }
        if (splice_val->type != LISP_PAIR) {
            printk("[Lisp Error] unquote-splicing: expected list, got non-pair\n");
            return rest_val;
        }
        lisp_val_t *head_res = &nil_val;
        lisp_val_t *tail_res = NULL;
        for (lisp_val_t *c = splice_val; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
            lisp_val_t *p = make_pair(c->u.pair.car, &nil_val);
            if (!tail_res) {
                head_res = p;
                tail_res = p;
            } else {
                tail_res->u.pair.cdr = p;
                tail_res = p;
            }
        }
        if (tail_res) {
            tail_res->u.pair.cdr = rest_val;
            return head_res;
        }
        return rest_val;
    }

    lisp_val_t *car_val = eval_quasiquote(first, env, depth);
    lisp_val_t *cdr_val = eval_quasiquote(rest, env, depth);
    return make_pair(car_val, cdr_val);
}

static lisp_val_t *lisp_eval_step(lisp_val_t *val, lisp_val_t *env) {
tail_call:
    if (!val) return &nil_val;

    if (val->type == LISP_INT || val->type == LISP_STRING || val->type == LISP_PRIMITIVE ||
        val->type == LISP_LAMBDA || val->type == LISP_BIGNUM || val->type == LISP_RATIO) {
        return val;
    }


    if (val->type == LISP_SYMBOL) {
        lisp_val_t *res = env_get_sym(env, val);
        if (!res) res = builtin_get(val->u.sym);       /* 37.3a: the built-ins */
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

        /* Special forms */
        if (op->type == LISP_SYMBOL) {
            switch (op->u.sym[0]) {
                case 'q':
                    /* Special form: quote */
                    if (op == &sym_quote || streq(op->u.sym, "quote")) {
                        return (args && args->type == LISP_PAIR) ? args->u.pair.car : &nil_val;
                    }
                    /* Special form: quasiquote */
                    if (op == &sym_quasiquote || streq(op->u.sym, "quasiquote")) {
                        lisp_val_t *form = (args && args->type == LISP_PAIR) ? args->u.pair.car : &nil_val;
                        return eval_quasiquote(form, refresh_global_tail(env, env, was_global), 0);
                    }
                    break;

                case 'u':
                    /* Special forms: unquote / unquote-splicing outside quasiquote */
                    if (op == &sym_unquote || streq(op->u.sym, "unquote") ||
                        op == &sym_unquote_splicing || streq(op->u.sym, "unquote-splicing")) {
                        printk("[Lisp Error] %s: unquote outside quasiquote\n", op->u.sym);
                        return &nil_val;
                    }
                    break;

                case 'i':
                    /* Special form: if -- the taken branch is a tail position. */
                    if (op == &sym_if || streq(op->u.sym, "if")) {
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
                    break;

                case 'b':
                    /* Special form: begin -- the last form is a tail position. */
                    if (op == &sym_begin || streq(op->u.sym, "begin")) {
                        lisp_val_t *last = eval_all_but_last(args, env, env, was_global);
                        if (!last) return &nil_val;
                        val = last;
                        env = refresh_global_tail(env, env, was_global);
                        goto tail_call;
                    }
                    break;

                case 'l':
                    /* Special form: let -- bindings are evaluated against the outer
                     * env, not each other (that's let*, right below), the last body
                     * form is a tail position in the extended env. */
                    if (op == &sym_let || streq(op->u.sym, "let")) {
                        if (args && args->type == LISP_PAIR && args->u.pair.car->type == LISP_SYMBOL &&
                            args->u.pair.cdr && args->u.pair.cdr->type == LISP_PAIR) {
                            lisp_val_t *name_sym = args->u.pair.car;
                            lisp_val_t *bindings = args->u.pair.cdr->u.pair.car;
                            lisp_val_t *body = args->u.pair.cdr->u.pair.cdr;

                            lisp_val_t *params_head = &nil_val, *params_tail = NULL;
                            lisp_val_t *call_args_head = &nil_val, *call_args_tail = NULL;
                            for (lisp_val_t *b = bindings; b && b->type == LISP_PAIR; b = b->u.pair.cdr) {
                                lisp_val_t *pair = b->u.pair.car;
                                if (pair && pair->type == LISP_PAIR && pair->u.pair.car->type == LISP_SYMBOL) {
                                    lisp_val_t *bound = lisp_eval(pair->u.pair.cdr ? pair->u.pair.cdr->u.pair.car : &nil_val,
                                                                   refresh_global_tail(env, env, was_global));
                                    lisp_val_t *pnode = make_pair(pair->u.pair.car, &nil_val);
                                    lisp_val_t *anode = make_pair(bound, &nil_val);
                                    if (!params_tail) { params_head = pnode; params_tail = pnode; } else { params_tail->u.pair.cdr = pnode; params_tail = pnode; }
                                    if (!call_args_tail) { call_args_head = anode; call_args_tail = anode; } else { call_args_tail->u.pair.cdr = anode; call_args_tail = anode; }
                                }
                            }

                            lisp_val_t *loop_env = refresh_global_tail(env, env, was_global);
                            lisp_val_t *lam = alloc_node(LISP_LAMBDA);
                            lam->u.lambda.params = params_head;
                            lam->u.lambda.body = body;
                            env_set_sym(&loop_env, name_sym, lam);
                            lam->u.lambda.env = loop_env; /* ties the knot: name now resolves to lam within lam's own captured env */

                            /* Apply lam to call_args_head in tail position -- mirrors
                             * the LISP_LAMBDA application branch further down exactly. */
                            lisp_val_t *local_env = lam->u.lambda.env;
                            bool nl_was_global = (local_env == global_env);
                            lisp_val_t *nl_original_tail = local_env;
                            lisp_val_t *p = lam->u.lambda.params;
                            lisp_val_t *a = call_args_head;
                            while (p && p->type == LISP_PAIR && a && a->type == LISP_PAIR) {
                                if (p->u.pair.car->type == LISP_SYMBOL) {
                                    env_set_sym(&local_env, p->u.pair.car, a->u.pair.car);
                                }
                                p = p->u.pair.cdr;
                                a = a->u.pair.cdr;
                            }
                            lisp_val_t *last = eval_all_but_last(lam->u.lambda.body, local_env, nl_original_tail, nl_was_global);
                            if (!last) return &nil_val;
                            val = last;
                            env = refresh_global_tail(local_env, nl_original_tail, nl_was_global);
                            goto tail_call;
                        }

                        if (args && args->type == LISP_PAIR) {
                            lisp_val_t *bindings = args->u.pair.car;
                            lisp_val_t *body = args->u.pair.cdr;
                            lisp_val_t *local_env = env;

                            for (lisp_val_t *b = bindings; b && b->type == LISP_PAIR; b = b->u.pair.cdr) {
                                lisp_val_t *pair = b->u.pair.car;
                                if (pair && pair->type == LISP_PAIR && pair->u.pair.car->type == LISP_SYMBOL) {
                                    lisp_val_t *bound = lisp_eval(pair->u.pair.cdr ? pair->u.pair.cdr->u.pair.car : &nil_val, refresh_global_tail(env, env, was_global));
                                    env_set_sym(&local_env, pair->u.pair.car, bound);
                                }
                            }

                            lisp_val_t *last = eval_all_but_last(body, local_env, env, was_global);
                            if (!last) return &nil_val;
                            val = last;
                            env = refresh_global_tail(local_env, env, was_global);
                            goto tail_call;
                        }
                    } else if (op == &sym_let_star || streq(op->u.sym, "let*")) {
                        /* Special form: let* (S2, plan/phase13_lisp_engine_extensions.md) */
                        if (args && args->type == LISP_PAIR) {
                            lisp_val_t *bindings = args->u.pair.car;
                            lisp_val_t *body = args->u.pair.cdr;
                            lisp_val_t *local_env = env;

                            for (lisp_val_t *b = bindings; b && b->type == LISP_PAIR; b = b->u.pair.cdr) {
                                lisp_val_t *pair = b->u.pair.car;
                                if (pair && pair->type == LISP_PAIR && pair->u.pair.car->type == LISP_SYMBOL) {
                                    lisp_val_t *bound = lisp_eval(pair->u.pair.cdr ? pair->u.pair.cdr->u.pair.car : &nil_val, refresh_global_tail(local_env, env, was_global));
                                    env_set_sym(&local_env, pair->u.pair.car, bound);
                                }
                            }

                            lisp_val_t *last = eval_all_but_last(body, local_env, env, was_global);
                            if (!last) return &nil_val;
                            val = last;
                            env = refresh_global_tail(local_env, env, was_global);
                            goto tail_call;
                        }
                    } else if (op == &sym_lambda || streq(op->u.sym, "lambda")) {
                        /* Special form: lambda -- (lambda (arg...) body-form...) */
                        lisp_val_t *params = (args && args->type == LISP_PAIR) ? args->u.pair.car : &nil_val;
                        lisp_val_t *body = (args && args->type == LISP_PAIR) ? args->u.pair.cdr : &nil_val;
                        lisp_val_t *lam = alloc_node(LISP_LAMBDA);
                        lam->u.lambda.params = params;
                        lam->u.lambda.body = body;
                        lam->u.lambda.env = (env == global_env) ? NULL : env;
                        return lam;
                    }
                    break;

                case 'w':
                    /* Special form: while (S2, plan/phase13_lisp_engine_extensions.md) */
                    if (op == &sym_while || streq(op->u.sym, "while")) {
                        if (args && args->type == LISP_PAIR) {
                            lisp_val_t *cond_expr = args->u.pair.car;
                            lisp_val_t *body = args->u.pair.cdr;
                            for (;;) {
                                if (node_pool_exhausted_warned || lisp_interrupted || lisp_poll_interrupt()) break;
                                lisp_val_t *cond_val = lisp_eval(cond_expr, refresh_global_tail(env, env, was_global));
                                if (!lisp_truthy(cond_val)) break;
                                for (lisp_val_t *c = body; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                                    lisp_eval(c->u.pair.car, refresh_global_tail(env, env, was_global));
                                }
                            }
                        }
                        return &nil_val;
                    }
                    break;

                case 'c':
                    /* Special form: cond -- the last form of the matched clause is a tail position. */
                    if (op == &sym_cond || streq(op->u.sym, "cond")) {
                        for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                            lisp_val_t *clause = c->u.pair.car;
                            if (clause && clause->type == LISP_PAIR) {
                                lisp_val_t *pred = clause->u.pair.car;
                                bool is_else = (pred == &sym_else || (pred->type == LISP_SYMBOL && streq(pred->u.sym, "else")));
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
                    break;

                case 'd':
                    /* Special form: define */
                    if (op == &sym_define || streq(op->u.sym, "define")) {
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
                                env_set_sym(&global_env, name_sym, lam);
                                return name_sym;
                            }

                            if (!target || target->type != LISP_SYMBOL) {
                                printk("[Lisp Error] define: expected a symbol or (name arg...)\n");
                                return &nil_val;
                            }

                            lisp_val_t *eval_val = (rest && rest->type == LISP_PAIR)
                                ? lisp_eval(rest->u.pair.car, env) : &nil_val;
                            env_set_sym(&global_env, target, eval_val);
                            return target;
                        }
                    }
                    break;

                case 's':
                    /* Special form: set! */
                    if (op == &sym_set_bang || streq(op->u.sym, "set!")) {
                        lisp_val_t *name = (args && args->type == LISP_PAIR) ? args->u.pair.car : NULL;
                        lisp_val_t *rest = name ? args->u.pair.cdr : NULL;
                        if (!name || name->type != LISP_SYMBOL || !rest || rest->type != LISP_PAIR) {
                            printk("[Lisp Error] set!: expected (set! name value)\n");
                            return &nil_val;
                        }
                        lisp_val_t *v = lisp_eval(rest->u.pair.car, refresh_global_tail(env, env, was_global));
                        if (node_pool_exhausted_warned || lisp_interrupted) return &nil_val;
                        lisp_val_t *binding = env_binding_sym(refresh_global_tail(env, env, was_global), name);
                        if (!binding) binding = env_binding_sym(global_env, name);
                        if (!binding) {
                            cprintf("set!: unbound variable %s\n", name->u.sym);
                            return &nil_val;
                        }
                        binding->u.pair.cdr = v;
                        return v;
                    }
                    break;

                default:
                    break;
            }
        }



        /* Evaluate Operator. A name is looked up here rather than through
         * lisp_eval() so that an unknown one can say it was meant as a
         * function. */
        lisp_val_t *fn;
        if (op->type == LISP_SYMBOL) {
            fn = env_get_sym(refresh_global_tail(env, env, was_global), op);
            if (!fn) fn = builtin_get(op->u.sym);
            if (!fn) {
                cprintf("Unbound function: %s\n", op->u.sym);
                return &nil_val;
            }
        } else {
            fn = lisp_eval(op, refresh_global_tail(env, env, was_global));
        }
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

        /* Calling what is not a function is an error, before any argument
         * is evaluated. It used to hand back the form itself, unevaluated:
         * `(set! k (+ k 1))`, before set! existed, "worked" and changed
         * nothing, and the while loop around it never ended. */
        if (fn->type != LISP_PRIMITIVE && fn->type != LISP_LAMBDA) {
            cprintf("Not a function: ");
            lisp_print(fn);
            cprintf("\n");
            return &nil_val;
        }

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
                    env_set_sym(&local_env, p->u.pair.car, a->u.pair.car);
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

/* Ctrl-C, asked at most every LISP_INTERRUPT_POLL_US of wall time.
 *
 * By time, not by count of calls: what one call costs ranges from a
 * microsecond to however long the panel takes to scroll a printed line, so
 * any count is either needlessly frequent in QEMU or far too rare on the
 * board -- the old one, a poll every 2^20 calls, was some 15 s of plain
 * arithmetic on the RP2350 and minutes for a loop that printed.
 *
 * It used to have to be rare: console_interrupt_requested() once threw away
 * whatever input it drained, so a poll during a command destroyed the next
 * one a script had already sent. It queues that input now (kernel/
 * console.c, console_pump()), and asking costs a lock and a look at each
 * input device. 50 ms is a delay nobody notices after pressing a key. */
#define LISP_INTERRUPT_POLL_US 50000u

static bool lisp_poll_interrupt(void) {
    uint32_t now = (uint32_t)time_get_us();     /* wraps every 71 min: harmless */
    if (now - interrupt_poll_us < LISP_INTERRUPT_POLL_US) return false;
    interrupt_poll_us = now;
    if (!console_interrupt_requested()) return false;
    printk("[Lisp] interrupted by Ctrl-C\n");
    lisp_interrupted = true;
    console_interrupt_clear();
    return true;
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
 * terms -- see the Ctrl-C poll below. Since 37.3a pool exhaustion no
 * longer bounds it: a form that runs a pool dry is collected in place, with
 * its stack as roots (gc_collect_in_form()); only what it keeps alive is
 * bounded. */
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
        eval_depth_exceeded_warned = false;
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
         * after it (lisp_gc_safepoint(), just called, cleared the latch;
         * it is also what clears it between lsh's commands, where the depth
         * never gets back to 0). */
        eval_poll_count = 0;
    }
    if (lisp_interrupted) {
        return &nil_val;
    }
    uintptr_t stack_lo, stack_hi;
    uintptr_t stack_here = (uintptr_t)__builtin_frame_address(0);
    if (sched_current_stack(&stack_lo, &stack_hi)) {
        if (stack_here < stack_lo + 2048) {
            if (!eval_depth_exceeded_warned) {
                printk("[Lisp Error] Stack limit reached (near stack bottom) -- "
                       "aborting recursion to prevent crash\n");
                eval_depth_exceeded_warned = true;
            }
            return &nil_val;
        }
    }
    if (eval_depth >= LISP_MAX_EVAL_DEPTH) {
        if (!eval_depth_exceeded_warned) {
            printk("[Lisp Error] Maximum evaluation depth (%d) exceeded -- "
                   "aborting to avoid a C stack overflow\n", LISP_MAX_EVAL_DEPTH);
            eval_depth_exceeded_warned = true;
        }
        return &nil_val;
    }
    /* Every 256 calls, the clock; every LISP_INTERRUPT_POLL_US, the console
     * (lisp_poll_interrupt()). */
    if (++eval_poll_count == 0 && lisp_poll_interrupt()) {
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

    /* The shell's line length (512), from scratch rather than this stack:
     * it was 128 bytes here, and a longer line -- one of the runner's own
     * tests -- was cut short and read without its closing parentheses. */
    scratch_t sc = { 0 };
    if (!scratch_acquire(&sc, 512)) {
        cprintf("No memory for the REPL's line\n");
        return;
    }
    char *buf = (char *)sc.base;
    while (1) {
        lisp_canvas_poll();     /* 37.3b: redraw a lost canvas before the prompt */
        /* 37.5a: the shared line editor, with the shell's history -- the
         * same keys, UTF-8, selection and clipboard as the shell itself. */
        int idx = readline_ex("lisp> ", buf, 512, NULL);
        if (idx < 0) continue;

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
    scratch_release(&sc);
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

