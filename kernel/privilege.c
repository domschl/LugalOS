/* Untrusted work: see kernel/include/kernel/privilege.h. */

#include "kernel/privilege.h"
#include "kernel/console.h"
#include "kernel/sched.h"

/* single task: the task running an agent's tool call (ea holds its session
 * claim, so there is at most one), or -1. Read by any task, which compares it
 * with its own pid: another task sees "not me" whatever the value. */
static int g_untrusted_pid = -1;
/* single task: as g_untrusted_pid. */
static int g_untrusted_depth;

void untrusted_enter(void) {
    if (g_untrusted_depth++ == 0) g_untrusted_pid = sched_current_pid();
}

void untrusted_leave(void) {
    if (g_untrusted_depth > 0 && --g_untrusted_depth == 0) g_untrusted_pid = -1;
}

bool untrusted_active(void) {
    return g_untrusted_pid >= 0 && g_untrusted_pid == sched_current_pid();
}

bool privileged_refused(const char *what) {
    if (!untrusted_active()) return false;
    cprintf("%s: refused -- it changes keys, identity, configuration or firmware, which only a "
            "person at the console may do\n", what);
    return true;
}

static bool comp_is(const char *c, unsigned n, const char *want) {
    unsigned i = 0;
    for (; i < n && want[i]; i++) {
        char a = c[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);    /* FAT names are case-insensitive */
        if (a != want[i]) return false;
    }
    return i == n && want[i] == '\0';
}

/* Walks `path` component by component -- empty and "." skipped, ".." popping
 * -- so neither "/sd0/x/../SYSTEM/./etc" nor a doubled slash slips past. */
bool privileged_path_refused(const char *path) {
    if (!untrusted_active() || !path) return false;
    enum { MAX_DEPTH = 32 };
    const char *comp[MAX_DEPTH];
    unsigned len[MAX_DEPTH], depth = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        unsigned n = (unsigned)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') { if (depth) depth--; continue; }
        if (depth == MAX_DEPTH) return privileged_refused(path);   /* too deep to judge: refuse */
        comp[depth] = s; len[depth] = n; depth++;
    }
    for (unsigned i = 0; i + 1 < depth; i++)
        if (comp_is(comp[i], len[i], "system") && comp_is(comp[i + 1], len[i + 1], "etc"))
            return privileged_refused(path);
    return false;
}
