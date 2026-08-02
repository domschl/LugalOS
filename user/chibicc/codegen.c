/*
 * chibicc - RISC-V Machine Code Generator
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
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | 0x33;
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

static void gen_expr(Node *node, uint8_t *code_buf) {
    if (!node) return;

    switch (node->kind) {
        case ND_NUM:
            // li a0, val
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, (int16_t)node->val));
            return;
        case ND_VAR:
            // ld a0, offset(fp) -> ld a0, offset(s0)
            code_idx = emit_word(code_buf, code_idx, encode_ld(10, 8, (int16_t)node->var->offset));
            return;
        case ND_ASSIGN:
            gen_expr(node->rhs, code_buf);
            // sd a0, offset(s0)
            code_idx = emit_word(code_buf, code_idx, encode_sd(10, 8, (int16_t)node->lhs->var->offset));
            return;
        case ND_ADD:
        case ND_SUB:
        case ND_MUL:
        case ND_DIV:
            gen_expr(node->rhs, code_buf);
            // push a0: addi sp, sp, -8; sd a0, 0(sp)
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -8));
            code_idx = emit_word(code_buf, code_idx, encode_sd(10, 2, 0));

            gen_expr(node->lhs, code_buf);
            // pop a1: ld a1, 0(sp); addi sp, sp, 8
            code_idx = emit_word(code_buf, code_idx, encode_ld(11, 2, 0));
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 8));

            if (node->kind == ND_ADD) code_idx = emit_word(code_buf, code_idx, encode_add(10, 10, 11));
            if (node->kind == ND_SUB) code_idx = emit_word(code_buf, code_idx, encode_sub(10, 10, 11));
            if (node->kind == ND_MUL) code_idx = emit_word(code_buf, code_idx, encode_mul(10, 10, 11));
            if (node->kind == ND_DIV) code_idx = emit_word(code_buf, code_idx, encode_div(10, 10, 11));
            return;
        default:
            break;
    }
}

static void gen_stmt(Node *node, uint8_t *code_buf) {
    if (!node) return;

    if (node->kind == ND_RETURN) {
        gen_expr(node->lhs, code_buf);
        // Epilogue: ld s0, 48(sp); ld ra, 56(sp); addi sp, sp, 64; ret
        code_idx = emit_word(code_buf, code_idx, encode_ld(8, 2, 48));
        code_idx = emit_word(code_buf, code_idx, encode_ld(1, 2, 56));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 64));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
        return;
    }

    if (node->kind == ND_EXPR_STMT) {
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

        gen_stmt(fn->body, code_buf);

        // Fallthrough Epilogue
        code_idx = emit_word(code_buf, code_idx, encode_ld(8, 2, 48));
        code_idx = emit_word(code_buf, code_idx, encode_ld(1, 2, 56));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 64));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
    }
    return code_idx;
}
