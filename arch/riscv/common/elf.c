#include "arch/elf.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "arch/vmm.h"
#include <string.h>

int elf_load_and_run(const char *path) {
    if (!path) return -1;

    static uint8_t *exec_page = NULL;
    if (!exec_page) {
        exec_page = (uint8_t *)vmm_alloc_page();
        if (!exec_page) return -1;
    }

    static uint8_t file_buf[4096];
    int bytes = vfs_read(path, file_buf, sizeof(file_buf) - 1);
    if (bytes < 5) { /* need at least magic + class byte */
        printk("[ELF Error] Failed to read ELF file '%s' (bytes=%d)\n", path, bytes);
        return -1;
    }

    if (memcmp(file_buf, ELF_MAGIC, 4) != 0) {
        printk("[ELF Error] '%s' is not a valid ELF binary (bad magic)\n", path);
        return -1;
    }

    /* --- Header validation (closes B12) ---
     *
     * Everything below this point used to be taken on trust: e_phoff indexed
     * the buffer with no bound, e_phnum controlled a loop over it, p_offset
     * became a read pointer, and `bytes - code_offset` underflowed to a huge
     * size when the offset exceeded the file. A corrupt or hostile ELF -- and
     * `exec` runs whatever is on the SD card -- could therefore read far
     * outside the 4 KB buffer.
     *
     * The file is read into a fixed buffer, so every offset is checked
     * against how much was actually read, and every addition is checked for
     * overflow before it is used rather than after. */
    const uint32_t avail = (uint32_t)bytes;

    uint8_t elf_class = file_buf[4]; /* 1: 32-bit, 2: 64-bit */
    uintptr_t entry_point = 0;
    uint32_t code_offset = 0;
    uint32_t code_size = 0;

    uint32_t ehdr_size, phdr_size, phoff, phnum, phentsize;
    uint16_t machine;

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

    if (avail < ehdr_size) {
        printk("[ELF Error] '%s' truncated: %u bytes, header needs %u\n",
               path, avail, ehdr_size);
        return -1;
    }

    if (elf_class == 1) {
        elf32_ehdr_t *e = (elf32_ehdr_t *)file_buf;
        machine = e->e_machine; entry_point = e->e_entry;
        phoff = e->e_phoff; phnum = e->e_phnum; phentsize = e->e_phentsize;
    } else {
        elf64_ehdr_t *e = (elf64_ehdr_t *)file_buf;
        machine = e->e_machine; entry_point = (uintptr_t)e->e_entry;
        /* e_phoff is 64-bit; a value past the buffer is rejected below, and
         * narrowing first would let a huge offset alias a small one. */
        if (e->e_phoff > avail) { printk("[ELF Error] '%s': program headers past end of file\n", path); return -1; }
        phoff = (uint32_t)e->e_phoff; phnum = e->e_phnum; phentsize = e->e_phentsize;
    }

    if (machine != EM_RISCV) {
        printk("[ELF Error] Unsupported machine architecture 0x%x\n", machine);
        return -1;
    }
    if (phentsize != phdr_size) {
        printk("[ELF Error] '%s': program header size %u, expected %u\n",
               path, phentsize, phdr_size);
        return -1;
    }

    /* The whole program header table must lie inside what was read. Checked
     * as a subtraction rather than phoff + phnum*size, which can wrap. */
    if (phnum > 0) {
        if (phoff > avail || (avail - phoff) / phdr_size < phnum) {
            printk("[ELF Error] '%s': program header table (%u entries at 0x%x) "
                   "does not fit in %u bytes\n", path, phnum, phoff, avail);
            return -1;
        }
    }

    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t p_type, p_off, p_filesz;
        if (elf_class == 1) {
            elf32_phdr_t *ph = (elf32_phdr_t *)(file_buf + phoff) + i;
            p_type = ph->p_type; p_off = ph->p_offset; p_filesz = ph->p_filesz;
        } else {
            elf64_phdr_t *ph = (elf64_phdr_t *)(file_buf + phoff) + i;
            p_type = ph->p_type;
            if (ph->p_offset > avail || ph->p_filesz > avail) continue; /* cannot be in range */
            p_off = (uint32_t)ph->p_offset; p_filesz = (uint32_t)ph->p_filesz;
        }
        if (p_type != PT_LOAD) continue;

        if (p_off > avail || (avail - p_off) < p_filesz) {
            printk("[ELF Error] '%s': segment %u (0x%x + %u) past end of file (%u)\n",
                   path, i, p_off, p_filesz, avail);
            return -1;
        }
        code_offset = p_off;
        code_size = p_filesz;
        break;
    }

    if (code_size == 0) {
        /* No PT_LOAD found: fall back to "everything after the header", which
         * is what a flat blob produced by this project's own `cc` looks like.
         * The subtraction is guarded -- unguarded, an offset past the end
         * underflowed to a ~4 GB size. */
        code_offset = ehdr_size;
        if (code_offset > avail) {
            printk("[ELF Error] '%s': no loadable segment and no body\n", path);
            return -1;
        }
        code_size = avail - code_offset;
    }

    if (code_size == 0) {
        printk("[ELF Error] '%s': loadable segment is empty\n", path);
        return -1;
    }
    if (code_size > PAGE_SIZE) {
        printk("[ELF Error] '%s': segment of %u bytes exceeds the %d-byte "
               "execution page\n", path, code_size, PAGE_SIZE);
        return -1;
    }

    /* Copy code segment to executable page using volatile writes */
    volatile uint8_t *dst = (volatile uint8_t *)exec_page;
    const uint8_t *src = (const uint8_t *)(file_buf + code_offset);
    uint32_t copy_len = code_size; /* already bounded to PAGE_SIZE above */

    for (uint32_t i = 0; i < copy_len; i++) {
        dst[i] = src[i];
    }

    printk("[ELF] Executing binary '%s' (Entry offset: 0x%lx, Size: %u bytes)...\n",
           path, (unsigned long)entry_point, code_size);

    /* Memory fence (rw, rw) and I-cache flush (fence.i) for RISC-V JIT execution */
    __asm__ __volatile__("fence rw, rw; .option push; .option arch, +zifencei; fence.i; .option pop" ::: "memory");

    uintptr_t run_target = (uintptr_t)exec_page;

#if defined(CONFIG_BOARD_RP2350)
    static uint8_t user_stack[16384] __attribute__((aligned(16)));
#else
    static uint8_t user_stack[65536] __attribute__((aligned(16)));
#endif
    uintptr_t stack_top = (uintptr_t)user_stack + sizeof(user_stack);


    volatile int ret_code = 0;

#if defined(CONFIG_TARGET_RV32)
    register uintptr_t r_sp __asm__("a0") = stack_top;
    register uintptr_t r_target __asm__("a1") = run_target;
    __asm__ __volatile__(
        "addi sp, sp, -8\n\t"
        "sw ra, 4(sp)\n\t"
        "sw s0, 0(sp)\n\t"
        "mv s0, sp\n\t"
        "mv sp, a0\n\t"
        "jalr ra, a1, 0\n\t"
        "mv sp, s0\n\t"
        "mv %0, a0\n\t"
        "lw ra, 4(sp)\n\t"
        "lw s0, 0(sp)\n\t"
        "addi sp, sp, 8\n\t"
        : "=r"(ret_code)
        : "r"(r_sp), "r"(r_target)
        : "memory"
    );
#else
    register uintptr_t r_sp __asm__("a0") = stack_top;
    register uintptr_t r_target __asm__("a1") = run_target;
    __asm__ __volatile__(
        "addi sp, sp, -16\n\t"
        "sd ra, 8(sp)\n\t"
        "sd s0, 0(sp)\n\t"
        "mv s0, sp\n\t"
        "mv sp, a0\n\t"
        "jalr ra, a1, 0\n\t"
        "mv sp, s0\n\t"
        "mv %0, a0\n\t"
        "ld ra, 8(sp)\n\t"
        "ld s0, 0(sp)\n\t"
        "addi sp, sp, 16\n\t"
        : "=r"(ret_code)
        : "r"(r_sp), "r"(r_target)
        : "memory"
    );
#endif

    printk("[ELF] Executable '%s' returned: %d\n", path, ret_code);
    return ret_code;
}
