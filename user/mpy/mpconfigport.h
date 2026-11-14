#ifndef LUGALOS_MPCONFIGPORT_H
#define LUGALOS_MPCONFIGPORT_H

#include <stdint.h>

/* Core feature level with selected MIT extmod modules, no FPU. */
#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_GC_ALLOC_THRESHOLD              (0)
#define MICROPY_HELPER_REPL                     (1)
#define MICROPY_REPL_AUTO_INDENT                (1)
#define MICROPY_ENABLE_SOURCE_LINE              (1)
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_OPT_COMPUTED_GOTO               (0)
#define MICROPY_ROM_TEXT_COMPRESSION            (0)

/* Kernel boots with mstatus.FS = 0; avoid float/libgcc soft-float in base port. */
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_NONE)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)

/* Portable integer-only setjmp/longjmp NLR and GC register scanning. */
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_GCREGS_SETJMP                   (1)

#define MICROPY_USE_INTERNAL_PRINTF             (0)
#define MICROPY_USE_INTERNAL_ERRNO              (1)
#define MICROPY_ALLOC_PATH_MAX                  (256)

#define MICROPY_PY_BUILTINS_HELP                (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT           lugal_mpy_help_text
#define MICROPY_PY_BUILTINS_INPUT               (1)
#define MICROPY_PY_BUILTINS_EXECFILE            (1)
#define MICROPY_PY_IO                           (1)
#define MICROPY_PY_GC                           (1)
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_EXIT                     (1)
#define MICROPY_PY_SYS_PATH                     (1)
#define MICROPY_PY_SYS_ARGV                     (1)
#define MICROPY_PY_SYS_PLATFORM                 "lugalos"
#define MICROPY_HW_BOARD_NAME                   "LugalOS"
#define MICROPY_HW_MCU_NAME                     "RISC-V"
#define MICROPY_BANNER_MACHINE                  "LugalOS " MICROPY_HW_MCU_NAME

/* Selected MIT extmod modules. */
#define MICROPY_PY_JSON                         (1)
#define MICROPY_PY_RE                           (0)
#define MICROPY_PY_BINASCII                     (1)
#define MICROPY_PY_HEAPQ                        (1)
#define MICROPY_PY_RANDOM                       (1)
#define MICROPY_PY_TIME                         (1)
#define MICROPY_PY_TIME_TIME_NS                 (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)

/* Minimal lexer file reader backed by LugalOS U-mode VFS syscalls. */
#define MICROPY_READER_POSIX                    (0)
#define MICROPY_VFS                             (0)

typedef long mp_int_t;
typedef unsigned long mp_uint_t;
typedef long mp_off_t;

#define MP_STATE_PORT MP_STATE_VM

#define MICROPY_PORT_ROOT_POINTERS \
    const char *readline_hist[8];

#include <alloca.h>

#endif /* LUGALOS_MPCONFIGPORT_H */
