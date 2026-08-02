#include "arch/elf.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "arch/vmm.h"
#include <string.h>

static uint8_t exec_page[4096] __attribute__((aligned(4096)));

int elf_load_and_run(const char *path) {
    if (!path) return -1;

    uint8_t file_buf[4096];
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

    /* Copy code segment to executable page */
    memset(exec_page, 0, sizeof(exec_page));
    memcpy(exec_page, file_buf + code_offset, code_size < 4096 ? code_size : 4096);

    printk("[ELF] Executing binary '%s' (Entry offset: 0x%lx, Size: %u bytes)...\n",
           path, (unsigned long)entry_point, code_size);

    /* Cast execution buffer or entry point to function pointer */
    typedef int (*entry_fn_t)(void);
    entry_fn_t entry = (entry_fn_t)(uintptr_t)exec_page;

    /* Execute native RISC-V binary */
    int ret_code = entry();
    printk("[ELF] Native RISC-V binary '%s' exited with return code: %d\n", path, ret_code);
    return ret_code;
}
