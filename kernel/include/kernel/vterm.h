#ifndef LUGALOS_KERNEL_VTERM_H
#define LUGALOS_KERNEL_VTERM_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel/lock.h"
#include "drivers/vtterm.h"

/*
 * Virtual Terminals (Phase 44.1, plan/phase44_horizontal_tiling_window_system.md).
 *
 * Each virtual terminal encapsulates:
 *   - An independent input queue (rx_buf ring buffer) with its own lock
 *   - An independent VT terminal emulator instance (vtterm_t) and cell shadow
 *   - An owner task (e.g. an interactive shell `lsh`)
 *   - An active status: whether it is the currently focused foreground console
 *
 * Background tasks continue writing to their vterm's shadow buffer without
 * interfering with the active display. When a vterm is switched to foreground,
 * its contents are repainted onto the screen.
 */

#define MAX_VTERMS          8u
#define VTERM_RX_BUF_SIZE   128u
#define VTERM_TITLE_MAX     32u

typedef struct vterm {
    int       id;
    bool      in_use;
    bool      active;            /* currently focused / visible foreground terminal */
    int       owner_pid;         /* PID of task owning this vterm */
    char      title[VTERM_TITLE_MAX];

    /* Input queue */
    ylock_t   input_lock;
    char      rx_buf[VTERM_RX_BUF_SIZE];
    uint8_t   rx_head;
    uint8_t   rx_tail;
    int       pushback;          /* ungetc character, -1 if none */
    bool      interrupt_pending; /* Ctrl-C */

    /* Terminal emulation */
    vtterm_t  vt;
    uint16_t *shadow;            /* cell shadow buffer (allocated in PSRAM or heap) */
    uint16_t  shadow_cols;
    uint16_t  shadow_rows;
    bool      has_vt;            /* true if vt and shadow are initialized */
} vterm_t;

/* Initialize the virtual terminal subsystem. Sets up vterm[0] as root console. */
void     vterm_init_subsystem(void);

/* Allocate a new virtual terminal. Returns pointer or NULL if table full. */
vterm_t *vterm_create(const char *title);

/* Destroy a virtual terminal and release its resources (cannot destroy vterm 0). */
void     vterm_destroy(int id);

/* Look up a vterm by ID (0 .. MAX_VTERMS - 1). Returns NULL if not in use. */
vterm_t *vterm_get(int id);

/* Get the vterm associated with the current running task. */
vterm_t *vterm_current(void);

/* Get current running task's vterm ID. */
int      vterm_current_id(void);

/* Get the currently active/focused foreground vterm. */
vterm_t *vterm_active(void);

/* Get currently active vterm ID. */
int      vterm_active_id(void);

/* Set the active/focused vterm (switches keyboard focus and repaints if screen up). */
bool     vterm_set_active(int id);

/* Total number of currently allocated vterms. */
int      vterm_count(void);

/* --- Input routing -------------------------------------------------------- */

/* Feeds a character into a vterm's input queue. Returns false if full. */
bool     vterm_feed_char(vterm_t *vt, char c);

/* Feeds a sequence of bytes into a vterm's input queue. Returns bytes queued. */
uint32_t vterm_feed_bytes(vterm_t *vt, const char *s, uint32_t n);

/* Non-blocking read from a vterm's input queue. Returns byte (0..255) or -1 if empty. */
int      vterm_getc_nonblock(vterm_t *vt);

/* Check if a vterm has input available in its queue. */
bool     vterm_has_char(vterm_t *vt);

/* Push a character back onto a vterm's input queue. */
void     vterm_ungetc(vterm_t *vt, char c);

/* Check and clear Ctrl-C interrupt flag for a vterm. */
bool     vterm_check_interrupt(vterm_t *vt);

/* --- Output routing ------------------------------------------------------- */

/* Write characters to a vterm. Runs through vtterm parser into shadow/screen. */
void     vterm_write(vterm_t *vt, const char *s, uint32_t n);

/* Set title of a vterm. */
void     vterm_set_title(vterm_t *vt, const char *title);

/* --- Terminal Spawning & Selftest ----------------------------------------- */

/* Spawns a new independent shell task attached to a new virtual terminal.
 * Returns the new vterm ID, or -1 on failure. */
int      shell_spawn_terminal(const char *title);

/* Automated self-test verifying multi-vterm creation, input/output isolation,
 * and concurrency. Returns 0 on success, or number of failures. */
int      vterm_selftest(void);

#endif /* LUGALOS_KERNEL_VTERM_H */
