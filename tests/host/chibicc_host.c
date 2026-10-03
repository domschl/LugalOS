/*
 * The in-kernel C compiler (user/chibicc) on the host, under ASan/UBSan or
 * valgrind (phase 40 review).
 *
 * `cc` compiles whatever file it is pointed at, inside the kernel, so no
 * input may make it touch memory it does not own or never return. This
 * compiles the fixtures shipped on the SD image (each must succeed), then
 * mutated copies of them -- bytes changed, tokens inserted, spans deleted or
 * repeated, the text cut short -- and a few hand-made shapes that probe
 * limits: deep nesting, long lines, many declarations. A compile may fail;
 * it may not fault, and it must come back.
 *
 * Usage: chibicc_host [iterations [seed]]. Fixtures are read from
 * ../../tools/sd_root, or from $LUGALOS_ROOT/tools/sd_root.
 */

#include "chibicc.h"
#include "shim.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int chibicc_compile(const char *src_path, const char *dst_elf_path);

static const char *g_fixtures[] = { "hello.c", "fib.c", "prime.c", "cat.c", "multi.c" };
#define NFIX (sizeof(g_fixtures) / sizeof(g_fixtures[0]))
static char *g_fix_text[NFIX];
static uint32_t g_fix_len[NFIX];

static char *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[n] = '\0';
    *len = (uint32_t)n;
    return b;
}

static void load_fixtures(void) {
    const char *root = getenv("LUGALOS_ROOT");
    char path[512];
    for (unsigned i = 0; i < NFIX; i++) {
        snprintf(path, sizeof(path), "%s/tools/sd_root/%s", root ? root : "../..", g_fixtures[i]);
        g_fix_text[i] = slurp(path, &g_fix_len[i]);
        if (!g_fix_text[i]) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    }
    uint32_t n;
    snprintf(path, sizeof(path), "%s/tools/sd_root/multi.h", root ? root : "../..");
    char *h = slurp(path, &n);
    if (!h) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    host_file_put("/ram0/multi.h", h, n);
    free(h);
}

static int compile_text(const char *text, uint32_t len) {
    host_file_put("/ram0/t.c", text, len);
    return chibicc_compile("/ram0/t.c", "/ram0/t.elf");
}

static volatile sig_atomic_t g_iter = -1;
static uint64_t g_seed0;

static void on_alarm(int sig) {
    (void)sig;
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "FAIL: compile did not return, input %d (seed %llu)\n",
                     (int)g_iter, (unsigned long long)g_seed0);
    (void)!write(2, msg, (size_t)n);
    _exit(3);
}

static const char *g_tokens[] = {
    "(", ")", "{", "}", "[", "]", ";", ",", "*", "&", "-", "+", "=", "==", "<", "->", ".",
    "int ", "char ", "long ", "void ", "struct ", "if ", "else ", "while ", "for ", "return ",
    "sizeof ", "\"", "'", "/*", "*/", "//", "\n#define X 1\n", "\n#include \"multi.h\"\n",
    "\n#ifdef X\n", "\n#endif\n", "0x", "99999999999999999999", "x", "main", "\\", "\n",
};
#define NTOK (sizeof(g_tokens) / sizeof(g_tokens[0]))

static uint32_t mutate(char *dst, uint32_t cap, const char *src, uint32_t len, uint64_t *s) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    int n = 1 + (int)(host_rand(s) % 8u);
    for (int m = 0; m < n && len > 0; m++) {
        uint32_t at = host_rand(s) % len;
        switch (host_rand(s) % 6u) {
            case 0: dst[at] = (char)(host_rand(s) & 0x7f); break;
            case 1: {                                           /* insert a token */
                const char *t = g_tokens[host_rand(s) % NTOK];
                uint32_t tl = (uint32_t)strlen(t);
                if (len + tl >= cap) break;
                memmove(dst + at + tl, dst + at, len - at);
                memcpy(dst + at, t, tl);
                len += tl;
                break;
            }
            case 2: {                                           /* delete a span */
                uint32_t dl = host_rand(s) % 40u;
                if (at + dl > len) dl = len - at;
                memmove(dst + at, dst + at + dl, len - at - dl);
                len -= dl;
                break;
            }
            case 3: {                                           /* repeat a span */
                uint32_t dl = host_rand(s) % 60u;
                if (at + dl > len) dl = len - at;
                if (len + dl >= cap) break;
                memmove(dst + at + dl, dst + at, len - at);
                len += dl;
                break;
            }
            case 4: len = at; break;                            /* cut short */
            default: dst[at] = "(){};*\"'\n"[host_rand(s) % 9u]; break;
        }
    }
    dst[len] = '\0';
    return len;
}

/* Shapes that probe limits rather than syntax. */
static void limits(void) {
    static char buf[1 << 16];
    struct { const char *name; int depth; const char *open, *mid, *close; } nest[] = {
        { "parentheses", 2000, "(", "1", ")" },
        { "unclosed parentheses", 2000, "(", "1", "" },
        { "unary minus", 4000, "-", "1", "" },
        { "blocks", 2000, "{", "", "}" },
        { "ifs", 1000, "if (1) ", "return 1;", "" },
        { "pointers", 1000, "*", "p", "" },
    };
    for (unsigned i = 0; i < sizeof(nest) / sizeof(nest[0]); i++) {
        size_t o = 0;
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "main() { int *p; return\n");
        for (int d = 0; d < nest[i].depth && o < sizeof(buf) - 200; d++) {
            o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s", nest[i].open);
            if (d % 50 == 49) buf[o++] = '\n';
        }
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "\n%s\n", nest[i].mid);
        for (int d = 0; d < nest[i].depth && *nest[i].close && o < sizeof(buf) - 200; d++) {
            o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s", nest[i].close);
            if (d % 50 == 49) buf[o++] = '\n';
        }
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, ";\n}\n");
        alarm(10);
        int r = compile_text(buf, (uint32_t)o);
        alarm(0);
        printf("chibicc_host: %d-deep %s -> %s\n", nest[i].depth, nest[i].name, r == 0 ? "compiled" : "refused");
    }
    /* A small source with a lot of code: twenty `x = x+x+...+x;` lines are
     * under 500 tokens and about 5 KB of machine code, past the 4 KB output
     * buffer codegen once ignored -- it wrote on into the kernel heap. */
    {
        size_t o = (size_t)snprintf(buf, sizeof(buf), "main() { int x; x = 1;\n");
        for (int i = 0; i < 20; i++)
            o += (size_t)snprintf(buf + o, sizeof(buf) - o, "x = x+x+x+x+x+x+x+x+x+x;\n");
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "return x; }\n");
        alarm(10);
        int r = compile_text(buf, (uint32_t)o);
        alarm(0);
        printf("chibicc_host: %zu-byte source, ~5 KB of code -> %s\n", o, r == 0 ? "compiled" : "refused");
    }
    /* Many small functions and variables: the pools' limits. */
    size_t o = 0;
    for (int f = 0; f < 400 && o < sizeof(buf) - 200; f++)
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "f%d(a, b) { int x%d; x%d = a + b; return x%d; }\n", f, f, f, f);
    o += (size_t)snprintf(buf + o, sizeof(buf) - o, "main() { return f1(1, 2); }\n");
    alarm(10);
    int r = compile_text(buf, (uint32_t)o);
    alarm(0);
    printf("chibicc_host: 400 functions, %zu bytes -> %s\n", o, r == 0 ? "compiled" : "refused");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* in order with sanitizer reports on stderr */
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    signal(SIGALRM, on_alarm);
    g_seed0 = seed;
    load_fixtures();

    int failures = 0;
    for (unsigned i = 0; i < NFIX; i++) {
        alarm(10);
        int r = compile_text(g_fix_text[i], g_fix_len[i]);
        alarm(0);
        if (r != 0) { fprintf(stderr, "FAIL: fixture %s did not compile\n", g_fixtures[i]); failures++; }
    }
    printf("chibicc_host: %u fixtures compiled%s\n", (unsigned)NFIX, failures ? " -- with FAILURES" : "");
    if (failures) return 1;

    limits();

    static char buf[16384];
    uint64_t s = seed | 1;
    int ok = 0;
    for (int it = 0; it < iterations; it++) {
        g_iter = it;
        unsigned f = host_rand(&s) % NFIX;
        uint32_t len = mutate(buf, sizeof(buf), g_fix_text[f], g_fix_len[f], &s);
        alarm(10);
        if (compile_text(buf, len) == 0) ok++;
        alarm(0);
    }
    printf("chibicc_host: %d mutated programs (seed %llu), %d compiled, no fault\n",
           iterations, (unsigned long long)seed, ok);
    host_files_clear();
    return 0;
}
