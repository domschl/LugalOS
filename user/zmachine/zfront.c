/* zfront.c — LugalOS kernel front for the Z-machine interpreter
 *
 * (zmachine_front.h's session contract, and the memory rules stated
 * there, are what this file exists to keep.)
 *
 * Shape borrowed from user/chess: the interpreter core never allocates;
 * this front owns three page-granular blocks, all from the *bulk* class
 * (PSRAM on boards that have it — plan/phase38_psram.md), claimed in
 * zmachine_run() and released on every exit path:
 *
 *   address space   128 KB  (Z-machine v1-v4 flat space, 0x20000)
 *   story image     file-size, kept resident so RESTART can reinit
 *                   without going back to SD
 *   VM struct       ~52 KB  (stacks, frames, savebuf) — too big for any
 *                           kernel task stack, so it is allocated too
 *
 * Idle cost: zero.  Nothing here is .bss, no constructor, no pool.
 *
 * Console: putc -> console_putc(); getc -> console_getc() (blocking,
 * yields).  The interpreter echoes typed characters itself (aread does
 * the echoing per the Z-machine spec — same model as the host harness's
 * raw-mode terminal), so the front must not echo.  Ctrl-C is polled
 * between instructions via console_interrupt_requested(); a Ctrl-C that
 * arrives while aread() blocks for a keystroke is caught by the next
 * poll once input resumes.
 */
#include "zfront.h"

#include "zvm.h"

#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/vterm.h"
#include "lisp.h"

#include <string.h>

/* Z-machine v1-v4 flat address space (host harness uses the same). */
#define Z_SPACE_BYTES 0x20000u

#define Z_MAX_STORY   Z_SPACE_BYTES   /* a v3 story cannot exceed the space */

#define Z_PAGES(bytes) (((uint32_t)(bytes) + PAGE_SIZE - 1u) / PAGE_SIZE)

#define Z_DIR_DEFAULT "/sd0/games/"
#define Z_STORY_DEFAULT "zork1"

/* Session context; lives in the VM allocation (see ctx placement below)
 * and is handed to the io callbacks as `user`. */
struct zfront_ctx {
    char story_path[96];
    char save_path[104];

    /* Lisp-line interception (the chess console's convention, chess_ui.c's
     * `else if (line[0] == '(')`): a game-prompt line that starts with
     * '(' is collected here, evaluated by Lisp, and the game's aread is
     * handed an empty line -- 'look again', harmless in every v3 game.
     * The interpreter owns the echo, so in lisp mode this front echoes
     * for it. */
    bool lisp_mode;
    uint32_t lisp_len;
    char lisp_line[256];
};

/* ------------------------------------------------------------------ */
/* io callbacks (struct z_io) — no libc, no heap                       */
/* ------------------------------------------------------------------ */

static void zfront_putc(void *user, uint8_t ch)
{
    (void)user;
    console_putc((char)ch);
}

static int zfront_getc(void *user)
{
    struct zfront_ctx *ctx = (struct zfront_ctx *)user;
    for (;;) {
        int c = (int)(uint8_t)console_getc();
        if (ctx->lisp_mode) {
            if (c == '\r' || c == '\n') {
                char line[256];
                uint32_t n = ctx->lisp_len < sizeof(line) - 1
                           ? ctx->lisp_len : sizeof(line) - 1;
                console_putc('\n');
                memcpy(line, ctx->lisp_line, n);
                line[n] = '\0';
                ctx->lisp_mode = false;
                ctx->lisp_len = 0;
                lisp_lock();
                lisp_val_t *res = lisp_eval_string(line);
                if (res && res->type != LISP_NIL) {
                    cprintf("=> ");
                    lisp_print(res);
                    cprintf("\n");
                }
                lisp_unlock();
                return '\n';       /* end the game's aread on empty input */
            }
            if (c == 0x7f || c == 0x08) {
                if (ctx->lisp_len > 0) {
                    ctx->lisp_len--;
                    console_puts("\b \b");
                }
                continue;
            }
            if (ctx->lisp_len < sizeof(ctx->lisp_line) - 1) {
                ctx->lisp_line[ctx->lisp_len++] = (char)c;
                console_putc((char)c);
            }
            continue;
        }
        if (c == '(') {
            ctx->lisp_mode = true;
            ctx->lisp_len = 0;
            ctx->lisp_line[ctx->lisp_len++] = '(';
            console_putc('(');
            continue;
        }
        return c;
    }
}

static int zfront_save(void *user, const uint8_t *data, uint32_t len)
{
    struct zfront_ctx *ctx = (struct zfront_ctx *)user;
    int fd = vfs_open(ctx->save_path,
                      VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) {
        return -1;
    }
    int rc = (vfs_pwrite(fd, data, len, 0) == (int)len) ? 0 : -1;
    vfs_close(fd);
    return rc;
}

static int zfront_restore(void *user, uint8_t *data, uint32_t cap,
                          uint32_t *len_out)
{
    struct zfront_ctx *ctx = (struct zfront_ctx *)user;
    vfs_stat_t st;
    int fd = vfs_open(ctx->save_path, VFS_O_READ);
    if (fd < 0) {
        return -1;
    }
    if (vfs_fstat(fd, &st) != 0 || st.size == 0 || st.is_dir
        || st.size > cap) {
        vfs_close(fd);
        return -1;
    }
    int got = vfs_pread(fd, data, st.size, 0);
    vfs_close(fd);
    if (got != (int)st.size) {
        return -1;
    }
    *len_out = st.size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                      */
/* ------------------------------------------------------------------ */

static bool has_suffix_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) {
        return false;
    }
    for (size_t i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

static void resolve_path(struct zfront_ctx *ctx, const char *name)
{
    if (name == NULL || name[0] == '\0') {
        name = Z_STORY_DEFAULT;
    }
    if (strchr(name, '/') != NULL
        || has_suffix_ci(name, ".z3") || has_suffix_ci(name, ".zip")) {
        ksnprintf(ctx->story_path, sizeof(ctx->story_path), "%s", name);
    } else {
        ksnprintf(ctx->story_path, sizeof(ctx->story_path), "%s%s.z3",
                  Z_DIR_DEFAULT, name);
    }
    ksnprintf(ctx->save_path, sizeof(ctx->save_path), "%s.lzs",
              ctx->story_path);
}

/* ------------------------------------------------------------------ */

static int reinit_vm(struct z_vm *vm, const uint8_t *image,
                     uint32_t image_len, struct z_io *io)
{
    memset(vm->m.mem, 0, vm->m.mem_size);
    return zvm_init(vm, image, image_len, io);
}

int zmachine_run(const char *name)
{
    uint8_t *space = NULL;    /* the 128 KB Z-machine address space   */
    uint8_t *image = NULL;    /* story, resident for RESTART          */
    struct z_vm *vm = NULL;
    uint32_t fsize = 0;       /* story bytes; doubles as image size   */
    uint32_t space_pages = Z_PAGES(Z_SPACE_BYTES);

    /* Pin the session to the terminal it started on.  The root console
     * task follows the active foreground terminal (task_get_vterm()==0),
     * and opening or switching terminals re-points that follower -- which
     * silently moves the game's input to the new terminal's queue while
     * its own worker task reads the very same queue (two readers, bytes
     * split at random) -- the freeze sequence reported 2026-10-04: game
     * running, Cmd+Enter opens a second terminal, the next command typed
     * there lands half in the game and half in the new shell.  Binding
     * the task the way shell_spawn_terminal() binds workers pins the
     * game to one terminal for its duration; the follow is restored on
     * the way out. */
    int mypid = sched_current_pid();
    int prev_vid = task_get_vterm(mypid);
    if (mypid >= 0 && prev_vid == 0) {
        task_set_vterm(mypid, vterm_current_id());
    }
    int reason = ZVM_OK;
    int rc = 1;               /* until a clean exit rewrites it        */

    /* One bounded context object on the (shell task's) stack is enough;
     * nothing heavy touches the stack. */
    struct zfront_ctx ctx;
    resolve_path(&ctx, name);

    vfs_stat_t st;
    if (vfs_stat(ctx.story_path, &st) != 0 || st.is_dir || st.size == 0) {
        cprintf("zmachine: no story at %s\n", ctx.story_path);
        return 1;
    }
    if (st.size > Z_MAX_STORY) {
        cprintf("zmachine: %s is %u bytes, larger than the %u-byte v3 space\n",
                ctx.story_path, st.size, Z_SPACE_BYTES);
        return 1;
    }

    fsize = st.size;   /* fixed before any allocation can fail midway */
    space = (uint8_t *)palloc_pages_bulk(space_pages);
    image = (uint8_t *)palloc_pages_bulk(Z_PAGES(fsize));
    vm    = (struct z_vm *)palloc_pages_bulk(Z_PAGES(sizeof(*vm)));
    if (space == NULL || image == NULL || vm == NULL) {
        cprintf("zmachine: out of bulk memory (see /proc/meminfo)\n");
        goto done;
    }
    memset(vm, 0, sizeof(*vm));

    {
        int fd = vfs_open(ctx.story_path, VFS_O_READ);
        if (fd < 0 || vfs_pread(fd, image, st.size, 0) != (int)st.size) {
            if (fd >= 0) {
                vfs_close(fd);
            }
            cprintf("zmachine: cannot read %s\n", ctx.story_path);
            goto done;
        }
        vfs_close(fd);
    }

    vm->m.mem = space;
    vm->m.mem_size = Z_SPACE_BYTES;

    struct z_io io;
    io.user = &ctx;   /* lisp_mode/line start zeroed: ctx is a stack local
                       * cleared by resolve + explicit init below */
    ctx.lisp_mode = false;
    ctx.lisp_len = 0;
    io.putc = zfront_putc;
    io.getc = zfront_getc;
    io.save = zfront_save;
    io.restore = zfront_restore;

    if (zvm_init(vm, image, fsize, &io) != Z_OK) {
        cprintf("zmachine: %s is not a playable v3 story\n", ctx.story_path);
        goto done;
    }

    cprintf("zmachine: %s — v%u rel %u, %u bytes (PSRAM: %s)\n",
            ctx.story_path, z_version(&vm->m), z_release(&vm->m), fsize,
            palloc_is_bulk(space) ? "yes" : "no (fallback: SRAM)");

    while (true) {
        if (console_interrupt_requested()) {
            console_interrupt_clear();
            cprintf("\nzmachine: interrupted\n");
            break;
        }
        reason = zvm_step(vm);
        if (reason == ZVM_STOP_RESTART) {
            if (reinit_vm(vm, image, fsize, &io) != Z_OK) {
                reason = ZVM_STOP_ERROR;
            } else {
                continue;
            }
        }
        if (reason != ZVM_OK) {
            break;
        }
    }

    if (reason == ZVM_STOP_ERROR || reason == ZVM_STOP_UNIMPL) {
        cprintf("zmachine: %s%s%s\n",
                vm->msg ? vm->msg : "run error",
                "", "");
        rc = 2;
    } else {
        rc = 0;                 /* QUIT, RESTART-to-nothing, Ctrl-C */
    }

done:
    if (mypid >= 0 && prev_vid == 0) {
        task_set_vterm(mypid, prev_vid);   /* resume following foreground */
    }
    if (space != NULL) {
        palloc_free(space, space_pages);
    }
    if (image != NULL) {
        palloc_free(image, Z_PAGES(fsize));
    }
    if (vm != NULL) {
        palloc_free(vm, Z_PAGES(sizeof(*vm)));
    }
    return rc;
}
