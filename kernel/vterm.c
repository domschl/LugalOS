#include "kernel/vterm.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include <string.h>

static vterm_t g_vterms[MAX_VTERMS];
static int     g_active_vterm = 0;
static ylock_t g_vterm_mgr_lock;
static bool    g_vterm_initialized = false;

/* External worker entry defined in kernel/shell.c */
void shell_worker_task_entry(void *arg);

void vterm_init_subsystem(void) {
    if (g_vterm_initialized) return;
    ylock_init(&g_vterm_mgr_lock);

    for (unsigned i = 0; i < MAX_VTERMS; i++) {
        vterm_t *vt = &g_vterms[i];
        memset(vt, 0, sizeof(*vt));
        vt->id = (int)i;
        vt->pushback = -1;
        ylock_init(&vt->input_lock);
    }

    /* vterm[0] is always the root console */
    g_vterms[0].in_use = true;
    g_vterms[0].active = true;
    g_vterms[0].owner_pid = 0;
    strncpy(g_vterms[0].title, "lsh [0]", VTERM_TITLE_MAX - 1);
    g_active_vterm = 0;

    g_vterm_initialized = true;
}

vterm_t *vterm_get(int id) {
    if (!g_vterm_initialized) vterm_init_subsystem();
    if (id < 0 || id >= (int)MAX_VTERMS) return NULL;
    vterm_t *vt = &g_vterms[id];
    return vt->in_use ? vt : NULL;
}

vterm_t *vterm_current(void) {
    if (!g_vterm_initialized) vterm_init_subsystem();
    int pid = sched_current_pid();
    int vid = (pid >= 0) ? task_get_vterm(pid) : 0;
    return vterm_get(vid);
}

int vterm_current_id(void) {
    vterm_t *vt = vterm_current();
    return vt ? vt->id : 0;
}

vterm_t *vterm_active(void) {
    if (!g_vterm_initialized) vterm_init_subsystem();
    return &g_vterms[g_active_vterm];
}

int vterm_active_id(void) {
    return g_active_vterm;
}

int vterm_count(void) {
    if (!g_vterm_initialized) vterm_init_subsystem();
    int cnt = 0;
    for (unsigned i = 0; i < MAX_VTERMS; i++) {
        if (g_vterms[i].in_use) cnt++;
    }
    return cnt;
}

vterm_t *vterm_create(const char *title) {
    if (!g_vterm_initialized) vterm_init_subsystem();

    ylock_acquire(&g_vterm_mgr_lock);
    int slot = -1;
    for (unsigned i = 1; i < MAX_VTERMS; i++) {
        if (!g_vterms[i].in_use) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        ylock_release(&g_vterm_mgr_lock);
        printk("[VTerm] Table full; cannot create virtual terminal\n");
        return NULL;
    }

    vterm_t *vt = &g_vterms[slot];
    vt->in_use = true;
    vt->active = false;
    vt->owner_pid = -1;
    vt->rx_head = vt->rx_tail = 0;
    vt->pushback = -1;
    vt->interrupt_pending = false;

    if (title && title[0]) {
        strncpy(vt->title, title, VTERM_TITLE_MAX - 1);
        vt->title[VTERM_TITLE_MAX - 1] = '\0';
    } else {
        ksnprintf(vt->title, VTERM_TITLE_MAX, "lsh [%d]", slot);
    }

    /* Allocate shadow buffer: 98 columns x 27 rows x 2 bytes = 5292 bytes = 2 pages */
    uint32_t cols = 98u;
    uint32_t rows = 27u;
    uint32_t shadow_bytes = cols * rows * sizeof(uint16_t);
    uint32_t pages = (shadow_bytes + 4095u) / 4096u;

    void *shadow = NULL;
#if defined(CONFIG_PSRAM_BYTES)
    shadow = palloc_pages_bulk(pages);
    if (!shadow) shadow = palloc_pages(pages);
#else
    shadow = palloc_pages(pages);
#endif

    if (!shadow) {
        vt->in_use = false;
        ylock_release(&g_vterm_mgr_lock);
        printk("[VTerm] Out of memory for terminal shadow buffer (%u pages)\n", pages);
        return NULL;
    }

    memset(shadow, 0, shadow_bytes);
    vt->shadow = (uint16_t *)shadow;
    vt->shadow_cols = (uint16_t)cols;
    vt->shadow_rows = (uint16_t)rows;

    /* Initialize vtterm for this virtual console */
    fbtext_t text;
    fbtext_init(&text, NULL, 0, cols, rows);
    vtterm_init(&vt->vt, &text, vt->shadow);
    vt->has_vt = true;
    vtterm_set_hidden(&vt->vt, true); /* starts hidden in background */

    ylock_release(&g_vterm_mgr_lock);
    return vt;
}

void vterm_destroy(int id) {
    if (id <= 0 || id >= (int)MAX_VTERMS) return; /* vterm 0 cannot be destroyed */
    if (!g_vterm_initialized) return;

    ylock_acquire(&g_vterm_mgr_lock);
    vterm_t *vt = &g_vterms[id];
    if (!vt->in_use) {
        ylock_release(&g_vterm_mgr_lock);
        return;
    }

    char reset_title[VTERM_TITLE_MAX];
    bool title_change = false;

    /* If closing the active foreground terminal, switch focus to root vterm 0 */
    if (g_active_vterm == id) {
        g_vterms[0].active = true;
        g_active_vterm = 0;
        if (g_vterms[0].has_vt) {
            vtterm_set_hidden(&g_vterms[0].vt, false);
            vtterm_repaint(&g_vterms[0].vt);
        }
        strncpy(reset_title, g_vterms[0].title, sizeof(reset_title) - 1);
        reset_title[sizeof(reset_title) - 1] = '\0';
        title_change = true;
    }

    if (vt->shadow) {
        uint32_t shadow_bytes = (uint32_t)vt->shadow_cols * (uint32_t)vt->shadow_rows * sizeof(uint16_t);
        uint32_t pages = (shadow_bytes + 4095u) / 4096u;
        palloc_free(vt->shadow, pages);
        vt->shadow = NULL;
    }

    vt->in_use = false;
    vt->active = false;
    vt->has_vt = false;
    vt->owner_pid = -1;

    ylock_release(&g_vterm_mgr_lock);
    if (title_change) {
        console_set_title(reset_title);
    }
}

bool vterm_set_active(int id) {
    if (!g_vterm_initialized) vterm_init_subsystem();
    if (id < 0 || id >= (int)MAX_VTERMS) return false;

    ylock_acquire(&g_vterm_mgr_lock);
    vterm_t *target = &g_vterms[id];
    if (!target->in_use) {
        ylock_release(&g_vterm_mgr_lock);
        return false;
    }

    if (g_active_vterm == id) {
        ylock_release(&g_vterm_mgr_lock);
        return true;
    }

    int prev = g_active_vterm;
    g_vterms[prev].active = false;
    target->active = true;
    g_active_vterm = id;

    /* Hide background vterm */
    if (g_vterms[prev].has_vt) {
        vtterm_set_hidden(&g_vterms[prev].vt, true);
    }

    /* Unhide and repaint foreground vterm */
    if (target->has_vt) {
        vtterm_set_hidden(&target->vt, false);
        vtterm_repaint(&target->vt);
    }

    char active_title[VTERM_TITLE_MAX];
    strncpy(active_title, target->title, sizeof(active_title) - 1);
    active_title[sizeof(active_title) - 1] = '\0';
    ylock_release(&g_vterm_mgr_lock);

    console_set_title(active_title);
    return true;
}

/* --- Input queue implementation ------------------------------------------ */

bool vterm_feed_char(vterm_t *vt, char c) {
    if (!vt || !vt->in_use) return false;

    if (c == 0x03) {
        vt->interrupt_pending = true;
    }

    ylock_acquire(&vt->input_lock);
    uint8_t next = (uint8_t)((vt->rx_head + 1u) % VTERM_RX_BUF_SIZE);
    if (next == vt->rx_tail) {
        ylock_release(&vt->input_lock);
        return false; /* buffer full */
    }

    vt->rx_buf[vt->rx_head] = c;
    vt->rx_head = next;
    ylock_release(&vt->input_lock);
    return true;
}

uint32_t vterm_feed_bytes(vterm_t *vt, const char *s, uint32_t n) {
    if (!vt || !vt->in_use || !s) return 0;
    uint32_t fed = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!vterm_feed_char(vt, s[i])) break;
        fed++;
    }
    return fed;
}

int vterm_getc_nonblock(vterm_t *vt) {
    if (!vt || !vt->in_use) return -1;

    if (vt->pushback >= 0) {
        int c = vt->pushback;
        vt->pushback = -1;
        return c;
    }

    ylock_acquire(&vt->input_lock);
    if (vt->rx_head == vt->rx_tail) {
        ylock_release(&vt->input_lock);
        return -1;
    }

    char c = vt->rx_buf[vt->rx_tail];
    vt->rx_tail = (uint8_t)((vt->rx_tail + 1u) % VTERM_RX_BUF_SIZE);
    ylock_release(&vt->input_lock);
    return (int)(unsigned char)c;
}

bool vterm_has_char(vterm_t *vt) {
    if (!vt || !vt->in_use) return false;
    if (vt->pushback >= 0) return true;
    ylock_acquire(&vt->input_lock);
    bool any = (vt->rx_head != vt->rx_tail);
    ylock_release(&vt->input_lock);
    return any;
}

void vterm_ungetc(vterm_t *vt, char c) {
    if (!vt || !vt->in_use) return;
    vt->pushback = (int)(unsigned char)c;
}

bool vterm_check_interrupt(vterm_t *vt) {
    if (!vt || !vt->in_use) return false;
    bool intr = vt->interrupt_pending;
    vt->interrupt_pending = false;
    return intr;
}

/* --- Output routing ------------------------------------------------------- */

void vterm_write(vterm_t *vt, const char *s, uint32_t n) {
    if (!vt || !vt->in_use || !s || n == 0) return;

    if (vt->has_vt) {
        for (uint32_t i = 0; i < n; i++) {
            vtterm_putc(&vt->vt, s[i]);
        }
    }
}

void vterm_set_title(vterm_t *vt, const char *title) {
    if (!vt || !vt->in_use || !title) return;
    strncpy(vt->title, title, VTERM_TITLE_MAX - 1);
    vt->title[VTERM_TITLE_MAX - 1] = '\0';
    if (vt->has_vt) {
        vtterm_set_title(&vt->vt, vt->title, (uint32_t)strlen(vt->title));
    }
    if (vt->active) {
        console_set_title(vt->title);
    }
}

/* --- Shell worker spawning ------------------------------------------------ */

int shell_spawn_terminal(const char *title) {
    vterm_t *vt = vterm_create(title);
    if (!vt) return -1;

    int pid = task_create("lsh-worker", shell_worker_task_entry, (void *)(intptr_t)vt->id);
    if (pid < 0) {
        vterm_destroy(vt->id);
        return -1;
    }

    task_set_vterm(pid, vt->id);
    vt->owner_pid = pid;
    return vt->id;
}

/* --- Selftest ------------------------------------------------------------- */

int vterm_selftest(void) {
    int fails = 0;
#define CHECK(name, cond) do { \
    bool ok_ = (cond); \
    cprintf("  %-50s %s\n", name, ok_ ? "ok" : "FAIL"); \
    if (!ok_) fails++; \
} while (0)

    cprintf("vterm selftest:\n");
    vterm_init_subsystem();

    /* 1. Root terminal exists */
    vterm_t *v0 = vterm_get(0);
    CHECK("vterm[0] initialized as active root", v0 != NULL && v0->in_use && v0->active && v0->id == 0);

    /* 2. Create secondary vterm */
    vterm_t *v1 = vterm_create("test-vterm-1");
    CHECK("vterm_create allocates slot 1", v1 != NULL && v1->id == 1 && v1->in_use && !v1->active);
    CHECK("vterm_count reports 2", vterm_count() == 2);

    /* 3. Input queue feed and read */
    const char *test_str = "hello\n";
    uint32_t fed = vterm_feed_bytes(v1, test_str, 6);
    CHECK("vterm_feed_bytes queues 6 bytes", fed == 6 && vterm_has_char(v1));

    char out[8];
    for (int i = 0; i < 6; i++) {
        int c = vterm_getc_nonblock(v1);
        out[i] = (char)c;
    }
    out[6] = '\0';
    CHECK("vterm_getc_nonblock retrieves exact bytes", strcmp(out, "hello\n") == 0);
    CHECK("vterm_has_char empty after drain", !vterm_has_char(v1));

    /* 4. Ungetc and interrupt */
    vterm_ungetc(v1, 'Z');
    CHECK("vterm_ungetc holds char", vterm_has_char(v1) && vterm_getc_nonblock(v1) == 'Z');
    vterm_feed_char(v1, 0x03);
    CHECK("vterm_check_interrupt detects Ctrl-C", vterm_check_interrupt(v1) && !vterm_check_interrupt(v1));

    /* 5. Terminal output into shadow */
    vterm_write(v1, "VTERM1_LINE\r\n", 13);
    CHECK("vterm_write executes into vtterm", v1->has_vt && v1->vt.row >= 1);

    /* 6. Active focus switching */
    bool sw1 = vterm_set_active(1);
    CHECK("vterm_set_active(1) switches active ID", sw1 && vterm_active_id() == 1 && v1->active && !v0->active);
    bool sw0 = vterm_set_active(0);
    CHECK("vterm_set_active(0) restores root", sw0 && vterm_active_id() == 0 && v0->active && !v1->active);

    /* 7. Destroy secondary vterm */
    vterm_destroy(1);
    CHECK("vterm_destroy frees slot 1", vterm_get(1) == NULL && vterm_count() == 1);

#undef CHECK
    return fails;
}
