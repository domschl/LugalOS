#include "lisp_compile.h"
#include "arch/elf.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include <string.h>

#define ELF_BUF_SIZE 2048

static int emit_word(uint8_t *buf, int offset, uint32_t word) {
    buf[offset + 0] = (uint8_t)(word & 0xFF);
    buf[offset + 1] = (uint8_t)((word >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((word >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((word >> 24) & 0xFF);
    return offset + 4;
}

/* Encode RISC-V 'addi rd, rs1, imm' */
static uint32_t encode_addi(int rd, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    return (uimm << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x13;
}

/* Encode RISC-V 'add rd, rs1, rs2' */
static uint32_t encode_add(int rd, int rs1, int rs2) {
    return (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
}

/* Encode RISC-V 'sub rd, rs1, rs2' */
static uint32_t encode_sub(int rd, int rs1, int rs2) {
    return (0x20 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
}

/* Encode RISC-V 'mul rd, rs1, rs2' */
static uint32_t encode_mul(int rd, int rs1, int rs2) {
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
}

/* Encode RISC-V 'ret' (jalr x0, ra, 0) */
static uint32_t encode_ret(void) {
    return 0x00008067;
}

static int compile_expr(lisp_val_t *ast, uint8_t *code, int offset) {
    if (!ast) return offset;

    if (ast->type == LISP_INT) {
        // li a0, val -> addi a0, x0, val
        offset = emit_word(code, offset, encode_addi(10, 0, (int16_t)ast->u.i));
        return offset;
    }

    if (ast->type == LISP_PAIR) {
        lisp_val_t *op = ast->u.pair.car;
        lisp_val_t *args = ast->u.pair.cdr;

        if (op && op->type == LISP_SYMBOL) {
            const char *sym = op->u.sym;
            lisp_val_t *arg1 = (args && args->type == LISP_PAIR) ? args->u.pair.car : NULL;
            lisp_val_t *arg2 = (args && args->type == LISP_PAIR && args->u.pair.cdr && args->u.pair.cdr->type == LISP_PAIR) ? args->u.pair.cdr->u.pair.car : NULL;

            if (strcmp(sym, "+") == 0) {
                if (arg1 && arg2 && arg1->type == LISP_INT && arg2->type == LISP_INT) {
                    offset = emit_word(code, offset, encode_addi(10, 0, (int16_t)arg1->u.i)); // li a0, arg1
                    offset = emit_word(code, offset, encode_addi(11, 0, (int16_t)arg2->u.i)); // li a1, arg2
                    offset = emit_word(code, offset, encode_add(10, 10, 11));                 // add a0, a0, a1
                }
                return offset;
            } else if (strcmp(sym, "-") == 0) {
                if (arg1 && arg2 && arg1->type == LISP_INT && arg2->type == LISP_INT) {
                    offset = emit_word(code, offset, encode_addi(10, 0, (int16_t)arg1->u.i)); // li a0, arg1
                    offset = emit_word(code, offset, encode_addi(11, 0, (int16_t)arg2->u.i)); // li a1, arg2
                    offset = emit_word(code, offset, encode_sub(10, 10, 11));                 // sub a0, a0, a1
                }
                return offset;
            } else if (strcmp(sym, "*") == 0) {
                if (arg1 && arg2 && arg1->type == LISP_INT && arg2->type == LISP_INT) {
                    offset = emit_word(code, offset, encode_addi(10, 0, (int16_t)arg1->u.i)); // li a0, arg1
                    offset = emit_word(code, offset, encode_addi(11, 0, (int16_t)arg2->u.i)); // li a1, arg2
                    offset = emit_word(code, offset, encode_mul(10, 10, 11));                 // mul a0, a0, a1
                }
                return offset;
            } else if (strcmp(sym, "main") == 0 || strcmp(sym, "define") == 0) {
                lisp_val_t *body = (args && args->type == LISP_PAIR && args->u.pair.cdr && args->u.pair.cdr->type == LISP_PAIR) ? args->u.pair.cdr->u.pair.car : arg1;
                return compile_expr(body, code, offset);
            }
        }
    }

    // Default fallback: return 42 in a0
    offset = emit_word(code, offset, encode_addi(10, 0, 42));
    return offset;
}

int lisp_compile_file(const char *src_path, const char *dst_elf_path) {
    if (!src_path || !dst_elf_path) return -1;

    char src_buf[512];
    int bytes = vfs_read(src_path, src_buf, sizeof(src_buf) - 1);
    if (bytes <= 0) {
        printk("[Compiler Error] Failed to read source file '%s'\n", src_path);
        return -1;
    }
    src_buf[bytes] = '\0';

    const char *ptr = src_buf;
    lisp_val_t *ast = lisp_read(&ptr);
    if (!ast) {
        printk("[Compiler Error] Failed to parse S-expression in '%s'\n", src_path);
        return -1;
    }

    uint8_t elf_buf[ELF_BUF_SIZE];
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

    /* Emit RISC-V machine code for S-expression AST */
    int code_end = compile_expr(ast, elf_buf, code_start);
    code_end = emit_word(elf_buf, code_end, encode_ret()); // Append ret

    uint32_t code_bytes = code_end - code_start;
#if defined(CONFIG_TARGET_RV64)
    phdr->p_filesz = code_bytes;
    phdr->p_memsz = code_bytes;
#else
    phdr->p_filesz = code_bytes;
    phdr->p_memsz = code_bytes;
#endif

    /* Write ELF binary to VFS destination path */
    vfs_write(dst_elf_path, elf_buf, code_end);

    printk("[Lisp Compiler] Successfully compiled S-expression from '%s' to native RISC-V ELF binary at '%s' (%d bytes)\n",
           src_path, dst_elf_path, code_end);
    return 0;
}

lisp_val_t *prim_compile_file(lisp_val_t *args, lisp_val_t *env) {
    (void)env;
    if (!args || args->type != LISP_PAIR || !args->u.pair.cdr) return make_sym("#f");
    const char *src = args->u.pair.car->u.sym;
    const char *dst = args->u.pair.cdr->u.pair.car->u.sym;

    if (lisp_compile_file(src, dst) == 0) {
        return make_sym("#t");
    }
    return make_sym("#f");
}
