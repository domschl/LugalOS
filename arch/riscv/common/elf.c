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
    int bytes = vfs_read(path, file_buf, sizeof(file_buf));
    if (bytes < 16) {
        printk("[ELF Error] Failed to read ELF file '%s' (bytes=%d)\n", path, bytes);
        return -1;
    }

    if (memcmp(file_buf, ELF_MAGIC, 4) != 0) {
        printk("[ELF Error] '%s' is not a valid ELF binary (bad magic)\n", path);
        return -1;
    }

    uint8_t elf_class = file_buf[4]; // 1: 32-bit, 2: 64-bit
    uintptr_t entry_point = 0;
    uint32_t code_offset = 0;
    uint32_t code_size = 0;

    if (elf_class == 1) { // ELF32
        elf32_ehdr_t *ehdr = (elf32_ehdr_t *)file_buf;
        if (ehdr->e_machine != EM_RISCV) {
            printk("[ELF Error] Unsupported machine architecture 0x%x\n", ehdr->e_machine);
            return -1;
        }
        entry_point = ehdr->e_entry;

        if (ehdr->e_phnum > 0) {
            elf32_phdr_t *phdr = (elf32_phdr_t *)(file_buf + ehdr->e_phoff);
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                    code_offset = phdr[i].p_offset;
                    code_size = phdr[i].p_filesz;
                    break;
                }
            }
        }
    } else if (elf_class == 2) { // ELF64
        elf64_ehdr_t *ehdr = (elf64_ehdr_t *)file_buf;
        if (ehdr->e_machine != EM_RISCV) {
            printk("[ELF Error] Unsupported machine architecture 0x%x\n", ehdr->e_machine);
            return -1;
        }
        entry_point = (uintptr_t)ehdr->e_entry;

        if (ehdr->e_phnum > 0) {
            elf64_phdr_t *phdr = (elf64_phdr_t *)(file_buf + ehdr->e_phoff);
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                    code_offset = (uint32_t)phdr[i].p_offset;
                    code_size = (uint32_t)phdr[i].p_filesz;
                    break;
                }
            }
        }
    } else {
        printk("[ELF Error] Invalid ELF class %d\n", elf_class);
        return -1;
    }

    if (code_size == 0) {
        code_offset = (elf_class == 1) ? sizeof(elf32_ehdr_t) : sizeof(elf64_ehdr_t);
        code_size = bytes - code_offset;
    }

    /* Copy code segment to executable page using volatile writes */
    volatile uint8_t *dst = (volatile uint8_t *)exec_page;
    const uint8_t *src = (const uint8_t *)(file_buf + code_offset);
    uint32_t copy_len = code_size < 4096 ? code_size : 4096;

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
