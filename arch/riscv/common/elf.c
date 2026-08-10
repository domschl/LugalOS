#include "arch/elf.h"
#include "arch/umode.h"
#include "arch/vmm.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "drivers/usb_cdc.h"
#include "kernel/mem_domain.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include <string.h>

/* Loading and running a user program (B6, plan/phase5_distributed_design.md §5.4).
 *
 * ## What changed, and why it is the point of the milestone
 *
 * This loader used to copy a binary into a page and *call it*, from a
 * privileged mode, on the kernel's own stack:
 *
 *     mv sp, a0 ; jalr ra, a1, 0 ; mv sp, s0
 *
 * That is not running a program, it is inlining an untrusted byte string into
 * the kernel. `exec` runs whatever is on the SD card, so anything it loaded
 * could touch any device register, write any kernel structure, and execute
 * privileged instructions -- with hardware isolation fully implemented (B3's
 * PMP, B5's Sv39) and simply not applied to the one code path that most
 * needed it.
 *
 * A loaded program now runs as a task in U-mode under a memory domain that
 * grants it exactly three pages: its text, its data, and its stack. Nothing
 * else in the address space is reachable, and every pointer it hands to a
 * syscall is validated against that same domain by kernel/uaccess.c.
 *
 * This is also what makes the U-mode self-containment structural rather than
 * hand-maintained. kernel/shell.c's probes live in a `.utext` section and
 * must avoid, by discipline, anything the compiler might place in kernel
 * .text or .rodata -- hence the `no_sanitize`, `always_inline` and `volatile`
 * workarounds there, each of which was added after a mystery fault. A
 * separately linked program cannot have that problem: its .rodata is in its
 * own image because there is no other image to put it in.
 *
 * ## The image model, and why it is shaped by PMP
 *
 * A user image is two pages, allocated as one naturally aligned block:
 *
 *     page 0   .text + .rodata        R|X
 *     page 1   .data + .bss           R|W
 *     (separate) one page of stack    R|W
 *
 * The alignment is not tidiness. A PMP region must be power-of-two sized and
 * self-aligned (NAPOT, see kernel/mem_domain.h), so granting the two pages
 * different permissions requires them to sit inside one aligned block --
 * within which each page-sized piece is separately expressible. A merely
 * contiguous pair is not, and the loader would have had to widen a grant or
 * abandon W^X. Hence palloc_pages_aligned().
 *
 * Sv39 needs none of that; it takes the same layout anyway, per Rule 0
 * (§5.1). One image model, both builds, so the constraint is exercised
 * everywhere rather than only where it is enforced.
 *
 * The last 16 bytes of page 0 are reserved for an exit stub -- see
 * install_exit_stub() for why a program needs one.
 *
 * ## One slot per program, released when it exits (C2)
 *
 * This used to be a *single* slot, allocated once and reused, so only one user
 * program could exist at a time. That was not laziness: the Sv39 backend
 * caches a page table per domain and nothing could free a page-table tree, so
 * a domain per exec would have leaked several pages every time. Reusing one
 * slot kept the domain -- and its page table -- built exactly once.
 *
 * C2 added the missing free path (vmm_free_table, and mem_domain_destroy on
 * top of it), so a program can now own its image, its user stack and its
 * domain, and hand all three back when it ends.
 *
 * The slots are a fixed table rather than a dynamic list. A process descriptor
 * is a few dozen bytes and a list of them would want an allocator that hands
 * out sub-page objects, which this kernel deliberately does not have (see
 * kernel/palloc.h). The bound is on descriptors, not on memory: what actually
 * limits concurrent programs is pages, and on RP2350 that arrives first --
 * three pages of image and stack each, out of a fifteen-page heap.
 *
 * ## Reaping, and why it is not automatic
 *
 * A program's pages cannot be freed by the program itself; it is running on
 * them. They are freed by reap_finished(), which runs at the start of every
 * load and can be asked for explicitly. That is the same shape as the
 * scheduler's stack reaper (kernel/sched.c) and for the same reason -- the
 * one difference being that a task's kernel stack is reaped by the *next*
 * task, whereas nothing schedules on a user image, so the next loader call is
 * the natural point.
 */

#define USER_IMAGE_PAGES 2u
#define EXIT_STUB_BYTES  16u

/* Everything a user program may occupy in page 0. */
#define USER_TEXT_MAX ((uint32_t)PAGE_SIZE - EXIT_STUB_BYTES)
#define USER_IMAGE_BYTES ((uint32_t)PAGE_SIZE * USER_IMAGE_PAGES)

/* How many programs may be resident at once. The real limit is pages -- three
 * per program plus its kernel stack -- so this only has to be large enough not
 * to be the thing that binds first. */
#define USER_PROC_MAX 4

typedef struct {
    bool          in_use;
    int           pid;        /* -1 until the task exists */
    uint8_t      *image;      /* USER_IMAGE_PAGES pages, self-aligned */
    uint8_t      *ustack;     /* one page */
    uintptr_t     entry;
    uintptr_t     user_sp;    /* below the argv block, not the page top */
    uintptr_t     argv_addr;  /* a user address inside this program's stack page */
    int           argc;
    mem_domain_t  domain;
} user_proc_t;

/* Most of the stack page stays stack. A program that wants more argument
 * space than this has outgrown being invoked from a command line. */
#define USER_ARGV_MAX_BYTES 512
#define USER_ARGV_MAX_COUNT USER_ARGV_LIMIT

static user_proc_t g_procs[USER_PROC_MAX];

/* Releases one slot's pages and its domain. The caller must have established
 * that the task is dead: on the MMU build this frees the page table, and
 * doing that to a running task would pull the ground out from under it. */
static void proc_release(user_proc_t *p) {
    if (!p->in_use) return;
    mem_domain_destroy(&p->domain);
    if (p->image) palloc_free(p->image, USER_IMAGE_PAGES);
    if (p->ustack) palloc_free(p->ustack, 1);
    p->image = NULL;
    p->ustack = NULL;
    p->pid = -1;
    p->in_use = false;
}

/* Returns the resources of every program whose task has ended. See the file
 * header on why this is not automatic. */
static int reap_finished(void) {
    int reaped = 0;
    for (int i = 0; i < USER_PROC_MAX; i++) {
        user_proc_t *p = &g_procs[i];
        if (!p->in_use || p->pid < 0) continue;
        /* Asked by domain, not by pid. task_create() recycles DEAD slots, so
         * a pid recorded here can by now belong to a different, living task --
         * in which case this slot would never be reaped and its three pages
         * would be stranded for the rest of the boot. The domain pointer is
         * unique to this slot for as long as the slot is in use, so it answers
         * the question actually being asked. */
        if (sched_domain_in_use(&p->domain)) continue;
        proc_release(p);
        reaped++;
    }
    return reaped;
}

/* Lays out argc/argv at the top of the program's own stack page and leaves
 * `user_sp` below it (C3, plan/phase6_memory_and_processes.md).
 *
 * The stack page, not the data page, for two reasons. The data page holds the
 * program's .data and .bss, and a program that happens to use all of it would
 * find its globals overwritten by its own arguments -- a corruption that would
 * depend on how big the program is. And the stack page is already R|W in the
 * task's domain and already has a natural free end, which is what the classic
 * Unix layout uses it for.
 *
 * The addresses written into the vector are *user* addresses. Both memory
 * models identity-map the image and stack, so the kernel's pointer and the
 * program's pointer are the same number here -- which is worth stating rather
 * than relying on silently, because the day a model stops identity-mapping,
 * this is the function that has to change.
 *
 * Layout, growing down from the top of the page:
 *
 *     [page top]  argument strings, packed, each NUL-terminated
 *                 argv[] -- argc pointers plus a NULL terminator
 *     argv ---->
 *                 ... the program's stack, growing down from user_sp
 *
 * Returns false if the arguments do not fit, leaving the slot untouched: a
 * silently truncated argument list is a wrong answer, and the program would
 * have no way to tell.
 */
static bool build_argv(user_proc_t *p, int argc, const char *const *argv) {
    uintptr_t top = (uintptr_t)p->ustack + PAGE_SIZE;
    uintptr_t limit = top - USER_ARGV_MAX_BYTES;

    if (argc < 0) argc = 0;
    if (argc > USER_ARGV_MAX_COUNT) {
        printk("[ELF Error] %d arguments; at most %d are supported\n",
               argc, USER_ARGV_MAX_COUNT);
        return false;
    }

    /* Strings first, from the top down, so the vector below them can point at
     * their final addresses. */
    uintptr_t sp = top;
    uintptr_t slots[USER_ARGV_MAX_COUNT];
    for (int i = argc - 1; i >= 0; i--) {
        const char *a = argv && argv[i] ? argv[i] : "";
        uint32_t len = 0;
        while (a[len]) len++;
        sp -= (len + 1);
        if (sp < limit) {
            printk("[ELF Error] Arguments exceed the %d-byte argument area\n",
                   USER_ARGV_MAX_BYTES);
            return false;
        }
        memcpy((void *)sp, a, len + 1);
        slots[i] = sp;
    }

    /* Then the vector, naturally aligned. */
    sp &= ~(uintptr_t)(sizeof(uintptr_t) - 1);
    sp -= (uintptr_t)(argc + 1) * sizeof(uintptr_t);
    if (sp < limit) {
        printk("[ELF Error] Arguments exceed the %d-byte argument area\n",
               USER_ARGV_MAX_BYTES);
        return false;
    }
    uintptr_t *vec = (uintptr_t *)sp;
    for (int i = 0; i < argc; i++) vec[i] = slots[i];
    vec[argc] = 0; /* argv[argc] == NULL, as every C program is entitled to expect */

    p->argv_addr = sp;
    p->argc = argc;
    /* 16-byte aligned: the ABI's stack alignment, and the compiler is entitled
     * to assume it from the first instruction. */
    p->user_sp = sp & ~(uintptr_t)15;
    return true;
}

/* Claims a free slot and builds its image, stack and domain. */
static user_proc_t *proc_alloc(void) {
    user_proc_t *p = NULL;
    for (int i = 0; i < USER_PROC_MAX; i++) {
        if (!g_procs[i].in_use) { p = &g_procs[i]; break; }
    }
    if (!p) {
        printk("[ELF Error] All %d user process slots are in use\n", USER_PROC_MAX);
        return NULL;
    }

    p->image = (uint8_t *)palloc_pages_aligned(USER_IMAGE_PAGES, USER_IMAGE_PAGES);
    if (!p->image) {
        printk("[ELF Error] No memory for a %u-page user image\n", USER_IMAGE_PAGES);
        return NULL;
    }
    p->ustack = (uint8_t *)palloc_pages(1);
    if (!p->ustack) {
        printk("[ELF Error] No memory for a user stack\n");
        palloc_free(p->image, USER_IMAGE_PAGES);
        p->image = NULL;
        return NULL;
    }

    mem_domain_init(&p->domain);
    /* Narrowest first. The three regions do not overlap, so ordering does not
     * change the outcome here -- but PMP resolves an address against the
     * lowest-numbered matching region, and keeping to the convention means a
     * later region that *does* overlap cannot silently widen an earlier one. */
    mem_domain_add(&p->domain, (uintptr_t)p->ustack, PAGE_SIZE, MEM_R | MEM_W);
    mem_domain_add(&p->domain, (uintptr_t)p->image, PAGE_SIZE, MEM_R | MEM_X);
    mem_domain_add(&p->domain, (uintptr_t)p->image + PAGE_SIZE, PAGE_SIZE,
                   MEM_R | MEM_W);

    p->pid = -1;
    p->in_use = true;
    return p;
}

/* A compiled `main` ends by *returning*, not by calling exit -- so ra has to
 * point at something before the program's first instruction runs, and that
 * something has to be executable by U-mode and inside the task's own domain.
 * This stub is it, planted in the last 16 bytes of the text page:
 *
 *     mv   a1, a0      ; carry main's return value across
 *     li   a0, 20      ; SYS_UEXIT
 *     ecall
 *     j    .           ; unreachable; a landing pad, not a loop
 *
 * Without it the kernel had to enter U-mode with whatever ra happened to
 * hold, and a program returning normally jumped to a wild address -- reported
 * by the fault handler as a task that had misbehaved, which is exactly the
 * wrong story about a program that did the most ordinary thing possible.
 *
 * Written as encoded words rather than assembly because it is data here: it
 * is copied into a page the kernel does not execute. Both XLENs use the same
 * encodings (all three are base-ISA instructions). */
static void install_exit_stub(uint8_t *text_page) {
    static const uint32_t stub[4] = {
        0x00050593u, /* mv   a1, a0            (addi a1, a0, 0)  */
        0x01400513u, /* li   a0, 20            (addi a0, x0, 20) */
        0x00000073u, /* ecall                                     */
        0x0000006fu, /* j    .                 (jal x0, 0)        */
    };
    memcpy(text_page + PAGE_SIZE - EXIT_STUB_BYTES, stub, sizeof(stub));
}

/* One PT_LOAD segment, already narrowed to the fields this loader uses. */
typedef struct {
    uint32_t off;      /* offset in the file */
    uint32_t vaddr;    /* offset within the image */
    uint32_t filesz;
    uint32_t memsz;
} seg_t;

/* Reads and validates the ELF headers, filling `segs`. Returns the number of
 * loadable segments, or -1.
 *
 * Header validation closes B12. Everything here used to be taken on trust:
 * e_phoff indexed a buffer with no bound, e_phnum controlled a loop over it,
 * p_offset became a read pointer, and a size computed by subtraction
 * underflowed to ~4 GB when the offset exceeded the file. Every offset is now
 * checked against the real file size, and every addition is checked for
 * overflow *before* use rather than after -- `(fsize - phoff) / phdr_size <
 * phnum` instead of `phoff + phnum * phdr_size`, which can wrap. */
static int parse_headers(const char *path, const uint8_t *hdr, uint32_t hdrlen,
                         uint32_t fsize, seg_t *segs, int max_segs,
                         uintptr_t *entry_out) {
    if (hdrlen < 5 || memcmp(hdr, ELF_MAGIC, 4) != 0) {
        printk("[ELF Error] '%s' is not a valid ELF binary (bad magic)\n", path);
        return -1;
    }

    uint8_t elf_class = hdr[4]; /* 1: 32-bit, 2: 64-bit */
    uint32_t ehdr_size, phdr_size;
    if (elf_class == 1) {
        ehdr_size = sizeof(elf32_ehdr_t);
        phdr_size = sizeof(elf32_phdr_t);
    } else if (elf_class == 2) {
        ehdr_size = sizeof(elf64_ehdr_t);
        phdr_size = sizeof(elf64_phdr_t);
    } else {
        printk("[ELF Error] Invalid ELF class %d\n", elf_class);
        return -1;
    }

    if (fsize < ehdr_size || hdrlen < ehdr_size) {
        printk("[ELF Error] '%s' truncated: %u bytes, header needs %u\n",
               path, fsize, ehdr_size);
        return -1;
    }

    uint32_t phoff, phnum, phentsize;
    uint16_t machine;
    if (elf_class == 1) {
        const elf32_ehdr_t *e = (const elf32_ehdr_t *)hdr;
        machine = e->e_machine;
        *entry_out = e->e_entry;
        phoff = e->e_phoff; phnum = e->e_phnum; phentsize = e->e_phentsize;
    } else {
        const elf64_ehdr_t *e = (const elf64_ehdr_t *)hdr;
        machine = e->e_machine;
        *entry_out = (uintptr_t)e->e_entry;
        /* Range-checked before narrowing: narrowing first would let a huge
         * 64-bit offset alias a small in-range one. */
        if (e->e_phoff > fsize) {
            printk("[ELF Error] '%s': program headers past end of file\n", path);
            return -1;
        }
        phoff = (uint32_t)e->e_phoff; phnum = e->e_phnum; phentsize = e->e_phentsize;
    }

    if (machine != EM_RISCV) {
        printk("[ELF Error] Unsupported machine architecture 0x%x\n", machine);
        return -1;
    }
    if (phnum > 0 && phentsize != phdr_size) {
        printk("[ELF Error] '%s': program header size %u, expected %u\n",
               path, phentsize, phdr_size);
        return -1;
    }

    if (phnum > 0) {
        /* Must fit in the file... */
        if (phoff > fsize || (fsize - phoff) / phdr_size < phnum) {
            printk("[ELF Error] '%s': program header table (%u entries at 0x%x) "
                   "does not fit in %u bytes\n", path, phnum, phoff, fsize);
            return -1;
        }
        /* ...and in the prefix that was actually read. A linker puts the
         * program headers immediately after the ELF header; one that does not
         * is a layout this loader has never been asked to handle, so it is
         * reported as unsupported rather than silently mis-read. */
        if (phoff > hdrlen || (hdrlen - phoff) / phdr_size < phnum) {
            printk("[ELF Error] '%s': program headers at 0x%x lie beyond the "
                   "first %u bytes; unsupported layout\n", path, phoff, hdrlen);
            return -1;
        }
    }

    int nseg = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t p_type, p_off, p_filesz, p_vaddr, p_memsz;
        if (elf_class == 1) {
            const elf32_phdr_t *ph = (const elf32_phdr_t *)(hdr + phoff) + i;
            p_type = ph->p_type; p_off = ph->p_offset; p_filesz = ph->p_filesz;
            p_vaddr = ph->p_vaddr; p_memsz = ph->p_memsz;
        } else {
            const elf64_phdr_t *ph = (const elf64_phdr_t *)(hdr + phoff) + i;
            p_type = ph->p_type;
            if (ph->p_offset > fsize || ph->p_filesz > fsize ||
                ph->p_vaddr > USER_IMAGE_BYTES || ph->p_memsz > USER_IMAGE_BYTES) {
                /* Out of range in 64 bits; the checks below would be
                 * meaningless after narrowing, so decide here. */
                if (ph->p_type != PT_LOAD) continue;
                printk("[ELF Error] '%s': segment %u lies past end of file\n", path, i);
                return -1;
            }
            p_off = (uint32_t)ph->p_offset; p_filesz = (uint32_t)ph->p_filesz;
            p_vaddr = (uint32_t)ph->p_vaddr; p_memsz = (uint32_t)ph->p_memsz;
        }
        if (p_type != PT_LOAD) continue;
        if (p_memsz == 0) continue;

        if (p_off > fsize || (fsize - p_off) < p_filesz) {
            printk("[ELF Error] '%s': segment %u (0x%x + %u) past end of file (%u)\n",
                   path, i, p_off, p_filesz, fsize);
            return -1;
        }
        if (p_filesz > p_memsz) {
            printk("[ELF Error] '%s': segment %u has %u file bytes in %u memory bytes\n",
                   path, i, p_filesz, p_memsz);
            return -1;
        }
        if (nseg >= max_segs) {
            printk("[ELF Error] '%s': more than %d loadable segments\n", path, max_segs);
            return -1;
        }
        segs[nseg].off = p_off;
        segs[nseg].vaddr = p_vaddr;
        segs[nseg].filesz = p_filesz;
        segs[nseg].memsz = p_memsz;
        nseg++;
    }

    if (nseg == 0) {
        /* No loadable segment: fall back to "everything after the ELF header",
         * which is what a flat blob from this project's own `cc` looks like.
         * The subtraction is guarded -- unguarded, an offset past the end
         * underflowed to a ~4 GB size. */
        if (ehdr_size > fsize) {
            printk("[ELF Error] '%s': no loadable segment and no body\n", path);
            return -1;
        }
        segs[0].off = ehdr_size;
        segs[0].vaddr = 0;
        segs[0].filesz = fsize - ehdr_size;
        segs[0].memsz = segs[0].filesz;
        if (segs[0].memsz == 0) {
            printk("[ELF Error] '%s': loadable segment is empty\n", path);
            return -1;
        }
        nseg = 1;
    }

    return nseg;
}

/* Checks that the segment layout fits the image model described at the top of
 * this file, and in particular that nothing lands on the exit stub. */
static bool layout_fits(const char *path, const seg_t *segs, int nseg) {
    for (int i = 0; i < nseg; i++) {
        uint32_t start = segs[i].vaddr;
        uint32_t end = start + segs[i].memsz; /* both bounded above already */

        if (start > USER_IMAGE_BYTES || end > USER_IMAGE_BYTES || end < start) {
            printk("[ELF Error] '%s': segment %d (0x%x..0x%x) does not fit in the "
                   "%u-byte user image\n", path, i, start, end, USER_IMAGE_BYTES);
            return false;
        }
        /* The exit stub occupies the top of the text page. A segment crossing
         * into it would be silently overwritten, so refuse instead. */
        if (start < (uint32_t)PAGE_SIZE && end > USER_TEXT_MAX) {
            printk("[ELF Error] '%s': segment %d ends at 0x%x, past the %u-byte "
                   "text limit (the last %u bytes hold the exit stub)\n",
                   path, i, end, USER_TEXT_MAX, EXIT_STUB_BYTES);
            return false;
        }
    }
    return true;
}

/* The task body. Installs the domain, then leaves for U-mode and does not
 * come back: the task ends either at the exit stub or at a fault. */
static void user_program_body(void *arg) {
    user_proc_t *p = (user_proc_t *)arg;
    if (!p || !p->in_use) return;

    if (task_set_domain(sched_current_pid(), &p->domain) != 0) {
        /* The hardware did not install the domain as written, so nothing here
         * knows what this program would actually be allowed to touch.
         * Refusing is the only honest option: entering U-mode anyway would
         * run untrusted code under a restriction nobody verified. */
        printk("[ELF Error] Memory domain not installed as written; "
               "refusing to run the program\n");
        return;
    }

    arch_enter_user((void (*)(void))p->entry,
                    p->user_sp,
                    (uintptr_t)p->image + PAGE_SIZE - EXIT_STUB_BYTES,
                    (uintptr_t)p->argc, p->argv_addr);
}

/* Loads `path` into a fresh slot and starts it. Returns the slot, or NULL --
 * having released anything it had claimed. Does not wait. */
static user_proc_t *load_into_slot(const char *path, int argc,
                                   const char *const *argv) {
    if (!path) return NULL;

    /* Before claiming anything: a finished program's three pages are exactly
     * what this load is about to ask for, and on RP2350's fifteen-page heap
     * that is the difference between the second program starting and not. */
    reap_finished();

    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        printk("[ELF Error] Failed to open '%s'\n", path);
        return NULL;
    }

    vfs_stat_t st;
    if (vfs_fstat(fd, &st) < 0) {
        printk("[ELF Error] Failed to stat '%s'\n", path);
        vfs_close(fd);
        return NULL;
    }
    uint32_t fsize = st.size;

    /* Big enough for an ELF header plus a normal program header table, and
     * static because it is far too large for a task's kernel stack. Read and
     * fully consumed before any task is created, so concurrent programs do not
     * make this shared buffer a hazard: loading is done from the caller's
     * thread of control, one at a time. */
    static uint8_t hdrbuf[1024];
    uint32_t want = fsize < sizeof(hdrbuf) ? fsize : (uint32_t)sizeof(hdrbuf);
    int got = vfs_pread(fd, hdrbuf, want, 0);
    if (got < 0 || (uint32_t)got != want) {
        printk("[ELF Error] Failed to read the header of '%s' (%d of %u bytes)\n",
               path, got, want);
        vfs_close(fd);
        return NULL;
    }

    seg_t segs[4];
    uintptr_t entry_off = 0;
    int nseg = parse_headers(path, hdrbuf, (uint32_t)got, fsize, segs,
                             (int)(sizeof(segs) / sizeof(segs[0])), &entry_off);
    if (nseg < 0 || !layout_fits(path, segs, nseg)) {
        vfs_close(fd);
        return NULL;
    }

    if (entry_off > USER_TEXT_MAX) {
        printk("[ELF Error] '%s': entry point 0x%lx is outside the text page\n",
               path, (unsigned long)entry_off);
        vfs_close(fd);
        return NULL;
    }

    user_proc_t *p = proc_alloc();
    if (!p) {
        vfs_close(fd);
        return NULL;
    }

    /* Zeroed before loading. palloc_pages() already returns zeroed memory, so
     * this is belt and braces for the .bss of a segment whose memsz exceeds
     * its filesz -- and it stays because the guarantee that matters is "a
     * program never sees the previous one's data", which should not depend on
     * a property of the allocator two layers away. */
    memset(p->image, 0, USER_IMAGE_BYTES);

    for (int i = 0; i < nseg; i++) {
        if (segs[i].filesz == 0) continue; /* .bss-only: the zeroing above is it */
        int n = vfs_pread(fd, p->image + segs[i].vaddr, segs[i].filesz, segs[i].off);
        if (n < 0 || (uint32_t)n != segs[i].filesz) {
            printk("[ELF Error] '%s': segment %d read %d of %u bytes\n",
                   path, i, n, segs[i].filesz);
            vfs_close(fd);
            proc_release(p);
            return NULL;
        }
    }
    vfs_close(fd);

    install_exit_stub(p->image);

    /* The kernel just wrote instructions through the data path. Without this
     * the core may execute whatever the instruction fetch already had cached
     * for those addresses -- most visibly, the *previous* program. */
    __asm__ __volatile__(
        "fence rw, rw; .option push; .option arch, +zifencei; fence.i; .option pop"
        ::: "memory");

    p->entry = (uintptr_t)p->image + entry_off;

    if (!build_argv(p, argc, argv)) {
        proc_release(p);
        return NULL;
    }

    printk("[ELF] '%s': %d segment(s), %d arg(s), entry 0x%lx -> U-mode task\n",
           path, nseg, p->argc, (unsigned long)entry_off);

    p->pid = task_create("uprog", user_program_body, p);
    if (p->pid < 0) {
        printk("[ELF Error] Could not create a task for '%s'\n", path);
        proc_release(p);
        return NULL;
    }

    /* Bound here rather than only in the task body, which is what makes the
     * reaper's question answerable: between task_create() and the body's first
     * instruction the task exists but has no domain, and a reaper asking "does
     * any live task use this domain?" would say no and free the image out from
     * under a program that had not started yet. The body still sets it -- that
     * call is where the *verification* lives, and it must run on the task's own
     * hart to be meaningful. */
    task_set_domain(p->pid, &p->domain);
    return p;
}

int elf_spawn(const char *path) {
    return elf_spawn_argv(path, 0, NULL);
}

int elf_spawn_argv(const char *path, int argc, const char *const *argv) {
    user_proc_t *p = load_into_slot(path, argc, argv);
    return p ? p->pid : -1;
}

int elf_load_and_run(const char *path) {
    return elf_load_and_run_argv(path, 0, NULL);
}

int elf_load_and_run_argv(const char *path, int argc, const char *const *argv) {
    user_proc_t *p = load_into_slot(path, argc, argv);
    if (!p) return -1;
    int pid = p->pid;

    /* Wait for it. Bounded, so a program that never ends cannot wedge the
     * shell silently -- and reported honestly when the bound is hit, because
     * the slot really is still in use at that point. */
    const uint32_t max_yields = 2000000u;
    uint32_t spins = 0;
    while (sched_task_state(pid) != TASK_DEAD && spins < max_yields) {
        /* Keep the console alive while the program runs.
         *
         * On RP2350 the console is USB, and USB is serviced by polling from
         * whatever happens to be busy-waiting -- the line editor, the UART
         * driver, time_delay_us(). A user program that computes for a while
         * enters the kernel only on its own syscalls and on timer ticks, and
         * this loop is the only other thing running, so without the poll here
         * nothing drains the CDC TX ring for the duration. The symptom is
         * specific and was initially mistaken for a preemption failure: a
         * spinning program's own output never appeared at all, while a
         * short-lived one's did. A no-op on the QEMU targets. */
        usb_cdc_task();
        sched_yield();
        spins++;
    }

    if (sched_task_state(pid) != TASK_DEAD) {
        /* Left running, and its slot left claimed -- which is now a statement
         * about one program rather than about the whole machine: other
         * programs can still be loaded, and this one's pages come back at the
         * next load once its task finally ends. */
        printk("[ELF] '%s' is still running after %u yields; leaving it\n",
               path, max_yields);
        return -1;
    }

    long status = 0;
    if (!sched_task_exited_cleanly(pid, &status)) {
        /* The trap handler has already said what the fault was. Saying so
         * again here is what distinguishes it from a program that returned 0:
         * an exit status alone cannot, since 0 is an ordinary return value. */
        cprintf("[ELF] '%s' was terminated before it could exit\n", path);
        return -1;
    }

    /* The console, not the log: this is the answer to a command someone
     * typed. `klog detach console` must not swallow a program's exit status
     * (C0). */
    cprintf("[ELF] '%s' returned %ld\n", path, status);
    return (int)status;
}
