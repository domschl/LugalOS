#include "lisp.h"
#include "lisp_compile.h"
#include "kernel/printk.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include <string.h>

#define NODE_POOL_SIZE 512

static lisp_val_t node_pool[NODE_POOL_SIZE];
static int node_pool_idx = 0;

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
    if (node_pool_idx >= NODE_POOL_SIZE) {
        printk("[Lisp Error] Node pool exhausted! Resetting heap.\n");
        node_pool_idx = 0;
    }
    lisp_val_t *v = &node_pool[node_pool_idx++];
    v->type = type;
    return v;
}

lisp_val_t *make_int(long val) {
    lisp_val_t *v = alloc_node(LISP_INT);
    v->u.i = val;
    return v;
}

lisp_val_t *make_sym(const char *sym) {
    lisp_val_t *v = alloc_node(LISP_SYMBOL);
    strncpy_local(v->u.sym, sym, 32);
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

/* Built-in primitives */
static lisp_val_t *prim_add(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    long sum = 0;
    for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
        if (c->u.pair.car->type == LISP_INT) {
            sum += c->u.pair.car->u.i;
        }
    }
    return make_int(sum);
}

static lisp_val_t *prim_sub(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    long res = args->u.pair.car->u.i;
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

static lisp_val_t *prim_eq(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return &false_val;
    lisp_val_t *a1 = args->u.pair.car;
    lisp_val_t *a2 = args->u.pair.cdr->u.pair.car;
    if (a1->type == LISP_INT && a2->type == LISP_INT) {
        return (a1->u.i == a2->u.i) ? &true_val : &false_val;
    }
    return &false_val;
}

static lisp_val_t *prim_peek(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return make_int(0);
    uintptr_t addr = (uintptr_t)args->u.pair.car->u.i;
    uint32_t val = *(volatile uint32_t *)addr;
    return make_int((long)val);
}

static lisp_val_t *prim_poke(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return &false_val;
    uintptr_t addr = (uintptr_t)args->u.pair.car->u.i;
    uint32_t val = (uint32_t)args->u.pair.cdr->u.pair.car->u.i;
    *(volatile uint32_t *)addr = val;
    return &true_val;
}

static lisp_val_t *prim_cat(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR || args->u.pair.car->type != LISP_SYMBOL) return &nil_val;
    char buf[512];
    buf[0] = '\0';
    vfs_read(args->u.pair.car->u.sym, buf, 512);
    return &nil_val;
}

void lisp_init(void) {
    node_pool_idx = 0;
    global_env = &nil_val;

    env_set(&global_env, "+", make_prim(prim_add));
    env_set(&global_env, "-", make_prim(prim_sub));
    env_set(&global_env, "*", make_prim(prim_mul));
    env_set(&global_env, "=", make_prim(prim_eq));
    env_set(&global_env, "peek", make_prim(prim_peek));
    env_set(&global_env, "poke", make_prim(prim_poke));
    env_set(&global_env, "cat", make_prim(prim_cat));
    env_set(&global_env, "compile-file", make_prim(prim_compile_file));

    printk("[Lisp] Scheme / S-Expression REPL Engine initialized.\n");
}

/* Printer */
void lisp_print(lisp_val_t *val) {
    if (!val || val->type == LISP_NIL) {
        printk("()");
        return;
    }
    switch (val->type) {
        case LISP_INT:
            printk("%ld", val->u.i);
            break;
        case LISP_SYMBOL:
            printk("%s", val->u.sym);
            break;
        case LISP_PRIMITIVE:
            printk("<#primitive>");
            break;
        case LISP_LAMBDA:
            printk("<#closure>");
            break;
        case LISP_PAIR:
            printk("(");
            lisp_print(val->u.pair.car);
            for (lisp_val_t *c = val->u.pair.cdr; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                printk(" ");
                lisp_print(c->u.pair.car);
            }
            printk(")");
            break;
        default:
            printk("?");
            break;
    }
}

/* Lexer / Parser */
static void skip_whitespace(const char **str) {
    while (**str == ' ' || **str == '\t' || **str == '\r' || **str == '\n') {
        (*str)++;
    }
}

lisp_val_t *lisp_read(const char **str) {
    skip_whitespace(str);
    if (**str == '\0') return NULL;

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

    /* Hexadecimal Numbers */
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
    }

    /* Decimal Numbers */
    if ((**str >= '0' && **str <= '9') || (**str == '-' && (*str)[1] >= '0' && (*str)[1] <= '9')) {
        long val = 0;
        int sign = 1;
        if (**str == '-') {
            sign = -1;
            (*str)++;
        }
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

/* Evaluator */
lisp_val_t *lisp_eval(lisp_val_t *val, lisp_val_t *env) {
    if (!val) return &nil_val;

    if (val->type == LISP_INT || val->type == LISP_PRIMITIVE || val->type == LISP_LAMBDA) {
        return val;
    }

    if (val->type == LISP_SYMBOL) {
        lisp_val_t *res = env_get(env, val->u.sym);
        if (res) return res;
        printk("Unbound symbol: %s\n", val->u.sym);
        return &nil_val;
    }

    if (val->type == LISP_PAIR) {
        lisp_val_t *op = val->u.pair.car;
        lisp_val_t *args = val->u.pair.cdr;

        /* Special form: define */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "define")) {
            if (args && args->type == LISP_PAIR) {
                lisp_val_t *sym = args->u.pair.car;
                lisp_val_t *eval_val = lisp_eval(args->u.pair.cdr->u.pair.car, env);
                env_set(&global_env, sym->u.sym, eval_val);
                return sym;
            }
        }

        /* Special form: lambda */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "lambda")) {
            lisp_val_t *params = args ? args->u.pair.car : &nil_val;
            lisp_val_t *body = (args && args->u.pair.cdr) ? args->u.pair.cdr->u.pair.car : &nil_val;
            lisp_val_t *lam = alloc_node(LISP_LAMBDA);
            lam->u.lambda.params = params;
            lam->u.lambda.body = body;
            lam->u.lambda.env = env;
            return lam;
        }

        /* Evaluate Operator */
        lisp_val_t *fn = lisp_eval(op, env);
        if (!fn) return &nil_val;

        if (fn->type == LISP_PRIMITIVE && fn->u.prim == prim_compile_file) {
            return fn->u.prim(args, env);
        }

        /* Evaluate Arguments */
        lisp_val_t *eval_args_head = &nil_val;
        lisp_val_t *eval_args_tail = NULL;

        for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
            lisp_val_t *ev = lisp_eval(c->u.pair.car, env);
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
            return fn->u.prim(eval_args_head, env);
        }

        if (fn->type == LISP_LAMBDA) {
            /* Create new scope extending lambda environment */
            lisp_val_t *local_env = fn->u.lambda.env;
            lisp_val_t *p = fn->u.lambda.params;
            lisp_val_t *a = eval_args_head;
            while (p && p->type == LISP_PAIR && a && a->type == LISP_PAIR) {
                if (p->u.pair.car->type == LISP_SYMBOL) {
                    env_set(&local_env, p->u.pair.car->u.sym, a->u.pair.car);
                }
                p = p->u.pair.cdr;
                a = a->u.pair.cdr;
            }
            return lisp_eval(fn->u.lambda.body, local_env);
        }
    }

    return val;
}

void lisp_repl(void) {
    printk("\n==================================================\n");
    printk("       LugalOS Scheme / S-Expression REPL         \n");
    printk("  Type expressions like (+ 10 20) or (cat /proc/ps)\n");
    printk("  Type 'exit' to return to lugal shell.            \n");
    printk("==================================================\n");

    char buf[128];
    while (1) {
        printk("lisp> ");
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
                }
            }
        }

        if (streq(buf, "exit")) break;
        if (idx == 0) continue;

        const char *ptr = buf;
        lisp_val_t *ast = lisp_read(&ptr);
        lisp_val_t *result = lisp_eval(ast, global_env);
        printk("=> ");
        lisp_print(result);
        printk("\n");
    }
}
