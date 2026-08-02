#ifndef LUGALOS_USER_LISP_COMPILE_H
#define LUGALOS_USER_LISP_COMPILE_H

#include "lisp.h"

int lisp_compile_file(const char *src_path, const char *dst_elf_path);
lisp_val_t *prim_compile_file(lisp_val_t *args, lisp_val_t *env);

#endif /* LUGALOS_USER_LISP_COMPILE_H */
