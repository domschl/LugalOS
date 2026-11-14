#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ctype.h>
#include "liblugal.h"

/* Large .rodata array to guarantee this ELF spans multiple R|X pages (> 4096 B)
 * and exercises linker/user_large.ld + arch/riscv/common/elf.c multi-page loading. */
static const uint32_t g_rodata_pad[1200] = { 0x12345678u, 0xdeadbeefu };

static int cmp_int(const void *a, const void *b) {
    int va = *(const int *)a;
    int vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (g_rodata_pad[0] != 0x12345678u || g_rodata_pad[1] != 0xdeadbeefu) {
        printf("ULIBC_RODATA_FAIL\n");
        return 1;
    }

    /* 1. malloc / realloc / free */
    char *p1 = (char *)malloc(128);
    char *p2 = (char *)calloc(16, 16);
    if (!p1 || !p2) {
        printf("ULIBC_ALLOC_FAIL\n");
        return 2;
    }
    snprintf(p1, 128, "fmt=%d hex=0x%x str=%s", -42, 0xabc, "ok");
    if (strcmp(p1, "fmt=-42 hex=0xabc str=ok") != 0) {
        printf("ULIBC_FMT_FAIL: %s\n", p1);
        return 3;
    }
    p1 = (char *)realloc(p1, 512);
    if (!p1 || strcmp(p1, "fmt=-42 hex=0xabc str=ok") != 0) {
        printf("ULIBC_REALLOC_FAIL\n");
        return 4;
    }
    free(p1);
    free(p2);

    /* 2. setjmp / longjmp */
    jmp_buf env;
    volatile int jumped = 0;
    int rc = setjmp(env);
    if (rc == 0) {
        jumped = 1;
        longjmp(env, 42);
    }
    if (rc != 42 || !jumped) {
        printf("ULIBC_JMP_FAIL\n");
        return 5;
    }

    /* 3. qsort / bsearch / strtol */
    int arr[5] = { 50, 10, 40, 20, 30 };
    qsort(arr, 5, sizeof(int), cmp_int);
    int key = 30;
    int *found = (int *)bsearch(&key, arr, 5, sizeof(int), cmp_int);
    if (!found || *found != 30 || arr[0] != 10 || arr[4] != 50) {
        printf("ULIBC_SORT_FAIL\n");
        return 6;
    }
    if (strtol("  -0x1f", NULL, 0) != -31 || toupper('a') != 'A') {
        printf("ULIBC_STRTOL_FAIL\n");
        return 7;
    }

    /* 4. VFS syscall via uopen/uread/uclose */
    int fd = uopen("/proc/version", LUGAL_O_READ);
    if (fd < 0) {
        printf("ULIBC_VFS_FAIL\n");
        return 8;
    }
    char vbuf[64];
    long nr = uread(fd, vbuf, sizeof(vbuf) - 1u);
    uclose(fd);
    if (nr <= 0) {
        printf("ULIBC_VFS_READ_FAIL\n");
        return 9;
    }

    printf("ULIBC_OK\n");
    return 0;
}
