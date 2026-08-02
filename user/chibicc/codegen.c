/*
 * chibicc - RISC-V Code Generator with String Literal (.rodata) PC-Relative Address Generation
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
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B;
#else
    uint8_t opcode = 0x33;
#endif
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x0 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_div(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B;
#else
    uint8_t opcode = 0x33;
#endif
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x4 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_rem(int rd, int rs1, int rs2) {
#if defined(CONFIG_TARGET_RV64)
    uint8_t opcode = 0x3B;
#else
    uint8_t opcode = 0x33;
#endif
    return (0x01 << 25) | (rs2 << 20) | (rs1 << 15) | (0x6 << 12) | (rd << 7) | opcode;
}

static uint32_t encode_store(int rs2, int rs1, int16_t imm, int sz) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    uint32_t imm11_5 = (uimm >> 5) & 0x7F;
    uint32_t imm4_0 = uimm & 0x1F;
    uint8_t funct3 = 0x3;
    if (sz == 1) funct3 = 0x0;
    else if (sz == 2) funct3 = 0x1;
    else if (sz == 4) funct3 = 0x2;
#if defined(CONFIG_TARGET_RV64)
    else if (sz == 8) funct3 = 0x3;
#else
    else if (sz == 8) funct3 = 0x2;
#endif
    return (imm11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (imm4_0 << 7) | 0x23;
}

static uint32_t encode_load(int rd, int rs1, int16_t imm, int sz) {
    uint32_t uimm = (uint32_t)imm & 0xFFF;
    uint8_t funct3 = 0x3;
    if (sz == 1) funct3 = 0x0;
    else if (sz == 2) funct3 = 0x1;
    else if (sz == 4) funct3 = 0x2;
#if defined(CONFIG_TARGET_RV64)
    else if (sz == 8) funct3 = 0x3;
#else
    else if (sz == 8) funct3 = 0x2;
#endif
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

static uint32_t encode_beqz(int rs1, int16_t offset) {
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
        code_idx = emit_word(code_buf, code_idx, encode_addi(10, 8, (int16_t)node->var->offset));
        return;
    }
    if (node->kind == ND_DEREF) {
        gen_expr(node->lhs, code_buf);
        return;
    }
    if (node->kind == ND_MEMBER) {
        gen_addr(node->lhs, code_buf);
        if (node->member) {
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 10, (int16_t)node->member->offset));
        }
        return;
    }
    printk("[chibicc Error] Not an lvalue for address-of operator\n");
}

static Function *global_prog = NULL;

static void gen_expr(Node *node, uint8_t *code_buf) {
    if (!node) return;

    switch (node->kind) {
        case ND_NUM:
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, (int16_t)node->val));
            return;
        case ND_STR: {
            int pc = code_idx;
            int target = node->var->offset;
            int diff = target - pc;
            int32_t hi = (diff + 0x800) >> 12;
            int16_t lo = diff - (hi << 12);
            code_idx = emit_word(code_buf, code_idx, encode_auipc(10, hi));
            code_idx = emit_word(code_buf, code_idx, encode_addi(10, 10, lo));
            return;
        }
        case ND_VAR: {
            int sz = (node->var && node->var->ty) ? node->var->ty->size : 4;
            if (node->var && node->var->ty && node->var->ty->kind == TY_PTR) {
                sz = 8;
            }
            if (node->var && node->var->ty && (node->var->ty->kind == TY_ARRAY || node->var->ty->kind == TY_STRUCT)) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 8, (int16_t)node->var->offset));
            } else {
                code_idx = emit_word(code_buf, code_idx, encode_load(10, 8, (int16_t)node->var->offset, sz));
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
                gen_expr(node->rhs, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_store(10, 8, (int16_t)node->lhs->var->offset, sz));
            } else if (node->lhs->kind == ND_DEREF || node->lhs->kind == ND_MEMBER) {
                int sz = 4;
                if (node->lhs->kind == ND_MEMBER && node->lhs->member && node->lhs->member->ty) {
                    sz = node->lhs->member->ty->size;
                } else if (node->lhs->kind == ND_DEREF && node->lhs->lhs && node->lhs->lhs->ty && node->lhs->lhs->ty->base) {
                    sz = node->lhs->lhs->ty->base->size;
                }
                gen_expr(node->rhs, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, -16));
                code_idx = emit_word(code_buf, code_idx, encode_store(10, 2, 0, 8));

                gen_addr(node->lhs, code_buf);
                code_idx = emit_word(code_buf, code_idx, encode_load(11, 2, 0, 8));
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
                code_idx = emit_word(code_buf, code_idx, encode_store(10, 2, 0, 8));
                arg_cnt++;
            }
            for (int i = arg_cnt - 1; i >= 0; i--) {
                code_idx = emit_word(code_buf, code_idx, encode_load(10 + i, 2, 0, 8));
                code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, 16));
            }

            if (strcmp(node->funcname, "lugal_syscall") == 0 || strcmp(node->funcname, "syscall") == 0) {
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // RISC-V ecall instruction
                return;
            }
            if (strcmp(node->funcname, "print") == 0 || strcmp(node->funcname, "puts") == 0) {
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
            if (strcmp(node->funcname, "putchar") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, 12)); // a0 = 12 (SYS_PUTCHAR)
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }
            if (strcmp(node->funcname, "read_file") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(13, 12, 0)); // a3 = a2
                code_idx = emit_word(code_buf, code_idx, encode_addi(12, 11, 0)); // a2 = a1
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, 13)); // a0 = 13 (SYS_READ_FILE)
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }
            if (strcmp(node->funcname, "write_file") == 0) {
                code_idx = emit_word(code_buf, code_idx, encode_addi(13, 12, 0)); // a3 = a2
                code_idx = emit_word(code_buf, code_idx, encode_addi(12, 11, 0)); // a2 = a1
                code_idx = emit_word(code_buf, code_idx, encode_addi(11, 10, 0)); // a1 = a0
                code_idx = emit_word(code_buf, code_idx, encode_addi(10, 0, 14)); // a0 = 14 (SYS_WRITE_FILE)
                code_idx = emit_word(code_buf, code_idx, 0x00000073); // ecall
                return;
            }

            int target_offset = 0;
            for (Function *fn = global_prog; fn; fn = fn->next) {
                if (strcmp(fn->name, node->funcname) == 0) {
                    target_offset = fn->code_offset;
                    break;
                }
            }
            int jal_pc = code_idx;
            int diff = target_offset - jal_pc;
            code_idx = emit_word(code_buf, code_idx, encode_jal(1, diff));
            return;
        }
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
            code_idx = emit_word(code_buf, code_idx, encode_store(10, 2, 0, 8));

            gen_expr(node->lhs, code_buf);
            code_idx = emit_word(code_buf, code_idx, encode_load(11, 2, 0, 8));
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
        }default:
            break;
    }
}

static int current_stack_sz = 64;

static void gen_stmt(Node *node, uint8_t *code_buf) {
    if (!node) return;

    if (node->kind == ND_RETURN) {
        gen_expr(node->lhs, code_buf);
        code_idx = emit_word(code_buf, code_idx, encode_load(8, 2, (int16_t)(current_stack_sz - 16), 8));
        code_idx = emit_word(code_buf, code_idx, encode_load(1, 2, (int16_t)(current_stack_sz - 8), 8));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, (int16_t)current_stack_sz));
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
            emit_word(code_buf, beqz_idx, encode_beqz(10, (int16_t)else_offset));

            gen_stmt(node->els, code_buf);
            code_idx = (code_idx + 3) & ~3;

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
        if (fn != main_fn && fn_cnt < 16) {
            order[fn_cnt++] = fn;
        }
    }

    // 1. Pass 1a & 1b: Run dry runs to converge function offsets and calculate .rodata section
    for (int pass = 0; pass < 2; pass++) {
        code_idx = 0;
        for (int i = 0; i < fn_cnt; i++) {
            Function *fn = order[i];
            code_idx = (code_idx + 3) & ~3; // Enforce 4-byte function alignment
            fn->code_offset = code_idx;
            int stack_sz = fn->stack_size > 64 ? fn->stack_size : 64;
            stack_sz = (stack_sz + 15) & ~15;
            current_stack_sz = stack_sz;

            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, (int16_t)-stack_sz));
            code_idx = emit_word(code_buf, code_idx, encode_store(1, 2, (int16_t)(stack_sz - 8), 8));
            code_idx = emit_word(code_buf, code_idx, encode_store(8, 2, (int16_t)(stack_sz - 16), 8));
            code_idx = emit_word(code_buf, code_idx, encode_addi(8, 2, (int16_t)stack_sz));

            int param_idx = 0;
            for (Obj *param = fn->params; param; param = param->next) {
                int sz = param->ty ? param->ty->size : 4;
                code_idx = emit_word(code_buf, code_idx, encode_store(10 + param_idx, 8, (int16_t)param->offset, sz));
                param_idx++;
            }

            gen_stmt(fn->body, code_buf);

            code_idx = emit_word(code_buf, code_idx, encode_load(8, 2, (int16_t)(stack_sz - 16), 8));
            code_idx = emit_word(code_buf, code_idx, encode_load(1, 2, (int16_t)(stack_sz - 8), 8));
            code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, (int16_t)stack_sz));
            code_idx = emit_word(code_buf, code_idx, encode_ret());
        }
    }

    int code_pass1 = code_idx;
    int rodata_offset = (code_pass1 + 7) & ~7;
    for (Obj *var = globals; var; var = var->next) {
        if (var->is_global && var->init_data) {
            var->offset = rodata_offset;
            int len = strlen(var->init_data) + 1;
            rodata_offset += len;
        }
    }

    // 2. Pass 2: Final Machine Code Generation with resolved function and string offsets
    code_idx = 0;
    for (int i = 0; i < fn_cnt; i++) {
        Function *fn = order[i];
        code_idx = (code_idx + 3) & ~3;
        fn->code_offset = code_idx;
        int stack_sz = fn->stack_size > 64 ? fn->stack_size : 64;
        stack_sz = (stack_sz + 15) & ~15;
        current_stack_sz = stack_sz;

        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, (int16_t)-stack_sz));
        code_idx = emit_word(code_buf, code_idx, encode_store(1, 2, (int16_t)(stack_sz - 8), 8));
        code_idx = emit_word(code_buf, code_idx, encode_store(8, 2, (int16_t)(stack_sz - 16), 8));
        code_idx = emit_word(code_buf, code_idx, encode_addi(8, 2, (int16_t)stack_sz));

        int param_idx = 0;
        for (Obj *param = fn->params; param; param = param->next) {
            int sz = param->ty ? param->ty->size : 4;
            code_idx = emit_word(code_buf, code_idx, encode_store(10 + param_idx, 8, (int16_t)param->offset, sz));
            param_idx++;
        }

        gen_stmt(fn->body, code_buf);

        code_idx = emit_word(code_buf, code_idx, encode_load(8, 2, (int16_t)(stack_sz - 16), 8));
        code_idx = emit_word(code_buf, code_idx, encode_load(1, 2, (int16_t)(stack_sz - 8), 8));
        code_idx = emit_word(code_buf, code_idx, encode_addi(2, 2, (int16_t)stack_sz));
        code_idx = emit_word(code_buf, code_idx, encode_ret());
    }

    // Pad text section up to rodata_offset
    while (code_idx < rodata_offset) {
        code_buf[code_idx++] = 0;
    }

    // Copy string literal payloads into .rodata section
    for (Obj *var = globals; var; var = var->next) {
        if (var->is_global && var->init_data) {
            int len = strlen(var->init_data) + 1;
            memcpy(code_buf + var->offset, var->init_data, len);
            code_idx = var->offset + len;
        }
    }

    // Align total size to 8 bytes
    code_idx = (code_idx + 7) & ~7;
    return code_idx;
}
