#include "kernel/hart.h"

/* The records themselves. In .bss, which is why SETUP_HART_POINTER must run
 * after the boot path's clear loop -- see the macro's own comment.
 *
 * Not static: arch/riscv/common/entry.S and arch/riscv/rp2350/boot_header.S
 * both take its address by name. */
hart_t g_harts[MAX_HARTS];

/* The offsets in kernel/hart.h are what the trap vector actually indexes
 * with. Nothing else checks that they still describe this struct, and a
 * silent mismatch would not be a wrong number -- it would be the trap path
 * loading a stack pointer out of the wrong word. */
_Static_assert(sizeof(hart_t) == HART_SIZE, "hart_t no longer matches HART_SIZE");
_Static_assert(__builtin_offsetof(hart_t, kernel_sp) == HART_OFF_KERNEL_SP, "HART_OFF_KERNEL_SP");
_Static_assert(__builtin_offsetof(hart_t, id) == HART_OFF_ID, "HART_OFF_ID");
_Static_assert(__builtin_offsetof(hart_t, tmp) == HART_OFF_TMP, "HART_OFF_TMP");
