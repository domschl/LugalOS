/*
 * chibicc - RISC-V Machine Code Generator with Control Flow & Function Calls
 * Copyright (c) 2020 Rui Ueyama
 * License: MIT License
 * Adapted for LugalOS Freestanding RISC-V Microkernel Architecture
 */

#include "chibicc.h"
#include "kernel/printk.h"
#include <string.h>

static int code_idx = 0;

static int emit_word(uint8_t *buf, int offset, uint32_t word) {
    buf[offset + 0] = (uint8_t)(word & 0xFF);
    buf[offset + 1] = (uint8_t)((word >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t)((word >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t)((word >> 24) & 0xFF);
    return offset + 4;
}

static uint32_t encode_addi(int rd, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    return (uimm << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | 0x13;
}

static uint32_t encode_add(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B; // addw
#else
    uint8_t opcode = 0x33; // add
#endif
    return (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_sub(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B; // subw
#else
    uint8_t opcode = 0x33; // sub
#endif
    return (0x20 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_mul(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B; // mulw
#else
    uint8_t opcode = 0x33; // mul
#endif
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_div(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B; // divw
#else
    uint8_t opcode = 0x33; // div
#endif
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_rem(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B; // remw
#else
    uint8_t opcode = 0x33; // rem
#endif
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x6 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_sd(int rs2, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    uint32_t imm11_5 = (uimm >> 5) & 0x7F;
    uint32_t imm4_0 = uimm & 0x1F;
#if defined(CONFIG_TARGET_RV64)
    uint8_t funct3 = 0x3; // sd
#else
    uint8_t funct3 = 0x2; // sw
#endif
    return (imm11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (imm4_0 << 7) | 0x23;
}

static uint32_t encode_ld(int rd, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
#if defined(CONFIG_TARGET_RV64)
    uint8_t funct3 = 0x3; // ld
#else
    uint8_t funct3 = 0x2; // lw
#endif
    return (uimm << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x03;
}

static uint32_t encode_ret(void) {
    return 0x00008067;
}

static uint32_t encode_slt(int rd, int rs1, int rs2) {
    return (0x00 << 25) | (rs2 << 20) | (rs1 << 15) | (0x2 << 12) | (rd << 7) | 0x33;
}

static uint32_t encode_xori(int rd, int rs1, int16_t imm) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    return (uimm << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | 0x13;
}

/* Branch B-Type: beqz rs1, offset */
static uint32_t encode_beqz(int rs1, int16_t offset) {
    uint32_t uoff = (uint32_t)offset & 0x1FFF;
    uint32_t b12 = (uoff >> 12) & 0x1;
    uint32_t b10_5 = (uoff >> 5) & 0x3F;
    uint32_t b4_1 = (uoff >> 1) & 0xF;
    uint32_t b11 = (uoff >> 11) & 0x1;
    return (b12 << 31) | (b10_5 << 25) | (0 << 20) | (rs1 << 15) | (0x0 << 12) | (b4_1 << 8) | (b11 << 7) | 0x63;
}

/* Jump J-Type: jal rd, offset */
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

static void gen_expr(Node *node, uint8_t *code_buf) {
    if (!node) return;

    switch (node->kind) {
        case ND_NUM:
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, (int16_t)node->val));
            return;
        case ND_VAR:
            code_idx = emit_word(code_buf, code_idx, encode_ld(10, 8, (int16_t)node->var->offset));
            return;
        case ND_ASSIGN:
            gen_expr(node->rhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_sd(10, 8, (int16_t)node->lhs->var->offset));
            return;
        case ND_FUNCALL: {
            int arg_cnt = 0;
            for (Node *arg = node->args; arg; arg = arg->next) {
                gen_expr(arg, code_buf);
                // Push arg to stack
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -8));
                code_idx = emit_word(code_buf, code_idx, encode_sd(10, 2, 0));
                arg_cnt++;
            }
            // Pop arguments into a0..a7
            for (int i = arg_cnt - 1; i >= 0; i--) {
                code_idx = emit_word(code_buf, code_idx, encode_ld(10 + i, 2, 0));
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 8));
            }
            // For simplicity, function calls call entry point directly or jal offset
            // We emit jal ra, func_offset (placeholder 0 for recursion)
            code_idx = emit_word(code_buf, code_idx, encode_jal(1, 0));
            return;
        }
        case ND_EQ:
        case ND_NE:
        case ND_LT:
        case ND_LE:
        case ND_ADD:
        case ND_SUB:
        case ND_MUL:
        case ND_DIV:
        case ND_MOD:
            gen_expr(node->rhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -8));
            code_idx = emit_word(code_buf, code_idx, encode_sd(10, 2, 0));

            gen_expr(node->lhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_ld(11, 2, 0));
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 8));

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
                code_idx = emit_word(code_buf, code_idx, encode_slt(10, 0, 10)); // a0 = (a0 == 0)
                code_idx = emit_word(code_buf, code_idx, encode_xori(10, 10, 1));
            }
            if (node->kind == ND_NE) {
                code_idx = emit_word(code_buf, code_idx, encode_sub(10, 10, 11));
                code_idx = emit_word(code_buf, code_idx, encode_slt(10, 0, 10)); // a0 = (a0 != 0)
            }
            return;
        default:
            break;
    }
}

static void gen_stmt(Node *node, uint8_t *code_buf) {
    if (!node) return;

    if (node->kind == ND_RETURN) {
        gen_expr(node->lhs, code_buf);
        code_idx = emit_word(code_buf, code_idx, encode_ld(8, 2, 48));
        code_idx = emit_word(code_buf, code_idx, encode_ld(1, 2, 56));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 64));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
        return;
    }

    if (node->kind == ND_IF) {
        gen_expr(node->cond, code_buf);

        int beqz_idx = code_idx;
        code_idx += 4; // Reserve space for beqz

        gen_stmt(node->then, code_buf);

        if (node->els) {
            int j_idx = code_idx;
            code_idx += 4; // Reserve space for jump

            int else_offset = code_idx - beqz_idx;
            emit_word(code_buf, beqz_idx, encode_beqz(10, (int16_t)else_offset));

            gen_stmt(node->els, code_buf);

            int end_offset = code_idx - j_idx;
            emit_word(code_buf, j_idx, encode_jal(0, end_offset));
        } else {
            int end_offset = code_idx - beqz_idx;
            emit_word(code_buf, beqz_idx, encode_beqz(10, (int16_t)end_offset));
        }
        return;
    }

    if (node->kind == ND_FOR) {
        if (node->init) gen_stmt(node->init, code_buf);

        int loop_begin = code_idx;
        int beqz_idx = -1;

        if (node->cond) {
            gen_expr(node->cond, code_buf);
            beqz_idx = code_idx;
            code_idx += 4;
        }

        gen_stmt(node->then, code_buf);
        if (node->inc) gen_expr(node->inc, code_buf);

        int jump_back = loop_begin - code_idx;
        code_idx = emit_word(code_buf, code_idx, encode_jal(0, jump_back));

        if (beqz_idx != -1) {
            int loop_end = code_idx - beqz_idx;
            emit_word(code_buf, beqz_idx, encode_beqz(10, (int16_t)loop_end));
        }
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
    (void)max_size;
    code_idx = 0;

    for (Function *fn = prog; fn; fn = fn->next) {
        // Prologue: addi sp, sp, -64; sd ra, 56(sp); sd s0, 48(sp); addi s0, sp, 64
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -64));
        code_idx = emit_word(code_buf, code_idx, encode_sd(1, 2, 56));
        code_idx = emit_word(code_buf, code_idx, encode_sd(8, 2, 48));
        code_idx = emit_word(code_buf, code_idx, encode_addi(8, 2, 64));

        // Save parameter registers (a0..a7) to stack slots
        int param_idx = 0;
        for (Obj *param = fn->params; param; param = param->next) {
            code_idx = emit_word(code_buf, code_idx, encode_sd(10 + param_idx, 8, (int16_t)param->offset));
            param_idx++;
        }

        gen_stmt(fn->body, code_buf);

        // Fallthrough Epilogue
        code_idx = emit_word(code_buf, code_idx, encode_ld(8, 2, 48));
        code_idx = emit_word(code_buf, code_idx, encode_ld(1, 2, 56));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 64));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
    }
    return code_idx;
}
