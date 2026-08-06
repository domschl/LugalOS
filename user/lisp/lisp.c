#include "lisp.h"
#include "lisp_compile.h"
#include "kernel/printk.h"
#include "kernel/shell.h"
#include "kernel/time.h"
#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "drivers/loopback_net.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "user/chibicc/include/chibicc.h"
#include "arch/elf.h"
#include <string.h>


#if defined(CONFIG_BOARD_RP2350)
#define NODE_POOL_SIZE 512
#else
#define NODE_POOL_SIZE 4096
#endif



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

lisp_val_t *make_str(const char *str) {
    lisp_val_t *v = alloc_node(LISP_STRING);
    strncpy_local(v->u.str, str ? str : "", 128);
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

static const char *get_str_val(lisp_val_t *val) {
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
    static char buf[4096];
    int len = vfs_read(path, buf, sizeof(buf) - 1);
    if (len >= 0) {
        buf[len] = '\0';
        printk("%s\n", buf);
    } else {
        printk("cat: cannot read path '%s'\n", path);
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
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    const char *text = get_str_val(args->u.pair.cdr->u.pair.car);
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
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return &false_val;
    const char *src = get_str_val(args->u.pair.car);
    const char *dst = get_str_val(args->u.pair.cdr->u.pair.car);
    return (vfs_cp(src, dst) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_rm(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    return (vfs_remove(path) == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_cc(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return &false_val;
    char safe_src[128];
    char safe_dst[128];
    const char *src = get_str_val(args->u.pair.car);
    const char *dst = get_str_val(args->u.pair.cdr->u.pair.car);
    strncpy_local(safe_src, src, 127); safe_src[127] = '\0';
    strncpy_local(safe_dst, dst, 127); safe_dst[127] = '\0';
    return (chibicc_compile(safe_src, safe_dst) == 0) ? &true_val : &false_val;
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

static lisp_val_t *prim_ps(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    vfs_ls("/proc/ps");
    return &nil_val;
}

static lisp_val_t *prim_meminfo(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    vfs_ls("/proc/meminfo");
    return &nil_val;
}

static lisp_val_t *prim_version(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    vfs_ls("/proc/version");
    return &nil_val;
}

static lisp_val_t *prim_df(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    vfs_read("/proc/df", NULL, 0);
    return &nil_val;
}

static lisp_val_t *prim_top(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    printk("\n==================================================\n");
    printk("           LugalOS System Monitor                 \n");
    printk("==================================================\n");
    vfs_read("/proc/version", NULL, 0);
    printk("\n[Process States]\n");
    vfs_read("/proc/ps", NULL, 0);
    printk("\n[Memory Status]\n");
    vfs_read("/proc/meminfo", NULL, 0);
    printk("\n[Storage Usage]\n");
    vfs_read("/proc/df", NULL, 0);
    printk("==================================================\n");
    return &nil_val;
}

static lisp_val_t *prim_load(lisp_val_t *args, lisp_val_t *env) {

    (void)env;
    if (!args || args->type != LISP_PAIR) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    static char buf[8192];
    int len = vfs_read(path, buf, sizeof(buf) - 1);
    if (len < 0) {
        printk("load: cannot open file '%s'\n", path);
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
            printk("%s", v->u.str);
        } else {
            lisp_print(v);
        }
    }
    return &nil_val;
}

static lisp_val_t *prim_newline(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    printk("\n");
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
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return &false_val;
    const char *path = get_str_val(args->u.pair.car);
    const char *text = get_str_val(args->u.pair.cdr->u.pair.car);
    int res = vfs_write(path, text, strlen(text));
    return (res == 0) ? &true_val : &false_val;
}

static lisp_val_t *prim_arch(lisp_val_t *args, lisp_val_t *env) {

    (void)args; (void)env;
#if defined(CONFIG_TARGET_RV32)
    return make_str("rv32");
#else
    return make_str("rv64");
#endif
}

static lisp_val_t *prim_mount_ramdisk(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    int size_kb = 512;
    if (args && args->type == LISP_PAIR && args->u.pair.car->type == LISP_INT) {
        size_kb = args->u.pair.car->u.i;
    }
    int res = vfs_mount_ramdisk(size_kb);
    return (res == 0) ? &true_val : &false_val;
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

static lisp_val_t *prim_i2c_scan(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    i2c_scan_bus();
    return &nil_val;
}

static lisp_val_t *prim_lsh(lisp_val_t *args, lisp_val_t *env) {
    (void)args; (void)env;
    shell_run();
    return &nil_val;
}

void lisp_init(void) {
    node_pool_idx = 0;
    global_env = &nil_val;

    env_set(&global_env, "#t", &true_val);
    env_set(&global_env, "#f", &false_val);

    env_set(&global_env, "+", make_prim(prim_add));
    env_set(&global_env, "-", make_prim(prim_sub));
    env_set(&global_env, "*", make_prim(prim_mul));
    env_set(&global_env, "=", make_prim(prim_eq));
    env_set(&global_env, "peek", make_prim(prim_peek));
    env_set(&global_env, "poke", make_prim(prim_poke));
    env_set(&global_env, "ls", make_prim(prim_ls));
    env_set(&global_env, "cat", make_prim(prim_cat));
    env_set(&global_env, "touch", make_prim(prim_touch));
    env_set(&global_env, "write", make_prim(prim_write));
    env_set(&global_env, "mkdir", make_prim(prim_mkdir));
    env_set(&global_env, "rmdir", make_prim(prim_rmdir));
    env_set(&global_env, "cp", make_prim(prim_cp));
    env_set(&global_env, "rm", make_prim(prim_rm));
    env_set(&global_env, "cc", make_prim(prim_cc));
    env_set(&global_env, "exec", make_prim(prim_exec));
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
    env_set(&global_env, "9p-loopback", make_prim(prim_p9_loopback));
    env_set(&global_env, "compile-file", make_prim(prim_compile_file));

    env_set(&global_env, "load", make_prim(prim_load));
    env_set(&global_env, "display", make_prim(prim_display));
    env_set(&global_env, "newline", make_prim(prim_newline));
    env_set(&global_env, "read-file", make_prim(prim_read_file));
    env_set(&global_env, "write-file", make_prim(prim_write_file));

    env_set(&global_env, "arch", make_prim(prim_arch));
    env_set(&global_env, "mount-ramdisk", make_prim(prim_mount_ramdisk));
    env_set(&global_env, "lsh", make_prim(prim_lsh));

    printk("[Lisp Engine] Initialized as Core Microkernel Execution Engine.\n");


    /* Automatically load system boot scripts if present */
    static char boot_buf[8192];
    int len = vfs_read("/sd0/system/stdlib.lisp", boot_buf, sizeof(boot_buf) - 1);
    if (len <= 0) {
        len = vfs_read("/flash0/system/stdlib.lisp", boot_buf, sizeof(boot_buf) - 1);
    }
    if (len > 0) {
        boot_buf[len] = '\0';
        lisp_eval_string(boot_buf);
        printk("[Lisp Boot] Loaded system/stdlib.lisp\n");
    }

    len = vfs_read("/sd0/system/init.lisp", boot_buf, sizeof(boot_buf) - 1);
    if (len <= 0) {
        len = vfs_read("/flash0/system/init.lisp", boot_buf, sizeof(boot_buf) - 1);
    }
    if (len > 0) {
        boot_buf[len] = '\0';
        lisp_eval_string(boot_buf);
        printk("[Lisp Boot] Executed system/init.lisp\n");
    }
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
        case LISP_STRING:
            printk("\"%s\"", val->u.str);
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

    /* Numbers (Hexadecimal and Decimal) */
    if (is_number_token(*str)) {
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

    if (val->type == LISP_INT || val->type == LISP_STRING || val->type == LISP_PRIMITIVE || val->type == LISP_LAMBDA) {
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

        /* Special form: quote */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "quote")) {
            return (args && args->type == LISP_PAIR) ? args->u.pair.car : &nil_val;
        }

        /* Special form: if */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "if")) {
            if (args && args->type == LISP_PAIR && args->u.pair.cdr) {
                lisp_val_t *cond_val = lisp_eval(args->u.pair.car, env);
                bool is_true = (cond_val != &false_val) &&
                               !(cond_val->type == LISP_SYMBOL && streq(cond_val->u.sym, "#f")) &&
                               !(cond_val->type == LISP_NIL);
                if (is_true) {
                    return lisp_eval(args->u.pair.cdr->u.pair.car, env);
                } else if (args->u.pair.cdr->u.pair.cdr) {
                    return lisp_eval(args->u.pair.cdr->u.pair.cdr->u.pair.car, env);
                }
            }
            return &nil_val;
        }

        /* Special form: begin */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "begin")) {
            lisp_val_t *res = &nil_val;
            for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                res = lisp_eval(c->u.pair.car, env);
            }
            return res;
        }

        /* Special form: let */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "let")) {
            if (args && args->type == LISP_PAIR) {
                lisp_val_t *bindings = args->u.pair.car;
                lisp_val_t *body = args->u.pair.cdr;
                lisp_val_t *local_env = env;

                for (lisp_val_t *b = bindings; b && b->type == LISP_PAIR; b = b->u.pair.cdr) {
                    lisp_val_t *pair = b->u.pair.car;
                    if (pair && pair->type == LISP_PAIR && pair->u.pair.car->type == LISP_SYMBOL) {
                        lisp_val_t *val = lisp_eval(pair->u.pair.cdr ? pair->u.pair.cdr->u.pair.car : &nil_val, env);
                        env_set(&local_env, pair->u.pair.car->u.sym, val);
                    }
                }

                lisp_val_t *res = &nil_val;
                for (lisp_val_t *c = body; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                    res = lisp_eval(c->u.pair.car, local_env);
                }
                return res;
            }
        }

        /* Special form: cond */
        if (op->type == LISP_SYMBOL && streq(op->u.sym, "cond")) {
            for (lisp_val_t *c = args; c && c->type == LISP_PAIR; c = c->u.pair.cdr) {
                lisp_val_t *clause = c->u.pair.car;
                if (clause && clause->type == LISP_PAIR) {
                    lisp_val_t *pred = clause->u.pair.car;
                    bool is_else = (pred->type == LISP_SYMBOL && streq(pred->u.sym, "else"));
                    lisp_val_t *pval = is_else ? &true_val : lisp_eval(pred, env);
                    bool is_true = (pval != &false_val) && !(pval->type == LISP_SYMBOL && streq(pval->u.sym, "#f")) && !(pval->type == LISP_NIL);
                    if (is_true) {
                        lisp_val_t *res = &nil_val;
                        for (lisp_val_t *expr = clause->u.pair.cdr; expr && expr->type == LISP_PAIR; expr = expr->u.pair.cdr) {
                            res = lisp_eval(expr->u.pair.car, env);
                        }
                        return res;
                    }
                }
            }
            return &nil_val;
        }

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

