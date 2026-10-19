#include "kernel/sysreg.h"
#include "kernel/lock.h"

/* guarded by: itself -- the one lock every read-modify-write of a shared
 * system word takes (kernel/sysreg.h). A leaf: nothing is called under it. */
static spinlock_t g_sysreg_lock;

uintptr_t sysreg_lock(void) { return spin_lock_irqsave(&g_sysreg_lock); }
void sysreg_unlock(uintptr_t flags) { spin_unlock_irqrestore(&g_sysreg_lock, flags); }

void sysreg_update(uintptr_t addr, uint32_t clear, uint32_t set) {
    volatile uint32_t *r = (volatile uint32_t *)addr;
    uintptr_t f = spin_lock_irqsave(&g_sysreg_lock);
    *r = (*r & ~clear) | set;
    spin_unlock_irqrestore(&g_sysreg_lock, f);
}
