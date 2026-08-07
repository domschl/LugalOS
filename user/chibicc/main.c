/*
 * chibicc - Main Entry Point & LugalOS VFS Integration
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "arch/elf.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include <string.h>

#define CHIBICC_BUF_SIZE 4096

int chibicc_compile(const char *src_path, const char *dst_elf_path) {
    if (!src_path || !dst_elf_path) return -1;

    static char src_buf[CHIBICC_BUF_SIZE];
    int bytes = vfs_read(src_path, src_buf, sizeof(src_buf) - 1);
    if (bytes <= 0) {
        printk("[chibicc Error] Failed to read C source file '%s'\n", src_path);
        return -1;
    }
    src_buf[bytes] = '\0';

    printk("[chibicc] Preprocessing '%s'...\n", src_path);
    char *preprocessed_src = preprocess(src_buf);

    printk("[chibicc] Tokenizing '%s'...\n", src_path);
    Token *tok = tokenize(preprocessed_src);
    if (!tok) {
        printk("[chibicc Error] Tokenization failed\n");
        return -1;
    }

    printk("[chibicc] Parsing C AST...\n");
    Function *prog = parse(tok);
    if (!prog) {
        printk("[chibicc Error] Parsing failed\n");
        return -1;
    }
    if (chibicc_pool_exhausted) {
        printk("[chibicc Error] Compiler pool exhausted while compiling '%s'; "
               "refusing to emit a possibly-corrupted binary\n", src_path);
        return -1;
    }
    printk("[chibicc] AST parsed successfully!\n");

    static uint8_t elf_buf[CHIBICC_BUF_SIZE];
    memset(elf_buf, 0, sizeof(elf_buf));

#if defined(CONFIG_TARGET_RV64)
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)elf_buf;
    memcpy(ehdr->e_ident, ELF_MAGIC, 4);
    ehdr->e_ident[4] = 2; // ELFCLASS64
    ehdr->e_ident[5] = 1; // ELFDATA2LSB
    ehdr->e_ident[6] = 1; // EV_CURRENT
    ehdr->e_type = 2;     // ET_EXEC
    ehdr->e_machine = EM_RISCV;
    ehdr->e_version = 1;
    ehdr->e_entry = 0;
    ehdr->e_phoff = sizeof(elf64_ehdr_t);
    ehdr->e_ehsize = sizeof(elf64_ehdr_t);
    ehdr->e_phentsize = sizeof(elf64_phdr_t);
    ehdr->e_phnum = 1;

    elf64_phdr_t *phdr = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    phdr->p_type = PT_LOAD;
    phdr->p_flags = 7; // RWX
    phdr->p_offset = sizeof(elf64_ehdr_t) + sizeof(elf64_phdr_t);
    phdr->p_vaddr = 0;
    phdr->p_paddr = 0;

    int code_start = sizeof(elf64_ehdr_t) + sizeof(elf64_phdr_t);
#else
    elf32_ehdr_t *ehdr = (elf32_ehdr_t *)elf_buf;
    memcpy(ehdr->e_ident, ELF_MAGIC, 4);
    ehdr->e_ident[4] = 1; // ELFCLASS32
    ehdr->e_ident[5] = 1; // ELFDATA2LSB
    ehdr->e_ident[6] = 1; // EV_CURRENT
    ehdr->e_type = 2;     // ET_EXEC
    ehdr->e_machine = EM_RISCV;
    ehdr->e_version = 1;
    ehdr->e_entry = 0;
    ehdr->e_phoff = sizeof(elf32_ehdr_t);
    ehdr->e_ehsize = sizeof(elf32_ehdr_t);
    ehdr->e_phentsize = sizeof(elf32_phdr_t);
    ehdr->e_phnum = 1;

    elf32_phdr_t *phdr = (elf32_phdr_t *)(elf_buf + sizeof(elf32_ehdr_t));
    phdr->p_type = PT_LOAD;
    phdr->p_flags = 7; // RWX
    phdr->p_offset = sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t);
    phdr->p_vaddr = 0;
    phdr->p_paddr = 0;

    int code_start = sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t);
#endif

    printk("[chibicc] Generating RISC-V Machine Code...\n");
    int code_len = codegen(prog, elf_buf + code_start, CHIBICC_BUF_SIZE - code_start);
    int total_elf_size = code_start + code_len;

    phdr->p_filesz = code_len;
    phdr->p_memsz = code_len;

    /* Write generated ELF binary to disk via VFS */
    printk("[chibicc] Writing ELF binary to VFS...\n");
    vfs_write(dst_elf_path, elf_buf, total_elf_size);
    printk("[chibicc] Build clean: generated %d-byte RISC-V ELF binary at '%s'\n",
           total_elf_size, dst_elf_path);
    return 0;
}
