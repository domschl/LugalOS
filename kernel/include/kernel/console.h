#ifndef LUGALOS_KERNEL_CONSOLE_H
#define LUGALOS_KERNEL_CONSOLE_H

#include <stdint.h>
#include "kernel/printk.h"
#include <stdbool.h>

/* The console stream (B4, plan/phase5_distributed_design.md §5.4).
 *
 * B0 gave the kernel log a ring and detachable sinks, and recorded a
 * limitation it could not fix at the time: printk() carried *two* unrelated
 * things -- kernel diagnostics and user-facing output (shell results, the
 * Lisp REPL, editor screens). So `klog detach console` silenced the shell
 * along with the log, which is not what anyone wants and not the §5.2
 * scenario this track exists to deliver.
 *
 * The two streams are now separate:
 *
 *   printk()   -- kernel diagnostics. Goes to the klog ring and its sinks.
 *   cprintf()  -- user-facing output. Goes to whatever device the console is
 *                 bound to, and is never affected by log-sink changes.
 *
 * Detaching the log sink now stops `[Sched] ...` lines from reaching the
 * terminal while the shell keeps working, and the log keeps accumulating in
 * the ring for /proc/kmsg either way. That is the whole point.
 *
 * The console is *bindable* for the same reason the log sinks are: a channel
 * may carry kernel output until something else should own it. Binding it to
 * a different device is what B4's later work (a console server reachable
 * through a channel) builds on.
 */

typedef void (*console_putc_fn)(char);

/* What a DEV_KIND_CONSOLE device hands back from its get(). A struct rather
 * than a bare function pointer because dev_get() returns void*, and ISO C
 * has no conforming conversion between object and function pointers. */
typedef struct {
    console_putc_fn putc;
} console_dev_t;

/* Binds the console stream to an output function. Passing NULL discards
 * output rather than crashing -- a console bound to nothing is a legitimate
 * state (the device was handed to something else), not an error. */
void console_bind(console_putc_fn putc);

/* --- The output lock (Y5d, plan/phase31_concurrency_hierarchy.md) ---------
 *
 * One message, one uninterrupted run. This is what printk_lock() used to be,
 * and it is a plain `ylock_t` now rather than a third hand-rolled blocking
 * primitive with its own owner, depth, waiter slot and wait-for edge --
 * kernel/lock.h's ylock already had every one of those, correctly, including
 * re-entry by the owner and the task-less-hart case that bring-up needs.
 *
 * It guards *the wire*, not one stream. Three writers reach the same UART and
 * all three take it: cprintf() and the console stream, printk_debug() (which
 * writes the registers directly and bypasses everything else by design), and
 * klogd's drain. Y5c's boot output spliced mid-word precisely because two of
 * them stopped sharing a lock.
 *
 * It may be held across a block -- a console write ends in chan_call() to the
 * uart task -- which is what makes it a ylock and not a spinlock. printk()
 * does not take it at all any more; that is the whole of Y5. */
void console_lock(void);
void console_unlock(void);
/* Whether the calling context holds console_lock() -- for a writer that
 * relies on it to check that it is really there (phase31 §7 R4). */
bool console_lock_held(void);

/* Ends a write: pushes whatever the UART driver has batched.
 *
 * The console stream's own, rather than borrowed from printk_unlock(), which
 * is where this lived until Y5d and is why taking printk() off that lock left
 * console output stranded in a per-hart batch (Y5c, §5.7). A whole write is
 * the boundary; single characters still batch. */
void console_flush(void);

/* What a console that is a *screen* adds to a byte stream -- the
 * RP2350-LCD-7's, and nothing else today. One slot; NULL clears it, and then
 * every call below answers as a plain serial line would.
 *
 *   flush      36.6a: the screen terminal batches into its U-mode task.
 *              Called from console_flush(), and on every turn of an input
 *              wait, so an echoed keystroke is not left in a batch -- and so
 *              the screen gets a regular chance to update its status bar
 *              (37.1) while nothing is being written.
 *   size       37.1: the text window's size (it is smaller than the screen:
 *              the status bar takes a row).
 *   set_title  37.1: the status bar's title.
 *   canvas     37.3b: one canvas request (drivers/screen.h's protocol) and
 *              its reply; false if there is no canvas to ask. */
typedef struct {
    void (*flush)(void);
    bool (*size)(unsigned *cols, unsigned *rows);
    void (*set_title)(const char *title);
    bool (*canvas)(const uint8_t *req, uint32_t n, uint8_t *reply);
    /* 37.5a: the whole 1-bpp frame, for a screenshot -- drivers/fbtext.h's
     * format (leftmost pixel in bit 0); false if there is none yet. */
    bool (*pixels)(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride);
} console_screen_t;

void console_set_screen(const console_screen_t *screen);

/* 37.1, plan/phase37_screen_layouts_and_apps.md §2.4: how many columns and
 * rows a program may use. False when the console cannot say (a serial line:
 * the far end's size is not known here), and the caller keeps its default. */
bool console_size(unsigned *cols, unsigned *rows);

/* 37.1: what the screen's status bar names as the running program. A no-op
 * where there is no screen, so callers need not ask. Nothing is written to
 * the byte stream: a host terminal on the tee keeps its own title. */
void console_set_title(const char *title);

/* 37.3b, plan/phase37_screen_layouts_and_apps.md §2: a canvas request, as
 * drivers/screen.h describes them, to whatever screen there is -- the
 * RP2350-LCD-7's panel, or a RAM screen elsewhere (drivers/ramscreen.c).
 * `reply` holds SCREEN_REPLY_LEN bytes. False when there is no canvas at
 * all; the reply's own status says whether this request was refused. */
bool console_canvas(const uint8_t *req, uint32_t n, uint8_t *reply);

/* 37.5b: everything written through console_putc() -- cprintf(), a Lisp
 * program's output, lisp_print() -- goes into `buf` instead of the screen
 * until console_capture_end(), which returns how many bytes it holds (the
 * last `cap` written). The editor's evaluate-and-stay uses it to put a
 * result on its status line rather than over its text. Not the kernel log,
 * which has its own sinks. One capture at a time. */
void console_capture(char *buf, uint32_t cap);
uint32_t console_capture_end(void);

/* 37.5a: the frame, for kernel/screenshot.c; false where there is none. */
bool console_pixels(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride);

/* 37.5a: keys the screen takes before any program sees them -- the USB
 * keyboard's Super+[ , Super+] , Super+\\ and Super+Shift+3. An input source hands one
 * over here while it is being drained (under the input lock); the console
 * runs it from its next input wait, outside every lock, where it may draw
 * and write files. */
#define CONSOLE_HOTKEY_NONE         0u
#define CONSOLE_HOTKEY_LEFT         1u  /* Super+[ : shrink window column / divider left */
#define CONSOLE_HOTKEY_RIGHT        2u  /* Super+] : expand window column / divider right */
#define CONSOLE_HOTKEY_SCREENSHOT   3u  /* Super+Shift+3 : save screenshot */
#define CONSOLE_HOTKEY_SWAP         4u  /* Super+\ : swap active window with neighbor */
#define CONSOLE_HOTKEY_NEW_TERM     5u  /* Super+Enter : spawn new terminal window */
#define CONSOLE_HOTKEY_FOCUS_LEFT   6u  /* Super+Left : focus left window */
#define CONSOLE_HOTKEY_FOCUS_RIGHT  7u  /* Super+Right : focus right window */
#define CONSOLE_HOTKEY_MOVE_LEFT    8u  /* Super+Ctrl+Left : move window left in ribbon */
#define CONSOLE_HOTKEY_MOVE_RIGHT   9u  /* Super+Ctrl+Right : move window right in ribbon */
#define CONSOLE_HOTKEY_CLOSE        10u /* Super+W : close active window */
#define CONSOLE_HOTKEY_JUMP_BASE    11u /* Super+1 .. Super+9 (codes 11..19) */
#define CONSOLE_HOTKEY_JUMP(n)      (CONSOLE_HOTKEY_JUMP_BASE + ((unsigned)(n) - 1u))
void console_hotkey(unsigned code);

/* 36.9, plan/phase36_rp2350_lcd7_terminal.md §4.4: where console input comes
 * from. Each source is polled by the console pump, in registration order,
 * into the one pushback ring every reader uses. The RP2350 has two today --
 * the serial UART (or its demux) and USB CDC -- and the RP2350-LCD-7 adds the
 * USB keyboard; every other board has the single `uart` source.
 *
 *   has_char()        a byte is waiting
 *   getc()            it, or -1 if there turned out to be none; never blocks
 *   peek_interrupt()  optional: a Ctrl-C is waiting, answered *without*
 *                     consuming, so it latches even when the ring is full
 *   indicator()       optional (37.2): a short state to show on a screen's
 *                     status bar, e.g. the keyboard's `Compose "`, or "" for
 *                     none. Polled by the screen, never pushed: a source is
 *                     drained under the input lock, and drawing takes the
 *                     console lock, so a push from here could deadlock
 *                     against a program printing while it checks for Ctrl-C.
 *
 * Registered once at boot by kernel/board.c (a keyboard later, when its task
 * starts); the table holds CONSOLE_INPUT_MAX. */
typedef struct {
    const char *name;
    bool (*has_char)(void);
    int  (*getc)(void);
    bool (*peek_interrupt)(void);
    const char *(*indicator)(void);
} console_input_t;

#define CONSOLE_INPUT_MAX 4
int console_input_register(const console_input_t *src);

/* 37.2: every source's non-empty indicator, two spaces apart, into buf
 * (NUL-terminated, at most cap bytes). Returns the length. */
uint32_t console_indicators(char *buf, uint32_t cap);

void console_putc(char c);
void console_puts(const char *s);

/* Flush the kernel log to the console before starting a new message, so the
 * backlog appears *before* it rather than inside it. Call at a whole-write
 * boundary only -- see the body in kernel/console.c for the two places that
 * called it mid-message and the splices that produced. */
void console_sync(void);

/* --- Where the CRLF convention lives (C0, plan/phase6_memory_and_processes.md §6.1) ---
 *
 * A terminal needs CR before LF; a 9P frame must not have one inserted into
 * it. Those two facts decide the layer this belongs to, and it is not the one
 * that looks most obvious.
 *
 * It used to live in vprintk_to(), which inserted '\r' for a '\n' *in the
 * format string* -- and therefore not for bytes emitted through %s. So
 * `cat file.c` printed a staircase: cprintf("%s\n", buf) translated its own
 * trailing newline and none of the file's.
 *
 * The tempting fix is to translate in uart_putc(), covering everything at
 * once. That is wrong here: drivers/uart_net.c sends SLIP-encoded 9P frames
 * through uart_putc(), so translating there would insert 0x0D into binary
 * protocol data and corrupt every frame containing a 0x0A byte.
 *
 * So it lives on the *console stream*: the thing that is by definition
 * attached to a terminal. printk() and cprintf() now emit raw '\n', the klog
 * ring stores raw '\n' (which is what a remote node reading /proc/kmsg over
 * 9P wants), and the conversion happens once, on the way out to a device
 * acting as a terminal.
 *
 * Exposed rather than kept static because the kernel log's "console" sink
 * needs the same conversion while writing to a device this stream does not
 * own -- see kernel/main.c. Callers pass the destination; the policy stays
 * here, in one place. */
void console_emit(console_putc_fn out, char c);

/* Formatted user-facing output. Same format engine as printk(); the
 * difference is only which stream it lands on. */
int cprintf(const char *fmt, ...) LUGALOS_PRINTF(1, 2);

/* --- The console as a server (B4) ---
 *
 * Two things make this a server rather than a renamed printf:
 *
 *   1. It is reachable through a channel. `/srv/console` is a chan endpoint,
 *      so any task -- or a remote node over 9P, since /srv/ is in the
 *      namespace -- can send it output using the same copy-always IPC as
 *      every other service. Nothing about writing to the console requires
 *      being the kernel, or being on this machine.
 *
 *   2. Its device is bound by NAME at runtime, from the device registry, so
 *      init.lisp decides who owns the terminal. That is §5.2's scenario:
 *      a channel carries kernel output until something else should have it.
 *
 * Local cprintf() deliberately stays a direct call rather than a channel
 * round trip. Routing every character through a rendezvous would make output
 * a scheduling event -- unusable from a fault handler, and a large cost for
 * no isolation gain while the console driver is kernel code anyway. The
 * server-ness here is about reachability and ownership, not about forcing
 * every byte through a queue. B6's preemption is what would change that
 * calculus, and it is called out in the plan rather than assumed away. */

/* Registers the "console" channel endpoint. Call once at boot, after the
 * device registry is populated. */
int console_server_init(void);

/* Binds the console stream to a named DEV_KIND_CONSOLE device from the
 * registry. Returns 0, or -1 if no such device is present. */
int console_bind_device(const char *name);

/* The name currently bound, or "(none)". */
const char *console_bound_device(void);

/* --- Ctrl-C interrupt (J2, plan/phase10_chess_completion.md) ---
 *
 * General kernel infrastructure, not chess-specific -- the input-side
 * counterpart to this file's output stream above, added because chess's
 * search needed a way to stop an unbounded (Level 8 / bare `go`) search
 * from the terminal, and the same need recurs for a run-away Lisp
 * evaluation (user/lisp/lisp.c's lisp_eval(), which already guards
 * unbounded *recursion* via LISP_MAX_EVAL_DEPTH but not a legitimately
 * bounded-depth call that is simply expensive).
 *
 * Advisory only: LugalOS has no preemption in the paths this is for (a
 * synchronous search, lisp_eval()'s own recursion), so this cannot stop
 * anything by itself. A long-running foreground command polls
 * console_interrupt_requested() at a cheap, regular interval of its own
 * choosing (search.c's check_up_time() already does this every 2048
 * nodes; lisp_eval() every 50 ms of evaluation) and unwinds cooperatively
 * when it returns true.
 *
 * Non-blocking, byte-level: drains whatever is waiting on the bound input
 * device and latches true the instant it sees a raw 0x03 (Ctrl-C). Nothing
 * is discarded -- every byte, the 0x03 included, is queued for whoever reads
 * next (see console_getc() below). Latching where bytes *enter* rather than
 * where they are consumed is what lets the latch be reliable without the
 * byte being stolen: edit_multiline_box() legitimately reads a Ctrl-C as
 * data (its Ctrl-X Ctrl-C exit), and a poller still sees the interrupt.
 *
 * It used to discard them, which this comment described as "a real,
 * explicit tradeoff: there is no push-back/ungetc() on the underlying
 * read". The tradeoff was not worth what it cost. search.c's
 * check_up_time() polls this every 2048 nodes, so during any engine think
 * *every keystroke that was not Ctrl-C was actively thrown away* -- type-
 * ahead did not merely fail to queue, it was consumed and destroyed by the
 * interrupt check itself. The push-back the old comment said did not exist
 * is now the few lines below it, and typing during a long command works.
 * (§"input-eater", plan/phase15_memory_reclamation.md.) */
bool console_interrupt_requested(void);

/* Clears a latched interrupt. Call once after a poll loop has acted on a
 * true result, so a Ctrl-C typed during one command doesn't leak into the
 * next. */
void console_interrupt_clear(void);

/* --- Console input, with push-back ---
 *
 * The input counterpart to console_putc()/console_puts(). Every reader of
 * console input should use these rather than uart_getc()/uart_has_char()
 * directly, because bytes can be sitting in the push-back ring instead of
 * the device: console_interrupt_requested() above drains the device while
 * hunting for Ctrl-C, and parks everything else here. A reader that went
 * straight to the UART would not see those bytes at all, in the exact
 * situation (a long foreground command) where they matter most.
 *
 * The ring is small and deliberately bounded -- it is type-ahead, not a
 * buffer anything should rely on. Nothing is discarded to keep it that way:
 * when it fills, the drain simply stops and the surplus stays in the device,
 * where the UART's own buffering holds it until a reader makes room. */
bool console_has_char(void);

/* Blocks (yielding, via the underlying device read) until a byte is
 * available. Serves push-back first, in arrival order, then the device. */
char console_getc(void);

/* Returns a byte to the front of the stream, for a reader that has looked
 * at one and decided it belongs to someone else. Bounded by the same ring;
 * a push-back that does not fit is dropped. */
void console_ungetc(char c);

#endif /* LUGALOS_KERNEL_CONSOLE_H */
