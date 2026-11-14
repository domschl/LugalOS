#include "py/compile.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/runtime/pyexec.h"

#include <stdio.h>
#include <string.h>

#define MPY_GC_HEAP_BYTES (96u * 1024u)

static uint8_t g_gc_heap[MPY_GC_HEAP_BYTES] __attribute__((aligned(16)));

static void init_sys_paths_and_argv(int argc, char **argv) {
#if MICROPY_PY_SYS_PATH
    mp_obj_list_t *path = MP_OBJ_TO_PTR(mp_sys_path);
    mp_obj_list_append(MP_OBJ_FROM_PTR(path), MP_OBJ_NEW_QSTR(MP_QSTR__slash_sd0_slash_lib_slash_python));
    mp_obj_list_append(MP_OBJ_FROM_PTR(path), MP_OBJ_NEW_QSTR(MP_QSTR__slash_flash0_slash_lib_slash_python));
#endif
#if MICROPY_PY_SYS_ARGV
    mp_obj_list_t *argv_list = MP_OBJ_TO_PTR(mp_sys_argv);
    for (int i = 0; i < argc; i++) {
        mp_obj_list_append(MP_OBJ_FROM_PTR(argv_list), mp_obj_new_str(argv[i], strlen(argv[i])));
    }
#else
    (void)argc;
    (void)argv;
#endif
}

static int run_str(const char *src) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&pt, source_name, false);
        mp_call_function_0(module_fun);
        nlr_pop();
        return 0;
    } else {
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        return 1;
    }
}

int main(int argc, char **argv) {
    int stack_dummy;
    mp_stack_set_top(&stack_dummy);
    mp_stack_set_limit(24 * 1024);

    gc_init(g_gc_heap, g_gc_heap + sizeof(g_gc_heap));
    mp_init();
    init_sys_paths_and_argv(argc, argv);

    int rc = 0;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        rc = run_str(argv[2]);
    } else if (argc >= 2 && argv[1][0] != '-') {
        int ok = pyexec_file(argv[1]);
        rc = ok ? 0 : 1;
    } else if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: mpy [-c \"code\" | script.py [args...]]\n");
    } else {
        pyexec_friendly_repl();
    }

    gc_sweep_all();
    mp_deinit();
    return rc;
}
