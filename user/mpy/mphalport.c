#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"
#include "usys.h"

#include <stdint.h>
#include <stdlib.h>

const char lugal_mpy_help_text[] =
    "Welcome to MicroPython on LugalOS!\n"
    "For online docs please visit http://docs.micropython.org/\n"
    "Control commands:\n"
    "  CTRL-D        -- exit\n";

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

int mp_hal_stdin_rx_chr(void) {
    /* Non-interactive EOF until console input channel is wired. */
    return 4;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uputchar(str[i]);
    }
    return (mp_uint_t)len;
}

void mp_hal_delay_ms(mp_uint_t ms) {
    long start = utime_ms();
    while ((unsigned long)(utime_ms() - start) < (unsigned long)ms) {
        uyield();
    }
}

void mp_hal_delay_us(mp_uint_t us) {
    udelay_us((long)us);
}

mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)utime_ms();
}

mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)(utime_ms() * 1000L);
}

mp_uint_t mp_hal_ticks_cpu(void) {
    return (mp_uint_t)uticks();
}

uint64_t mp_hal_time_ns(void) {
    return (uint64_t)(unsigned long)utime_ms() * 1000000ULL;
}

void lugal_resolve_path(const char *in, char *out, size_t out_max);

mp_import_stat_t mp_import_stat(const char *raw_path) {
    char path[256];
    lugal_resolve_path(raw_path, path, sizeof(path));
    lugal_stat_t st;
    if (ustat(path, &st) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    return st.is_dir ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

typedef struct {
    int fd;
    size_t pos;
    size_t len;
    uint8_t buf[256];
} lugal_file_reader_t;

static mp_uint_t lugal_reader_readbyte(void *data) {
    lugal_file_reader_t *r = (lugal_file_reader_t *)data;
    if (r->pos >= r->len) {
        long n = uread(r->fd, r->buf, sizeof(r->buf));
        if (n <= 0) {
            return MP_READER_EOF;
        }
        r->pos = 0;
        r->len = (size_t)n;
    }
    return r->buf[r->pos++];
}

static void lugal_reader_close(void *data) {
    lugal_file_reader_t *r = (lugal_file_reader_t *)data;
    uclose(r->fd);
    m_del_obj(lugal_file_reader_t, r);
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    char path[256];
    lugal_resolve_path(qstr_str(filename), path, sizeof(path));
    int fd = uopen(path, LUGAL_O_READ);
    if (fd < 0) {
        mp_raise_OSError(2); /* ENOENT */
    }
    lugal_file_reader_t *r = m_new_obj(lugal_file_reader_t);
    r->fd = fd;
    r->pos = 0;
    r->len = 0;
    mp_reader_t reader = { r, lugal_reader_readbyte, lugal_reader_close };
    return mp_lexer_new(filename, reader);
}

void nlr_jump_fail(void *val) {
    (void)val;
    uprint("FATAL: uncaught NLR\n");
    uexit(1);
    for (;;) {}
}

/* Soft-float helpers expected by MicroPython's libm under -fno-builtin. */
float fabsf(float x) {
    union { float f; uint32_t u; } v = { x };
    v.u &= 0x7fffffffu;
    return v.f;
}

float copysignf(float x, float y) {
    union { float f; uint32_t u; } vx = { x }, vy = { y };
    vx.u = (vx.u & 0x7fffffffu) | (vy.u & 0x80000000u);
    return vx.f;
}

float nanf(const char *tagp) {
    (void)tagp;
    union { float f; uint32_t u; } v;
    v.u = 0x7fc00000u;
    return v.f;
}
