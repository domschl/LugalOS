/*
 * chibicc - RISC-V Code Generator with String Literal (.rodata) PC-Relative Address Generation
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "kernel/printk.h"
#include <string.h>

#if defined(CONFIG_TARGET_RV64)
#define REG_SZ 8
#else
#define REG_SZ 4
#endif

static int code_idx = 0;

/* Phase 40 review: codegen checks what it emits. It used to ignore the size
 * of its output buffer -- `(void)max_size` -- so a program whose code passed
 * 4 KB was written on past the end, into the kernel heap; and every constant,
 * stack offset and branch was cast to its instruction field whatever its
 * value, so `return 5000;` returned 904. Each such case now fails the
 * compile, and nothing is written outside the buffer. */
static int  g_cg_max;       /* bytes code_buf holds */
static bool g_cg_failed;
static int  g_cg_depth;     /* gen_expr()/gen_stmt() recursion */

static void cg_fail(const char *what) {
    if (!g_cg_failed) printk("[chibicc Error] %s\n", what);
    g_cg_failed = true;
}

static bool fits12(long v) { return v >= -2048 && v <= 2047; }

/* A constant lui+addi(w) can load: any long on RV32, an int on RV64. */
static bool fits32(long v) {
#if defined(CONFIG_TARGET_RV64)
    return v >= -2147483647L - 1 && v <= 2147483647L;
#else
    (void)v;
    return true;
#endif
}

/* `v` as a 12-bit immediate, or a failed compile saying `what`. */
static int16_t imm12(long v, const char *what) {
    if (!fits12(v)) { cg_fail(what); return 0; }
    return (int16_t)v;
}

#define FRAME_ERR "a stack frame or variable offset beyond 2 KB (cc addresses locals with 12-bit offsets)"

static int emit_word(uint8_t *buf, int offset, uint32_t word) {
    if (offset < 0 || offset > g_cg_max - 4) {
        cg_fail("program too large: its code does not fit cc's output buffer");
        return offset + 4;   /* sizes stay right, so the passes still agree */
    }
    buf[offset + 0] = (uint8_t)(word & 0xFF);
    buf[offset + 1] = (uint8_t)((word >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((word >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((word >> 24) & 0xFF);
    return offset + 4;
}

static uint32_t encode_lui(int rd, int32_t imm20) {
    uint32_t uimm = (uint32_t)imm20 & 0xFFFFF;
    return (uimm << 12) | (rd << 7) | 0x37;
}

static uint32_t encode_auipc(int rd, int32_t imm20) {
    uint32_t uimm = (uint32_t)imm20 & 0xFFFFF;
    return (uimm << 12) | (rd << 7) | 0x17;
}

static uint32_t encode_addi(int rd, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    return (uimm << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x13;
}

static uint32_t encode_add(int rd, int rs1, int rs2) {
    return (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_sub(int rd, int rs1, int rs2) {
    return (0x20 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_mul(int rd, int rs1, int rs2) {
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_div(int rd, int rs1, int rs2) {
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_rem(int rd, int rs1, int rs2) {
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x6 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_store(int rs2, int rs1, int16_t imm, int sz) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    uint32_t imm11_5 = (uimm >> 5) & 0x7F;
    uint32_t imm4_0 = uimm & 0x1F;
    uint8_t funct3 = 0x2; // Default 32-bit (sw)
    if (sz == 1) funct3 = 0x0;       // sb
    else if (sz == 2) funct3 = 0x1;  // sh
    else if (sz == 4) funct3 = 0x2;  // sw
    else if (sz == 8) {
#if defined(CONFIG_TARGET_RV64)
        funct3 = 0x3; // sd (RV64)
#else
        funct3 = 0x2; // sw (RV32 fallback)
#endif
    }
    return (imm11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (imm4_0 << 7) | 0x23;
}

static uint32_t encode_load(int rd, int rs1, int16_t imm, int sz) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    uint8_t funct3 = 0x2; // Default 32-bit (lw)
    if (sz == 1) funct3 = 0x0;       // lb
    else if (sz == 2) funct3 = 0x1;  // lh
    else if (sz == 4) funct3 = 0x2;  // lw
    else if (sz == 8) {
#if defined(CONFIG_TARGET_RV64)
        funct3 = 0x3; // ld (RV64)
#else
        funct3 = 0x2; // lw (RV32 fallback)
#endif
    }
    return (uimm << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x03;
}

static uint32_t encode_ret(void) {
    return 0x00008067;
}

static uint32_t encode_slt(int rd, int rs1, int rs2) {
    return (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x2 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_sltu(int rd, int rs1, int rs2) {
    return (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x3 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_xori(int rd, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    return (uimm << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | 0x13;
}

static uint32_t encode_beqz(int rs1, int offset) {
    if (offset < -4096 || offset > 4094) cg_fail("a branch farther than 4 KB");
    uint32_t uoff = (uint32_t)offset;
    uint32_t b12   = (uoff >> 12) & 0x1;
    uint32_t b11   = (uoff >> 11) & 0x1;
    uint32_t b10_5 = (uoff >> 5)  & 0x3F;
    uint32_t b4_1  = (uoff >> 1)  & 0xF;
    return (b12 << 31) | (b10_5 << 25) | (0 << 20) | (rs1 << 15) | (0x0 << 12) | (b4_1 << 8) | (b11 << 7) | 0x63;
}

static uint32_t encode_jal(int rd, int32_t offset) {
    uint32_t uoff = (uint32_t)offset & 0x1FFFFF;
    uint32_t j20 = (uoff >> 20) & 0x1;
    uint32_t j10_1 = (uoff >> 1) & 0x3FF;
    uint32_t j11 = (uoff >> 11) & 0x1;
    uint32_t j19_12 = (uoff >> 12) & 0xFF;
    return (j20 << 31) | (j10_1 << 21) | (j11 << 20) | (j19_12 << 12) | (rd << 7) | 0x6F;
}

static void gen_expr(Node *node, uint8_t *code_buf);
static void gen_stmt(Node *node, uint8_t *code_buf);

static void gen_addr(Node *node, uint8_t *code_buf) {
    if (node->kind == ND_VAR) {
        code_idx = emit_word(code_buf, code_idx, encode_addi(10, 8, imm12(node->var->offset, FRAME_ERR)));
        return;
    }
    if (node->kind == ND_DEREF) {
        gen_expr(node->lhs, code_buf);
        return;
    }
    if (node->kind == ND_MEMBER) {
        gen_addr(node->lhs, code_buf);
        if (node->member) {
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 10, imm12(node->member->offset, "a struct member beyond 2 KB")));
        }
        return;
    }
    cg_fail("not an lvalue: cannot take its address or assign to it");
}

static Function *global_prog = NULL;

/* True only while the final (emitting) pass runs. The dry passes exist to
 * converge function offsets, so a diagnostic raised in one of them is
 * reporting on offsets that are not settled yet. */
static bool g_final_pass = false;

static int break_jals[16];
static int break_cnt = 0;
static int loop_depth = 0;

static void gen_expr_inner(Node *node, uint8_t *code_buf);

/* The AST is walked recursively on the kernel stack. The parser bounds how
 * deeply it nests calls, but a left-associative chain -- 1+1+...+1 -- is
 * parsed by a loop and still builds a tree as deep as it is long. */
static void gen_expr(Node *node, uint8_t *code_buf) {
    if (g_cg_depth >= 2 * CHIBICC_MAX_NEST) { cg_fail("expression nested too deeply"); return; }
    g_cg_depth++;
    gen_expr_inner(node, code_buf);
    g_cg_depth--;
}

static void gen_expr_inner(Node *node, uint8_t *code_buf) {
    if (!node) return;

    switch (node->kind) {
        case ND_NUM: {
            long v = node->val;
            if (fits12(v)) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, (int16_t)v));
            } else if (fits32(v)) {
                /* lui + addi, in 32-bit arithmetic: on RV32 a literal up to
                 * 0xFFFFFFFF is that bit pattern; on RV64 addiw sign-extends
                 * the 32-bit sum, which is what makes lui's own sign
                 * extension come out right at the top of the int range. */
                int32_t v32 = (int32_t)(uint32_t)(unsigned long)v;
                int32_t hi = (int32_t)(((int64_t)v32 + 0x800) >> 12);
                int16_t lo = (int16_t)((int64_t)v32 - (int64_t)hi * 4096);
                code_idx = emit_word(code_buf, code_idx, encode_lui(10, hi));
#if defined(CONFIG_TARGET_RV64)
                code_idx = emit_word(code_buf, code_idx,
                                     encode_addi(10, 10, lo) ^ 0x13u ^ 0x1Bu);   /* addiw */
#else
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 10, lo));
#endif
            } else {
                cg_fail("a constant outside cc's 32-bit range");
            }
            return;
        }
        case ND_STR: {
            int pc = code_idx;
            int target = node->var->offset;
            int diff = target - pc;
            int32_t hi = (diff + 0x800) >> 12;
            int16_t lo = (int16_t)(diff - hi * 4096);
            code_idx = emit_word(code_buf, code_idx, encode_auipc(10, hi));
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 10, lo));
            return;
        }
        case ND_VAR: {
            int sz = (node->var && node->var->ty) ? node->var->ty->size : 4;
            if (node->var && node->var->ty && node->var->ty->kind == TY_PTR) {
                sz = REG_SZ;
            }
            if (node->var && node->var->ty && (node->var->ty->kind == TY_ARRAY || node->var->ty->kind == TY_STRUCT)) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 8, imm12(node->var->offset, FRAME_ERR)));
            } else {
                code_idx = emit_word(code_buf, code_idx, encode_load(10, 8, imm12(node->var->offset, FRAME_ERR), sz));
            }
            return;
        }
        case ND_ADDR:
            gen_addr(node->lhs, code_buf);
            return;
        case ND_DEREF: {
            int sz = 4;
            if (node->lhs && node->lhs->ty && node->lhs->ty->base) {
                sz = node->lhs->ty->base->size;
            } else if (node->lhs && (node->lhs->kind == ND_ADD || node->lhs->kind == ND_SUB)) {
                if (node->lhs->lhs && node->lhs->lhs->kind == ND_VAR && node->lhs->lhs->var && node->lhs->lhs->var->ty && node->lhs->lhs->var->ty->base) {
                    sz = node->lhs->lhs->var->ty->base->size;
                }
            }
            gen_expr(node->lhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_load(10, 10, 0, sz));
            return;
        }
        case ND_MEMBER: {
            int sz = (node->member && node->member->ty) ? node->member->ty->size : 4;
            gen_addr(node, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_load(10, 10, 0, sz));
            return;
        }
        case ND_ASSIGN:
            if (node->lhs->kind == ND_VAR) {
                int sz = (node->lhs->var && node->lhs->var->ty) ? node->lhs->var->ty->size : 4;
                if (node->lhs->var && node->lhs->var->ty && node->lhs->var->ty->kind == TY_PTR) sz = REG_SZ;
                gen_expr(node->rhs, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_store(10, 8, imm12(node->lhs->var->offset, FRAME_ERR), sz));
            } else if (node->lhs->kind == ND_DEREF || node->lhs->kind == ND_MEMBER) {
                int sz = 4;
                if (node->lhs->kind == ND_MEMBER && node->lhs->member && node->lhs->member->ty) {
                    sz = node->lhs->member->ty->size;
                } else if (node->lhs->kind == ND_DEREF && node->lhs->lhs && node->lhs->lhs->ty && node->lhs->lhs->ty->base) {
                    sz = node->lhs->lhs->ty->base->size;
                }
                gen_expr(node->rhs, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -16));
                code_idx = emit_word(code_buf, code_idx, encode_store(10, 2, 0, REG_SZ));

                gen_addr(node->lhs, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_load(11, 2, 0, REG_SZ));
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 16));

                code_idx = emit_word(code_buf, code_idx, encode_store(11, 10, 0, sz));
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 11, 0));
            }
            return;
        case ND_FUNCALL: {
            int arg_cnt = 0;
            for (Node *arg = node->args; arg; arg = arg->next) {
                gen_expr(arg, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -16));
                code_idx = emit_word(code_buf, code_idx, encode_store(10, 2, 0, REG_SZ));
                arg_cnt++;
            }
            if (arg_cnt > 8) cg_fail("a call with more than 8 arguments");
            for (int i = arg_cnt - 1; i >= 0 && i < 8; i--) {
                code_idx = emit_word(code_buf, code_idx, encode_load(10 + i, 2, 0, REG_SZ));
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 16));
            }

            if (strcmp(node->funcname, "lugal_syscall") == 0 || strcmp(node->funcname, "syscall") == 0) {
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // RISC-V ecall instruction
                return;
            }
            if (strcmp(node->funcname, "print") == 0 || strcmp(node->funcname, "puts") == 0 || strcmp(node->funcname, "printf") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, 10)); // a0 = 10 (SYS_PRINT)
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }
            if (strcmp(node->funcname, "putnum") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, 11)); // a0 = 11 (SYS_PUTNUM)
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }
            /* read_file(path, buf, max) and write_file(path, buf, len),
             * which lugal.h declares: SYS_READ_FILE (13) / SYS_WRITE_FILE
             * (14) with the three arguments one register up. They used to
             * fall through to the general call below, find no such function
             * and become a jal to itself -- tools/sd_root/cat.c never
             * returned. */
            if (strcmp(node->funcname, "read_file") == 0 || strcmp(node->funcname, "write_file") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(13, 12, 0)); // a3 = a2
                code_idx = emit_word(code_buf, code_idx, encode_addi(12, 11, 0)); // a2 = a1
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx,
                                     encode_addi(10, 0, node->funcname[0] == 'r' ? 13 : 14));
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }
            if (strcmp(node->funcname, "putchar") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, 12)); // a0 = 12 (SYS_PUTCHAR)
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }

            /* Resolve by identity, and emit a jal either way.
             *
             * Two things used to go wrong here, and the second one is why a
             * program with several functions produced no output at all.
             *
             * First, "not found" was inferred from `code_offset == 0`. But
             * codegen() puts main at order[0], so offset 0 is a perfectly
             * ordinary address for it -- and during the first dry pass every
             * function that has not been emitted yet still holds the 0 its
             * memset left. A real call to a real function therefore read as
             * unresolved purely because of where it sat in the pass.
             *
             * Second, and worse: that branch `return`ed without emitting
             * anything. The offset-convergence loop in codegen() depends on
             * every pass emitting the *same number of bytes* so that the
             * offsets it measures in one pass are still true in the next.
             * Skipping an instruction in the dry passes and emitting it in
             * the final one moves every subsequent function, so nothing ever
             * converged: with four functions the entry point no longer landed
             * on main and the program ran off into nothing, silently.
             *
             * So: look the name up as a pointer, emit a jal unconditionally
             * (a placeholder to nowhere if it is genuinely unknown, which
             * keeps the size identical in every pass), and only warn on the
             * final pass, where the answer is real. */
            Function *target = NULL;
            for (Function *fn = global_prog; fn; fn = fn->next) {
                if (strcmp(fn->name, node->funcname) == 0) {
                    target = fn;
                    break;
                }
            }
            /* A call to nothing used to compile to a jal to itself -- a
             * program that spun forever. It is an error now. */
            if (!target && g_final_pass) {
                printk("[chibicc Error] call to undefined function '%s'\n", node->funcname);
                g_cg_failed = true;
            }
            int jal_pc = code_idx;
            int diff = target ? (target->code_offset - jal_pc) : 0;
            code_idx = emit_word(code_buf, code_idx, encode_jal(1, diff));
            return;
        }
        case ND_NEG:
            /* Unary minus had no case at all: `-x` emitted nothing and
             * evaluated to whatever a0 held -- `x = -5;` stored the previous
             * value (phase 40 review). */
            gen_expr(node->lhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_sub(10, 0, 10));
            return;
        case ND_ADD:
        case ND_SUB:
        case ND_MUL:
        case ND_DIV:
        case ND_MOD:
        case ND_NE:
        case ND_EQ:
        case ND_LT:
        case ND_LE: {
            gen_expr(node->rhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -16));
            code_idx = emit_word(code_buf, code_idx, encode_store(10, 2, 0, REG_SZ));

            gen_expr(node->lhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_load(11, 2, 0, REG_SZ));
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 16));

            if (node->kind == ND_ADD) code_idx = emit_word(code_buf, code_idx, encode_add(10, 10, 11));
            if (node->kind == ND_SUB) code_idx = emit_word(code_buf, code_idx, encode_sub(10, 10, 11));
            if (node->kind == ND_MUL) code_idx = emit_word(code_buf, code_idx, encode_mul(10, 10, 11));
            if (node->kind == ND_DIV) code_idx = emit_word(code_buf, code_idx, encode_div(10, 10, 11));
            if (node->kind == ND_MOD) code_idx = emit_word(code_buf, code_idx, encode_rem(10, 10, 11));
            if (node->kind == ND_LT)  code_idx = emit_word(code_buf, code_idx, encode_slt(10, 10, 11));
            if (node->kind == ND_LE) {
                code_idx = emit_word(code_buf, code_idx, encode_slt(10, 11, 10));
                code_idx = emit_word(code_buf, code_idx, encode_xori(10, 10, 1));
            }
            if (node->kind == ND_EQ) {
                code_idx = emit_word(code_buf, code_idx, encode_sub(10, 10, 11));
                code_idx = emit_word(code_buf, code_idx, encode_sltu(10, 0, 10));
                code_idx = emit_word(code_buf, code_idx, encode_xori(10, 10, 1));
            }
            if (node->kind == ND_NE) {
                code_idx = emit_word(code_buf, code_idx, encode_sub(10, 10, 11));
                code_idx = emit_word(code_buf, code_idx, encode_sltu(10, 0, 10));
            }
            return;
        }
        default:
            /* An expression this backend does not generate: nothing emitted
             * would mean a value nobody computed. */
            cg_fail("an expression cc cannot generate code for");
            break;
    }
}

static int current_stack_sz = 64;

static void gen_stmt_inner(Node *node, uint8_t *code_buf);

static void gen_stmt(Node *node, uint8_t *code_buf) {
    if (g_cg_depth >= 2 * CHIBICC_MAX_NEST) { cg_fail("statements nested too deeply"); return; }
    g_cg_depth++;
    gen_stmt_inner(node, code_buf);
    g_cg_depth--;
}

static void gen_stmt_inner(Node *node, uint8_t *code_buf) {
    if (!node) return;

    if (node->kind == ND_RETURN) {
        gen_expr(node->lhs, code_buf);
        code_idx = emit_word(code_buf, code_idx, encode_load(8, 2, imm12(current_stack_sz - 2 * REG_SZ, FRAME_ERR), REG_SZ));
        code_idx = emit_word(code_buf, code_idx, encode_load(1, 2, imm12(current_stack_sz - REG_SZ, FRAME_ERR), REG_SZ));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, imm12(current_stack_sz, FRAME_ERR)));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
        return;
    }

    if (node->kind == ND_IF) {
        gen_expr(node->cond, code_buf);

        int beqz_idx = code_idx;
        code_idx = emit_word(code_buf, code_idx, 0x00000013); // NOP placeholder

        gen_stmt(node->then, code_buf);
        code_idx = (code_idx + 3) & ~3;

        if (node->els) {
            int j_idx = code_idx;
            code_idx = emit_word(code_buf, code_idx, 0x00000013); // NOP placeholder

            int else_offset = code_idx - beqz_idx;
            emit_word(code_buf, beqz_idx, encode_beqz(10, else_offset));

            gen_stmt(node->els, code_buf);
            code_idx = (code_idx + 3) & ~3;

            int end_offset = code_idx - j_idx;
            emit_word(code_buf, j_idx, encode_jal(0, end_offset));
        } else {
            int end_offset = code_idx - beqz_idx;
            emit_word(code_buf, beqz_idx, encode_beqz(10, end_offset));
        }
        return;
    }

    if (node->kind == ND_BREAK) {
        /* A break that did not fit the table used to be dropped -- and the
         * loop then did not break. */
        if (loop_depth == 0) { cg_fail("break outside a loop"); return; }
        if (break_cnt >= 16) { cg_fail("more than 16 breaks in one loop nest"); return; }
        break_jals[break_cnt++] = code_idx;
        code_idx = emit_word(code_buf, code_idx, 0x00000013); // NOP placeholder for JAL to loop end
        return;
    }

    if (node->kind == ND_FOR) {
        if (node->init) gen_stmt(node->init, code_buf);

        int loop_begin = code_idx;
        int beqz_idx = -1;

        int prev_break_cnt = break_cnt;
        loop_depth++;

        if (node->cond) {
            gen_expr(node->cond, code_buf);
            beqz_idx = code_idx;
            code_idx = emit_word(code_buf, code_idx, 0x00000013); // NOP placeholder
        }

        gen_stmt(node->then, code_buf);
        if (node->inc) gen_expr(node->inc, code_buf);
        code_idx = (code_idx + 3) & ~3;

        int jump_back = loop_begin - code_idx;
        code_idx = emit_word(code_buf, code_idx, encode_jal(0, jump_back));
        code_idx = (code_idx + 3) & ~3;

        if (beqz_idx != -1) {
            int loop_end = code_idx - beqz_idx;
            emit_word(code_buf, beqz_idx, encode_beqz(10, loop_end));
        }

        /* Patch break statements to jump to loop_end */
        for (int i = prev_break_cnt; i < break_cnt; i++) {
            int b_idx = break_jals[i];
            int b_off = code_idx - b_idx;
            emit_word(code_buf, b_idx, encode_jal(0, b_off));
        }
        break_cnt = prev_break_cnt;
        loop_depth--;
        return;
    }

    if (node->kind == ND_BLOCK || node->kind == ND_EXPR_STMT) {
        if (node->lhs) gen_expr(node->lhs, code_buf);
        if (node->body) {
            for (Node *n = node->body; n; n = n->next) {
                gen_stmt(n, code_buf);
            }
        }
        return;
    }
}

int codegen(Function *prog, uint8_t *code_buf, int max_size) {
    g_cg_max = max_size;
    g_cg_failed = false;
    g_cg_depth = 0;
    global_prog = prog;

    Function *main_fn = NULL;
    for (Function *fn = prog; fn; fn = fn->next) {
        if (strcmp(fn->name, "main") == 0) {
            main_fn = fn;
            break;
        }
    }

    Function *order[16];
    int fn_cnt = 0;
    if (main_fn) order[fn_cnt++] = main_fn;
    for (Function *fn = prog; fn; fn = fn->next) {
        if (fn == main_fn) continue;
        /* The 17th function used to be left out, and calls to it went
         * nowhere. */
        if (fn_cnt >= 16) { cg_fail("more than 16 functions"); return -1; }
        order[fn_cnt++] = fn;
    }

    // 1. Pass 1: Run dry runs to converge function offsets and calculate .rodata section
    int rodata_offset = 0;
    g_final_pass = false;
    for (int pass = 0; pass < 3; pass++) {
        code_idx = 0;
        break_cnt = 0;
        loop_depth = 0;
        for (int i = 0; i < fn_cnt; i++) {
            Function *fn = order[i];
            code_idx = (code_idx + 3) & ~3;
            fn->code_offset = code_idx;
            int stack_sz = fn->stack_size > 64 ? fn->stack_size : 64;
            stack_sz = (stack_sz + 15) & ~15;
            current_stack_sz = stack_sz;

            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, imm12(-stack_sz, FRAME_ERR)));
            code_idx = emit_word(code_buf, code_idx, encode_store(1, 2, imm12(stack_sz - REG_SZ, FRAME_ERR), REG_SZ));
            code_idx = emit_word(code_buf, code_idx, encode_store(8, 2, imm12(stack_sz - 2 * REG_SZ, FRAME_ERR), REG_SZ));
            code_idx = emit_word(code_buf, code_idx, encode_addi(8, 2, imm12(stack_sz, FRAME_ERR)));

            int param_idx = 0;
            for (Obj *param = fn->params; param; param = param->param_next) {
                int sz = param->ty ? param->ty->size : 4;
                code_idx = emit_word(code_buf, code_idx, encode_store(10 + param_idx, 8, imm12(param->offset, FRAME_ERR), sz));
                param_idx++;
            }

            gen_stmt(fn->body, code_buf);

            code_idx = emit_word(code_buf, code_idx, encode_load(8, 2, imm12(stack_sz - 2 * REG_SZ, FRAME_ERR), REG_SZ));
            code_idx = emit_word(code_buf, code_idx, encode_load(1, 2, imm12(stack_sz - REG_SZ, FRAME_ERR), REG_SZ));
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, imm12(stack_sz, FRAME_ERR)));
            code_idx = emit_word(code_buf, code_idx, encode_ret());
        }

        rodata_offset = (code_idx + 7) & ~7;
        for (Obj *var = globals; var; var = var->next) {
            if (var->is_global && var->init_data) {
                rodata_offset = (rodata_offset + 3) & ~3;
                var->offset = rodata_offset;
                int len = strlen(var->init_data) + 1;
                rodata_offset += len;
            }
        }
    }

    // 2. Pass 2: Final Machine Code Generation with resolved function and string offsets
    g_final_pass = true;
    code_idx = 0;
    break_cnt = 0;
    loop_depth = 0;
    for (int i = 0; i < fn_cnt; i++) {
        Function *fn = order[i];
        code_idx = (code_idx + 3) & ~3;
        fn->code_offset = code_idx;
        int stack_sz = fn->stack_size > 64 ? fn->stack_size : 64;
        stack_sz = (stack_sz + 15) & ~15;
        current_stack_sz = stack_sz;

        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, imm12(-stack_sz, FRAME_ERR)));
        code_idx = emit_word(code_buf, code_idx, encode_store(1, 2, imm12(stack_sz - REG_SZ, FRAME_ERR), REG_SZ));
        code_idx = emit_word(code_buf, code_idx, encode_store(8, 2, imm12(stack_sz - 2 * REG_SZ, FRAME_ERR), REG_SZ));
        code_idx = emit_word(code_buf, code_idx, encode_addi(8, 2, imm12(stack_sz, FRAME_ERR)));

        int param_idx = 0;
        for (Obj *param = fn->params; param; param = param->param_next) {
            int sz = param->ty ? param->ty->size : 4;
            code_idx = emit_word(code_buf, code_idx, encode_store(10 + param_idx, 8, imm12(param->offset, FRAME_ERR), sz));
            param_idx++;
        }

        gen_stmt(fn->body, code_buf);

        code_idx = emit_word(code_buf, code_idx, encode_load(8, 2, imm12(stack_sz - 2 * REG_SZ, FRAME_ERR), REG_SZ));
        code_idx = emit_word(code_buf, code_idx, encode_load(1, 2, imm12(stack_sz - REG_SZ, FRAME_ERR), REG_SZ));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, imm12(stack_sz, FRAME_ERR)));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
    }

    // Copy string literal payloads into .rodata section (4-byte aligned)
    for (Obj *var = globals; var; var = var->next) {
        if (var->is_global && var->init_data) {
            int len = strlen(var->init_data) + 1;
            int pad_len = (len + 3) & ~3;
            if (var->offset < 0 || var->offset > g_cg_max - pad_len) {
                cg_fail("program too large: its strings do not fit cc's output buffer");
                break;
            }
            memset(code_buf + var->offset, 0, pad_len);
            memcpy(code_buf + var->offset, var->init_data, len);
            if (var->offset + pad_len > code_idx) {
                code_idx = var->offset + pad_len;
            }
        }
    }

    // Align total size to 8 bytes
    code_idx = (code_idx + 7) & ~7;
    if (code_idx > g_cg_max) cg_fail("program too large: its code does not fit cc's output buffer");
    return g_cg_failed ? -1 : code_idx;
}
