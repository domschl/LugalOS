/* user/zmachine/host_main.c — host test harness for the LugalOS Z-machine
 *
 * Builds zork-host: loads a story file, reports the header, then runs
 * the Version 3 interpreter (zvm.c) until it stops.
 *
 * Build:  make
 * Run:    ./zork-host [options] [story.zip]
 *   -n N      stop after at most N instructions (default: no limit)
 *   -t        trace every instruction (pc + opcode)
 *   -q        quiet: suppress the header report
 *
 * M2 acceptance: zork1.zip runs from the initial PC through its boot
 * setup to the first aread() prompt without errors.  Text output shows
 * as [zstr $addr] markers until the M3 text backend lands.
 */
#include "zvm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* The Z-machine address space is 18 bits; reserve it all. */
#define Z_HOST_MEM_SIZE 0x20000u

/* ---------------------------------------------------------------------- */
/* Host I/O adapters (libc lives ONLY in this file)                       */
/* ---------------------------------------------------------------------- */

static void host_putc(void *user, uint8_t ch)
{
    (void)user;
    fputc((int)ch, stdout);
    fflush(stdout);
}

/* Interactive input: the INTERPRETER echoes typed characters (per the
 * Z-machine spec, and implemented in zvm_aread), so the terminal must
 * not echo as well.  Switch stdin to raw mode on first input -- only
 * when stdin is actually a terminal (piped test input has no echo to
 * duplicate, and must not be modified).  Mirrors frotz's behaviour. */
static struct termios saved_tc;
static int tc_saved;

static void restore_tc(void)
{
    if (tc_saved != 0) {
        tcsetattr(0, TCSAFLUSH, &saved_tc);
    }
}

/* v3 SAVE/RESTORE: fixed save file next to the story ("<story>.lzs").
 * The original v3 convention lets the interpreter pick the auxiliary
 * storage; frotz prompts for a filename, we keep it scriptable. */
static char save_path[1024];

static int host_save(void *user, const uint8_t *data, uint32_t len)
{
    FILE *f;
    size_t put;
    (void)user;
    f = fopen(save_path, "wb");
    if (f == NULL) {
        return -1;
    }
    put = fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return (put == (size_t)len) ? 0 : -1;
}

static int host_restore(void *user, uint8_t *data, uint32_t cap,
                        uint32_t *len_out)
{
    FILE *f;
    long size;
    size_t got;
    (void)user;
    f = fopen(save_path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0
        || (uint32_t)size > cap) {
        fclose(f);
        return -1;
    }
    rewind(f);
    got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        return -1;
    }
    *len_out = (uint32_t)size;
    return 0;
}

static int host_getc(void *user)
{
    int c;
    (void)user;
    fflush(stdout);
    if (tc_saved == 0 && isatty(0) != 0) {
        if (tcgetattr(0, &saved_tc) == 0) {
            struct termios raw = saved_tc;
            tc_saved = 1;
            /* keep ISIG (Ctrl-C) and OPOST (\n -> \r\n for echo) */
            raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(0, TCSAFLUSH, &raw);
            atexit(restore_tc);
        }
    }
    c = fgetc(stdin);
    return (c == EOF) ? -1 : c;
}

/* ---------------------------------------------------------------------- */
/* File loading                                                           */
/* ---------------------------------------------------------------------- */

static int read_whole_file(const char *path, uint8_t **out, uint32_t *out_len)
{
    FILE *f;
    long size;
    uint8_t *buf;
    size_t got;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "%s: open failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fprintf(stderr, "%s: seek failed: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }
    rewind(f);

    buf = malloc((size_t)size);
    if (buf == NULL) {
        fprintf(stderr, "%s: out of memory (%ld bytes)\n", path, size);
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size) {
        fprintf(stderr, "%s: short read (%zu of %ld bytes)\n", path, got, size);
        free(buf);
        return -1;
    }

    *out = buf;
    *out_len = (uint32_t)size;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Opcode naming (for the trace display; tables per zmach06e §7.2)        */
/* ---------------------------------------------------------------------- */

static const char *const names_2op[32] = {
    "<2OP:$0>",
    "je", "jl", "jg",
    "dec_chk", "inc_chk",
    "jin", "test",
    "or", "and",
    "test_attr", "set_attr", "clear_attr",
    "store", "insert_obj",
    "loadw", "loadb",
    "get_prop", "get_prop_addr", "get_next_prop",
    "add", "sub", "mul", "div", "mod",
    "call_2s (v4+)", "<2OP:$1A>", "<2OP:$1B>",
    "<2OP:$1C>", "<2OP:$1D>", "<2OP:$1E>", "<2OP:$1F>"
};

static const char *const names_1op[16] = {
    "jz", "get_sibling", "get_child", "get_parent",
    "get_prop_len", "inc", "dec", "print_addr",
    "call_1s (v4+)",
    "remove_obj", "print_obj", "ret", "jump",
    "print_paddr", "load", "not (v1/v4)"
};

static const char *const names_0op[16] = {
    "rtrue", "rfalse", "print", "print_ret",
    "nop",
    "save (v1/v4)", "restore (v1/v4)",
    "restart", "ret_popped",
    "pop (v1)",
    "quit", "new_line",
    "show_status (v3)", "verify",
    "<EXT escape $BE (v5+)>",
    "piracy (v5+)"
};

static const char *const names_var[32] = {
    "call", "storew", "storeb", "put_prop",
    "aread", "print_char", "print_num",
    "random", "push", "pull",
    "split_window", "set_window",
    "call_vs2 (v4+)", "erase_window (v4+)",
    "erase_line (v4+)", "set_cursor (v4+)",
    "get_cursor (v4+)", "set_text_style (v4+)",
    "buffer_mode (v4+)",
    "output_stream", "input_stream",
    "sound_effect",
    "read_char (v4+)", "scan_table (v4+)",
    "not (v5+)", "call_vn (v5+)", "call_vn2 (v5+)",
    "tokenise (v5+)", "encode_text (v5+)",
    "copy_table (v5+)", "print_table (v5+)",
    "check_arg_count (v5+)"
};

static const char *opcode_name(uint8_t op)
{
    if (op < 0x80u) {
        return names_2op[op & 0x1fu];
    }
    if (op < 0xb0u) {
        return names_1op[op & 0x0fu];
    }
    if (op < 0xc0u) {
        return names_0op[op & 0x0fu];
    }
    if (op < 0xe0u) {
        return names_2op[op & 0x1fu];
    }
    return names_var[op & 0x1fu];
}

static const char *stop_reason_name(int r)
{
    switch (r) {
    case ZVM_STOP_QUIT:    return "quit";
    case ZVM_STOP_RESTART: return "restart";
    case ZVM_STOP_AREAD:   return "aread";
    case ZVM_STOP_SAVE:    return "save";
    case ZVM_STOP_RESTORE: return "restore";
    case ZVM_STOP_UNIMPL:  return "unimplemented opcode";
    case ZVM_STOP_ERROR:   return "RUNTIME ERROR";
    default:               return "unknown";
    }
}

/* ---------------------------------------------------------------------- */

static const char *load_error_name(int err)
{
    switch (err) {
    case Z_ERR_NULL:       return "internal null pointer";
    case Z_ERR_TOO_SMALL:  return "story file too small / corrupt header";
    case Z_ERR_TOO_BIG:    return "story file too large for interpreter";
    case Z_ERR_VERSION:    return "unsupported Z-machine version (v3 only)";
    default:               return "unknown error";
    }
}

int main(int argc, char **argv)
{
    const char *story_path = NULL;
    unsigned long max_inst = 0;   /* 0 = unlimited */
    int trace = 0;
    int quiet = 0;
    static struct z_vm vm;      /* large (savebuf); keep off the stack */
    uint8_t *image;
    uint32_t image_len;
    uint8_t first_op;
    char serial[8];
    uint16_t computed_checksum;
    unsigned long executed;
    int err, reason;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_inst = strtoul(argv[++i], NULL, 10);
        } else {
            story_path = argv[i];
        }
    }
    if (story_path == NULL) {
        story_path = "zork1.zip";
    }
    if (snprintf(save_path, sizeof(save_path), "%s.lzs", story_path)
        >= (int)sizeof(save_path)) {
        fprintf(stderr, "story path too long\n");
        return 1;
    }

    if (read_whole_file(story_path, &image, &image_len) != 0) {
        return 1;
    }

    uint8_t *mem = malloc(Z_HOST_MEM_SIZE);
    if (mem == NULL) {
        fprintf(stderr, "out of memory for %u byte address space\n",
                Z_HOST_MEM_SIZE);
        free(image);
        return 1;
    }
    memset(mem, 0, Z_HOST_MEM_SIZE);

    memset(&vm, 0, sizeof(vm));
    vm.m.mem = mem;
    vm.m.mem_size = Z_HOST_MEM_SIZE;

    {
        struct z_io io;
        io.user = NULL;
        io.putc = host_putc;
        io.getc = host_getc;
        io.save = host_save;
        io.restore = host_restore;
        err = zvm_init(&vm, image, image_len, &io);
    }
    if (err != Z_OK) {
        fprintf(stderr, "%s: zvm_init failed: %s\n",
                story_path, load_error_name(err));
        free(image);
        free(mem);
        return 1;
    }
    /* image stays live: RESTART reinitializes from it */

    if (!quiet) {
        computed_checksum = z_checksum(&vm.m);
        z_serial(&vm.m, serial);
        fprintf(stderr,
                "story %s: v%u rel %u serial %s, len %u, "
                "high $%x, dict $%x, objects $%x, globals $%x, "
                "checksum $%04x/%04x [%s]\n",
                story_path, z_version(&vm.m), z_release(&vm.m), serial,
                vm.m.file_len, vm.m.high_base, vm.m.dict_base,
                vm.m.objects_base, vm.m.global_base,
                vm.m.checksum, computed_checksum,
                vm.m.checksum == computed_checksum ? "OK" : "MISMATCH");
    }

    vm.trace = trace;
    executed = 0;
    reason = ZVM_OK;
    while (reason == ZVM_OK) {
        if (max_inst != 0u && executed >= max_inst) {
            fprintf(stderr, "\n[instruction limit %lu reached]\n", max_inst);
            reason = ZVM_OK;
            break;
        }
        if (trace) {
            fprintf(stderr, "$%05x: $%02x %-18s sp=%-4u fr=%u acc=%04x\n",
                    vm.pc, z_read_byte(&vm.m, vm.pc),
                    opcode_name(z_read_byte(&vm.m, vm.pc)),
                    vm.sp, vm.nframes,
                    vm.sp > vm.frames[vm.nframes - 1].base_sp
                        ? vm.stack[vm.sp - 1] : 0xFFFFu);
        }
        reason = zvm_step(&vm);
        executed++;

        if (reason == ZVM_STOP_RESTART) {
            /* RESTART: wipe RAM and boot again from the story image
             * (the firmware equivalent reloads from XIP flash). */
            struct z_io io;
            memset(mem, 0, Z_HOST_MEM_SIZE);
            memset(&vm, 0, sizeof(vm));
            vm.m.mem = mem;
            vm.m.mem_size = Z_HOST_MEM_SIZE;
            io.user = NULL;
            io.putc = host_putc;
            io.getc = host_getc;
            io.save = host_save;
            io.restore = host_restore;
            if (zvm_init(&vm, image, image_len, &io) != Z_OK) {
                reason = ZVM_STOP_ERROR;
                break;
            }
            vm.trace = trace;
            reason = ZVM_OK;
        }
    }

    first_op = z_read_byte(&vm.m, vm.pc);
    fprintf(stderr,
            "stop: %s%s%s after %lu instructions at $%05x ($%02x %s)\n",
            stop_reason_name(reason),
            (vm.msg != NULL && reason != ZVM_OK) ? ": " : "",
            (vm.msg != NULL) ? vm.msg : "",
            executed, vm.pc, first_op, opcode_name(first_op));

    free(image);
    free(mem);

    if (reason == ZVM_STOP_ERROR || reason == ZVM_STOP_UNIMPL) {
        return 2;
    }
    return 0;
}
