/* user/zmachine/zvm.c — Z-machine Version 3 central processing unit
 *
 * Instruction encoding (zmach06e.txt §7.2, z-spec10.pdf §6):
 *
 *   %0abxxxxx  00-7F  long:     2OP number, types a,b (%0 byte const,
 *                              %1 variable)
 *   %10ttxxxx  80-AF short:     1OP number, tt operand type
 *                              (%00 word, %01 byte, %10 variable)
 *   %1011xxxx  B0-BF short:     0OP number
 *   %110xxxxx  C0-DF variable:  2OP number, type byte (pairs: %00 word,
 *                              %01 byte, %10 variable, %11 end)
 *   %111xxxxx  E0-FF variable:  VAR number, type byte
 *
 * The $BE EXT escape is a Version 5 construct and rejected here.
 *
 * Semantics follow zmach06e.txt (era-correct for zork1.zip) with
 * z-spec10.pdf cross-checks; every non-obvious choice is cited below.
 */
#include "zvm.h"
#include "zparse.h"
#include "ztext.h"

/* ------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static int z_stop(struct z_vm *vm, int reason, const char *msg)
{
    vm->stop_reason = reason;
    vm->msg = msg;
    return reason;
}

/* 16-bit signed interpretation of Z-machine numbers. */
static int16_t z_s16(uint16_t v)
{
    return (int16_t)v;
}

/* Deterministic PRNG (xorshift32); freestanding, no libc rand(). */
static uint32_t z_random_next(struct z_vm *vm)
{
    uint32_t x = vm->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rng_state = x;
    return x;
}

/* ------------------------------------------------------------------------
 * Variables (zmach06e §7.1): 0 = stack top, 1..15 = locals, 16.. = globals
 * ------------------------------------------------------------------------ */

static uint16_t z_read_var(struct z_vm *vm, uint8_t n)
{
    struct z_frame *f = &vm->frames[vm->nframes - 1];

    if (n == 0u) {
        if (vm->sp == f->base_sp) {
            z_stop(vm, ZVM_STOP_ERROR, "pull from empty routine stack");
            return 0;
        }
        return vm->stack[--vm->sp];
    }
    if (n <= Z_MAX_LOCALS) {
        if (n > f->nlocals) {
            z_stop(vm, ZVM_STOP_ERROR, "local variable index out of range");
            return 0;
        }
        return f->locals[n - 1];
    }
    return z_read_word(&vm->m, vm->m.global_base + 2u * (uint32_t)(n - 16u));
}

static void z_write_var(struct z_vm *vm, uint8_t n, uint16_t val)
{
    struct z_frame *f = &vm->frames[vm->nframes - 1];

    if (n == 0u) {
        if (vm->sp >= Z_STACK_WORDS) {
            z_stop(vm, ZVM_STOP_ERROR, "routine stack overflow");
            return;
        }
        vm->stack[vm->sp++] = val;
        return;
    }
    if (n <= Z_MAX_LOCALS) {
        if (n > f->nlocals) {
            z_stop(vm, ZVM_STOP_ERROR, "local variable index out of range");
            return;
        }
        f->locals[n - 1] = val;
        return;
    }
    z_write_word(&vm->m, vm->m.global_base + 2u * (uint32_t)(n - 16u), val);
}

/* ------------------------------------------------------------------------
 * Fetch helpers (PC advances; v3 wraps at $10000 -> high base)
 * ------------------------------------------------------------------------ */

static uint8_t z_fetch_byte(struct z_vm *vm)
{
    uint8_t b = z_read_byte(&vm->m, vm->pc);
    vm->pc = vm->pc + 1u;
    return b;
}

static uint16_t z_fetch_word(struct z_vm *vm)
{
    uint16_t w = (uint16_t)((uint32_t)z_fetch_byte(vm) << 8 | z_fetch_byte(vm));
    return w;
}

/* Fetch one operand given its 2-bit type (%11 must not occur here).
 * Used by short 1OP (%00 word, %01 byte, %10 variable) and by the
 * variable-form type byte (%00 word, %01 byte, %10 variable, %11 end). */
static uint16_t z_fetch_operand(struct z_vm *vm, uint8_t type)
{
    switch (type) {
    case 0u: return z_fetch_word(vm);            /* word constant   */
    case 1u: return z_fetch_byte(vm);            /* byte constant   */
    default: return z_read_var(vm, z_fetch_byte(vm)); /* variable   */
    }
}

/* Long 2OP form has only ABBREVIATED types: %0 = one-byte constant,
 * %1 = variable (zmach06e §7.2: "long instruction %0abxxxxx ... %0: a
 * byte constant; %1: a variable number").  A 16-bit constant cannot
 * appear in long form; the variable form carries it as %00. */
static uint16_t z_fetch_operand_long(struct z_vm *vm, uint8_t abbreviated)
{
    if (abbreviated == 0u) {
        return z_fetch_byte(vm);
    }
    return z_read_var(vm, z_fetch_byte(vm));
}

/* Skip an inline Z-string: big-endian words until bit 15 set. */
static void z_skip_string(struct z_vm *vm)
{
    while ((z_fetch_word(vm) & 0x8000u) == 0u) {
        /* keep going */
    }
}

/* ------------------------------------------------------------------------
 * Return / branch
 * ------------------------------------------------------------------------ */

/* Deliver `val` as the return value of the current routine (zmach06e
 * §8.5 RET): pop the frame, restore the caller's stack extent, honour
 * the call's pending result store, and resume the caller. */
static void z_do_return(struct z_vm *vm, uint16_t val)
{
    struct z_frame *f;

    if (vm->nframes <= 1u) {
        z_stop(vm, ZVM_STOP_ERROR, "return from initial stack frame");
        return;
    }
    f = &vm->frames[vm->nframes - 1];
    vm->nframes--;
    vm->sp = f->base_sp;
    vm->pc = f->resume_pc;
    if (f->pending_dest != 0xFFu) {
        z_write_var(vm, f->pending_dest, val);
    }
}

/* Branch instruction bytes (zmach06e §7.3, z-spec10 §4.7): polarity
 * bit 7, long/short form bit 6; taken branches with offset 0/1 return
 * false/true from the current routine.  When the condition does not
 * satisfy the polarity, execution simply continues after the bytes. */
static void z_branch(struct z_vm *vm, int condition)
{
    uint8_t b = z_fetch_byte(vm);
    int branch_on = (b & 0x80u) != 0u;
    int32_t offset;

    if (b & 0x40u) {
        offset = (int32_t)(b & 0x3Fu);            /* 6-bit unsigned */
    } else {
        uint8_t lo = z_fetch_byte(vm);
        offset = (int32_t)(((uint32_t)(b & 0x3Fu) << 8) | lo);
        if (offset >= 0x2000) {
            offset -= 0x4000;                     /* 14-bit signed  */
        }
    }

    if ((condition != 0) == (branch_on != 0)) {
        if (offset == 0) {
            z_do_return(vm, 0);
        } else if (offset == 1) {
            z_do_return(vm, 1);
        } else {
            /* Target: address after branch data + offset - 2. */
            vm->pc = vm->pc + (uint32_t)(offset - 2);
        }
    }
}

/* ------------------------------------------------------------------------
 * Object model (z-spec10 §12, zmach06e §8.6)
 *
 * v3 object table at header $0c *2:
 *   +0            : 31 default property words (properties 1..31)
 *   +62 + 9*(o-1) : attributes (4 bytes; attr 0 = bit 7 of byte 0)
 *   +66 + 9*(o-1) : parent, sibling, child
 *   +67 + 9*(o-1) : property list address (word)
 * ------------------------------------------------------------------------ */

#define Z_V3_DEFAULT_WORDS 31u
#define Z_V3_ATTR_BYTES    4u

#define Z_OBJ_PARENT_OFF (Z_V3_ATTR_BYTES)
#define Z_OBJ_SIB_OFF    (Z_V3_ATTR_BYTES + 1u)
#define Z_OBJ_CHILD_OFF  (Z_V3_ATTR_BYTES + 2u)
#define Z_OBJ_PROPS_OFF  (Z_V3_ATTR_BYTES + 3u)

static uint32_t z_object_entry(struct z_vm *vm, uint16_t obj)
{
    return vm->m.objects_base + Z_V3_DEFAULT_WORDS * 2u
           + (uint32_t)(obj - 1u) * (Z_V3_ATTR_BYTES + 5u);
}

static uint8_t z_get_parent(struct z_vm *vm, uint16_t obj)
{
    return z_read_byte(&vm->m, z_object_entry(vm, obj) + Z_OBJ_PARENT_OFF);
}

/* Unlink obj from its parent's child list (parent byte untouched). */
static void z_unlink(struct z_vm *vm, uint16_t obj)
{
    uint32_t entry = z_object_entry(vm, obj);
    uint8_t parent = z_read_byte(&vm->m, entry + Z_OBJ_PARENT_OFF);
    uint32_t pentry;
    uint8_t child;

    if (parent == 0u) {
        return;
    }
    pentry = z_object_entry(vm, parent);
    child = z_read_byte(&vm->m, pentry + Z_OBJ_CHILD_OFF);
    if (child == (uint8_t)obj) {
        z_write_byte(&vm->m, pentry + Z_OBJ_CHILD_OFF,
                     z_read_byte(&vm->m, entry + Z_OBJ_SIB_OFF));
        return;
    }
    while (child != 0u) {
        uint32_t centry = z_object_entry(vm, child);
        uint8_t sib = z_read_byte(&vm->m, centry + Z_OBJ_SIB_OFF);
        if (sib == (uint8_t)obj) {
            z_write_byte(&vm->m, centry + Z_OBJ_SIB_OFF,
                         z_read_byte(&vm->m, entry + Z_OBJ_SIB_OFF));
            return;
        }
        child = sib;
    }
}

/* Locate property `prop` in obj's property list; return the byte
 * address of the property DATA, or 0.  v3 size byte: 32*(len-1)+propnum,
 * list terminated by 0 (z-spec10 §12.4.1). */
static uint32_t z_find_prop(struct z_vm *vm, uint16_t obj, uint8_t prop)
{
    uint32_t q = z_read_word(&vm->m, z_object_entry(vm, obj) + Z_OBJ_PROPS_OFF);
    uint32_t len = z_read_byte(&vm->m, q);        /* short name words */
    q = q + 1u + 2u * len;

    for (;;) {
        uint8_t hdr = z_read_byte(&vm->m, q);
        if (hdr == 0u) {
            return 0;
        }
        if ((hdr & 0x1Fu) == prop) {
            return q + 1u;
        }
        q += 1u + (uint32_t)(hdr >> 5) + 1u;
    }
}

static uint8_t z_prop_len_at(struct z_vm *vm, uint32_t data_addr)
{
    /* v1-3: length = (size byte >> 5) + 1  (zmach06e GET_PROP_LEN) */
    return (uint8_t)((z_read_byte(&vm->m, data_addr - 1u) >> 5) + 1u);
}

/* ------------------------------------------------------------------------
 * Instruction dispatch
 * ------------------------------------------------------------------------ */

/* 2OP opcode space, shared by the long form (0x00-0x7F) and the
 * variable form (0xC0-0xDF).  nargs is 2 for long, 0..4 for variable. */
static int z_exec_2op(struct z_vm *vm, uint8_t opcode, const uint16_t *args,
                      uint8_t nargs, uint8_t *done)
{
    uint16_t val;
    uint32_t e;
    uint8_t a;

    *done = 1;
    switch (opcode) {
    case 0x01: { /* je: branch if arg0 equals ANY other operand.
                  * With a single operand it never branches (zmach06e
                  * §8.4 NOTE: ST-comparison emulators are wrong). */
        int eq = 0;
        uint8_t i;
        for (i = 1; i < nargs; i++) {
            if (args[0] == args[i]) {
                eq = 1;
                break;
            }
        }
        z_branch(vm, eq);
        return ZVM_OK;
    }
    case 0x02: /* jl */
        z_branch(vm, z_s16(args[0]) < z_s16(args[1]));
        return ZVM_OK;
    case 0x03: /* jg */
        z_branch(vm, z_s16(args[0]) > z_s16(args[1]));
        return ZVM_OK;
    case 0x04: { /* dec_chk */
        uint8_t vnum = (uint8_t)args[0];
        uint16_t nv = (uint16_t)(z_read_var(vm, vnum) - 1u);
        z_write_var(vm, vnum, nv);
        z_branch(vm, z_s16(nv) < z_s16(args[1]));
        return ZVM_OK;
    }
    case 0x05: { /* inc_chk */
        uint8_t vnum = (uint8_t)args[0];
        uint16_t nv = (uint16_t)(z_read_var(vm, vnum) + 1u);
        z_write_var(vm, vnum, nv);
        z_branch(vm, z_s16(nv) > z_s16(args[1]));
        return ZVM_OK;
    }
    case 0x06: /* jin: parent equality; 0 means "no parent" */
        z_branch(vm, (uint16_t)z_get_parent(vm, (uint16_t)args[0]) == args[1]);
        return ZVM_OK;
    case 0x07: /* test: branch if ALL bits of b present in a */
        z_branch(vm, (args[0] & args[1]) == args[1]);
        return ZVM_OK;
    case 0x08: /* or */
        val = (uint16_t)(args[0] | args[1]);
        break;
    case 0x09: /* and */
        val = (uint16_t)(args[0] & args[1]);
        break;
    case 0x0A: { /* test_attr */
        e = z_object_entry(vm, (uint16_t)args[0]);
        a = (uint8_t)(args[1] & 0x1Fu);
        z_branch(vm, (z_read_byte(&vm->m, e + (a >> 3))
                      & (uint8_t)(0x80u >> (a & 7u))) != 0u);
        return ZVM_OK;
    }
    case 0x0B: { /* set_attr */
        e = z_object_entry(vm, (uint16_t)args[0]);
        a = (uint8_t)(args[1] & 0x1Fu);
        z_write_byte(&vm->m, e + (a >> 3),
                     z_read_byte(&vm->m, e + (a >> 3))
                         | (uint8_t)(0x80u >> (a & 7u)));
        return ZVM_OK;
    }
    case 0x0C: { /* clear_attr */
        e = z_object_entry(vm, (uint16_t)args[0]);
        a = (uint8_t)(args[1] & 0x1Fu);
        z_write_byte(&vm->m, e + (a >> 3),
                     z_read_byte(&vm->m, e + (a >> 3))
                         & (uint8_t) ~(0x80u >> (a & 7u)));
        return ZVM_OK;
    }
    case 0x0D: /* store: arg0 IS the target variable number (zmach06e
                * §7.1: operands fetched normally, result is the number) */
        z_write_var(vm, (uint8_t)args[0], args[1]);
        return ZVM_OK;
    case 0x0E: { /* insert_obj */
        uint16_t o = (uint16_t)args[0];
        uint16_t d = (uint16_t)args[1];
        uint32_t oe;
        uint32_t de;
        if (d == 0u || o == 0u) {
            return z_stop(vm, ZVM_STOP_ERROR, "insert_obj with object 0");
        }
        z_unlink(vm, o);
        oe = z_object_entry(vm, o);
        de = z_object_entry(vm, d);
        z_write_byte(&vm->m, oe + Z_OBJ_SIB_OFF,
                     z_read_byte(&vm->m, de + Z_OBJ_CHILD_OFF));
        z_write_byte(&vm->m, de + Z_OBJ_CHILD_OFF, (uint8_t)o);
        z_write_byte(&vm->m, oe + Z_OBJ_PARENT_OFF, (uint8_t)d);
        return ZVM_OK;
    }
    case 0x0F: /* loadw (16-bit wrap in address arithmetic) */
        val = z_read_word(&vm->m,
                          (uint32_t)args[0] + (uint32_t)args[1] * 2u);
        break;
    case 0x10: /* loadb */
        val = z_read_byte(&vm->m, (uint32_t)args[0] + (uint32_t)args[1]);
        break;
    case 0x11: { /* get_prop; defaults table when property absent */
        uint16_t o = (uint16_t)args[0];
        uint8_t p = (uint8_t)args[1];
        uint32_t d = z_find_prop(vm, o, p);
        if (d == 0u) {
            val = z_read_word(&vm->m, vm->m.objects_base + 2u * (p - 1u));
        } else {
            uint8_t len = z_prop_len_at(vm, d);
            val = (len == 1u) ? z_read_byte(&vm->m, d)
                              : z_read_word(&vm->m, d);
        }
        break;
    }
    case 0x12: /* get_prop_addr */
        val = (uint16_t)z_find_prop(vm, (uint16_t)args[0], (uint8_t)args[1]);
        break;
    case 0x13: { /* get_next_prop: prop 0 -> first; else successor */
        uint16_t o = (uint16_t)args[0];
        uint8_t p = (uint8_t)args[1];
        uint32_t q = z_read_word(&vm->m, z_object_entry(vm, o) + Z_OBJ_PROPS_OFF);
        uint32_t qstart = q + 1u + 2u * z_read_byte(&vm->m, q);
        uint32_t qnext;
        uint8_t hdr;
        RESULT_SKIP:;
        hdr = z_read_byte(&vm->m, qstart);
        if (hdr == 0u) {
            val = 0;
            break;
        }
        if (p == 0u) {
            val = hdr & 0x1Fu;
            break;
        }
        if ((hdr & 0x1Fu) == p) {
            qnext = qstart + 1u + (hdr >> 5) + 1u;
            hdr = z_read_byte(&vm->m, qnext);
            val = (hdr == 0u) ? 0u : (uint8_t)(hdr & 0x1Fu);
            break;
        }
        qstart += 1u + (hdr >> 5) + 1u;
        goto RESULT_SKIP;
    }
    case 0x14: /* add */
        val = (uint16_t)(args[0] + args[1]);
        break;
    case 0x15: /* sub */
        val = (uint16_t)(args[0] - args[1]);
        break;
    case 0x16: /* mul (low 16 bits) */
        val = (uint16_t)((uint32_t)args[0] * (uint32_t)args[1]);
        break;
    case 0x17: /* div: truncates toward zero (C semantics) */
        if (args[1] == 0u) {
            return z_stop(vm, ZVM_STOP_ERROR, "division by zero");
        }
        val = (uint16_t)(z_s16(args[0]) / z_s16(args[1]));
        break;
    case 0x18: /* mod: sign follows dividend (C99) */
        if (args[1] == 0u) {
            return z_stop(vm, ZVM_STOP_ERROR, "modulo by zero");
        }
        val = (uint16_t)(z_s16(args[0]) % z_s16(args[1]));
        break;
    default:
        *done = 0;
        return ZVM_OK;
    }
    /* fell through with val: store into the result byte */
    {
        uint8_t dest = z_fetch_byte(vm);
        z_write_var(vm, dest, val);
    }
    return ZVM_OK;
}

int zvm_step(struct z_vm *vm)
{
    uint8_t op;
    uint8_t opcode;
    uint16_t args[4];
    uint8_t nargs;
    uint8_t done = 0;
    uint16_t val;
    int r;

    if (vm->stop_reason != ZVM_OK) {
        return vm->stop_reason;
    }

    vm->inst_count++;
    op = z_fetch_byte(vm);

    /* Decode class + operands. */
    if (op < 0x80u) {
        /* long 2OP: operand types in bits 6,5 (0 byte const, 1 variable) */
        opcode = op & 0x1Fu;
        nargs = 2;
        args[0] = z_fetch_operand_long(vm, (uint8_t)((op >> 6) & 1u));
        args[1] = z_fetch_operand_long(vm, (uint8_t)((op >> 5) & 1u));
        r = z_exec_2op(vm, opcode, args, nargs, &done);
        goto post;
    }
    if (op < 0xB0u) {
        opcode = op & 0x0Fu;                     /* short 1OP */
        nargs = 1;
        args[0] = z_fetch_operand(vm, (uint8_t)((op >> 4) & 3u));
    } else if (op < 0xC0u) {
        opcode = op & 0x0Fu;                     /* short 0OP */
        nargs = 0;
    } else {
        /* variable form: type byte, 2-bit pairs, max 4 operands */
        uint8_t types = z_fetch_byte(vm);
        uint8_t i;
        opcode = op & 0x1Fu;
        nargs = 0;
        for (i = 0; i < 4u; i++) {
            uint8_t t = (uint8_t)((types >> (6u - 2u * i)) & 3u);
            if (t == 3u) {
                break;
            }
            args[nargs++] = z_fetch_operand(vm, t);
        }
        if (op < 0xE0u) {                        /* variable 2OP */
            r = z_exec_2op(vm, opcode, args, nargs, &done);
            goto post;
        }
    }

    /* ---- 1OP space (short form only in v3) ---- */
    if (op >= 0x80u && op < 0xB0u) {
        switch (opcode) {
        case 0x00: /* jz */
            z_branch(vm, args[0] == 0u);
            goto next;
        case 0x01: { /* get_sibling: result byte AND branch (v3) */
            uint8_t dest = z_fetch_byte(vm);
            uint16_t sib = 0;
            if (z_get_parent(vm, (uint16_t)args[0]) != 0u) {
                sib = z_read_byte(&vm->m, z_object_entry(vm, (uint16_t)args[0])
                                             + Z_OBJ_SIB_OFF);
            }
            z_write_var(vm, dest, sib);
            z_branch(vm, sib != 0u);
            goto next;
        }
        case 0x02: { /* get_child: result byte AND branch (v3) */
            uint8_t dest = z_fetch_byte(vm);
            uint16_t child = z_read_byte(
                &vm->m, z_object_entry(vm, (uint16_t)args[0]) + Z_OBJ_CHILD_OFF);
            z_write_var(vm, dest, child);
            z_branch(vm, child != 0u);
            goto next;
        }
        case 0x03: /* get_parent */
            val = z_get_parent(vm, (uint16_t)args[0]);
            break;
        case 0x04: /* get_prop_len */
            val = z_prop_len_at(vm, args[0]);
            break;
        case 0x05: /* inc (variable) */
            z_write_var(vm, (uint8_t)args[0],
                        (uint16_t)(z_read_var(vm, (uint8_t)args[0]) + 1u));
            goto next;
        case 0x06: /* dec (variable) */
            z_write_var(vm, (uint8_t)args[0],
                        (uint16_t)(z_read_var(vm, (uint8_t)args[0]) - 1u));
            goto next;
        case 0x07: /* print_addr (dynamic/static Z-string by address) */
            ztext_print(vm, args[0]);
            goto next;
        case 0x09: /* remove_obj */
            z_unlink(vm, (uint16_t)args[0]);
            z_write_byte(&vm->m,
                         z_object_entry(vm, (uint16_t)args[0]) + Z_OBJ_PARENT_OFF,
                         0);
            goto next;
        case 0x0A: { /* print_obj: short name from prop-list header */
            uint32_t q = z_read_word(&vm->m,
                                     z_object_entry(vm, (uint16_t)args[0])
                                         + Z_OBJ_PROPS_OFF);
            ztext_print(vm, q + 1u);
            goto next;
        }
        case 0x0B: /* ret */
            z_do_return(vm, args[0]);
            goto next;
        case 0x0C: /* jump: pc = address after instruction + s - 2 */
            vm->pc = vm->pc + (uint32_t)((int32_t)(int16_t)args[0] - 2);
            goto next;
        case 0x0D: /* print_paddr: packed string address (v3: *2) */
            ztext_print(vm, (uint32_t)args[0] * 2u);
            goto next;
        case 0x0E: /* load (variable) */
            val = z_read_var(vm, (uint8_t)args[0]);
            break;
        default:
            return z_stop(vm, ZVM_STOP_UNIMPL, "undefined 1OP opcode");
        }
        {
            uint8_t dest = z_fetch_byte(vm);
            z_write_var(vm, dest, val);
        }
        goto next;
    }

    /* ---- 0OP space ---- */
    if (op >= 0xB0u && op < 0xC0u) {
        switch (opcode) {
        case 0x00: /* rtrue */
            z_do_return(vm, 1);
            goto next;
        case 0x01: /* rfalse */
            z_do_return(vm, 0);
            goto next;
        case 0x02: /* print: inline Z-string follows */
            ztext_print(vm, vm->pc);
            z_skip_string(vm);
            goto next;
        case 0x03: /* print_ret: inline Z-string, then a line break and
                    * return true (frotz parity: new_line after string) */
            ztext_print(vm, vm->pc);
            z_skip_string(vm);
            if (vm->io.putc != NULL) {
                vm->io.putc(vm->io.user, '\n');
            }
            z_do_return(vm, 1);
            goto next;
        case 0x04: /* nop */
            goto next;
        case 0x05: { /* save (v3): branch when the save succeeded */
            int ok = 0;
            /* Resumed execution point after a restore: the instruction
             * following SAVE's branch byte (vm->pc sits ON that byte). */
            uint32_t n = zvm_serialize(vm, vm->savebuf,
                                       sizeof(vm->savebuf), vm->pc + 1u);
            if (n != 0u && vm->io.save != NULL) {
                ok = (vm->io.save(vm->io.user, vm->savebuf, n) == 0);
            }
            z_branch(vm, ok);
            goto next;
        }
        case 0x06: { /* restore (v3): success replaces the whole state
                      * (execution then continues after the corresponding
                      * SAVE, per zmach06e RESTORE); failure continues
                      * past the branch bytes without branching. */
            uint32_t len = 0u;
            if (vm->io.restore != NULL
                && vm->io.restore(vm->io.user, vm->savebuf,
                                  sizeof(vm->savebuf), &len) == 0
                && zvm_deserialize(vm, vm->savebuf, len) == 0) {
                goto next;            /* pc comes from the image */
            }
            z_branch(vm, 0);
            goto next;
        }
        case 0x07: /* restart */
            return z_stop(vm, ZVM_STOP_RESTART, "restart requested");
        case 0x08: /* ret_popped */
            z_do_return(vm, z_read_var(vm, 0));
            goto next;
        case 0x0A: /* quit */
            return z_stop(vm, ZVM_STOP_QUIT, "quit");
        case 0x0B: /* new_line */
            if (vm->io.putc != NULL) {
                vm->io.putc(vm->io.user, '\n');
            }
            goto next;
        case 0x0C: /* show_status (v3 status line redraw): no-op */
            goto next;
        case 0x0D: /* verify: branch when checksum MATCHES (zmach06e
                    * VERIFY); no branch -> return 2 (v1-5 convention) */
            z_branch(vm, z_checksum(&vm->m) == vm->m.checksum);
            if (vm->stop_reason == ZVM_OK) {
                uint8_t dest = z_fetch_byte(vm);
                z_write_var(vm, dest, 2);
            }
            goto next;
        default:
            return z_stop(vm, ZVM_STOP_UNIMPL, "undefined 0OP opcode");
        }
    }

    /* ---- VAR space (op >= 0xE0) ---- */
    switch (opcode) {
    case 0x00: { /* call (v3 CALL_FV: raddr + up to 3 args) */
        struct z_frame *f;
        uint8_t nlocals;
        uint8_t i;
        uint8_t dest;
        uint32_t r;
        if (args[0] == 0u) {
            dest = z_fetch_byte(vm);
            z_write_var(vm, dest, 0);
            break;
        }
        dest = z_fetch_byte(vm);                 /* result byte first  */
        r = (uint32_t)args[0] * 2u;              /* v3: R_O = 0        */
        nlocals = z_read_byte(&vm->m, r);
        if (nlocals > Z_MAX_LOCALS) {
            return z_stop(vm, ZVM_STOP_ERROR, "routine header: >15 locals");
        }
        if (vm->nframes >= Z_MAX_FRAMES) {
            return z_stop(vm, ZVM_STOP_ERROR, "call stack overflow");
        }
        f = &vm->frames[vm->nframes++];
        f->resume_pc = vm->pc;
        f->base_sp = vm->sp;
        f->nlocals = nlocals;
        f->pending_dest = dest;
        for (i = 0; i < nlocals; i++) {
            /* v3 locals: default words from the routine header... */
            f->locals[i] = z_read_word(&vm->m, r + 1u + 2u * i);
        }
        /* ...then overwritten by the passed arguments (z-spec10 §6.4.4) */
        for (i = 0; nargs > 1u && i < (uint8_t)(nargs - 1u); i++) {
            f->locals[i] = args[i + 1u];
        }
        vm->pc = r + 1u + 2u * nlocals;
        goto next;
    }
    case 0x01: /* storew */
        z_write_word(&vm->m, (uint32_t)args[0] + (uint32_t)args[1] * 2u,
                     args[2]);
        goto next;
    case 0x02: /* storeb */
        z_write_byte(&vm->m, (uint32_t)args[0] + (uint32_t)args[1],
                     (uint8_t)args[2]);
        goto next;
    case 0x03: { /* put_prop */
        uint32_t d = z_find_prop(vm, (uint16_t)args[0], (uint8_t)args[1]);
        if (d == 0u) {
            return z_stop(vm, ZVM_STOP_ERROR, "put_prop on absent property");
        }
        if (z_prop_len_at(vm, d) == 1u) {
            z_write_byte(&vm->m, d, (uint8_t)args[2]);
        } else {
            z_write_word(&vm->m, d, args[2]);
        }
        goto next;
    }
    case 0x04: { /* aread (v3 READ): line input + dictionary tokenize */
        int rc = zvm_aread(vm, args[0], args[1]);
        if (rc != ZVM_OK) {
            return z_stop(vm, rc, "aread: input unavailable");
        }
        goto next;
    }
    case 0x05: /* print_char: ZSCII output */
        ztext_put_zscii(vm, args[0]);
        goto next;
    case 0x06: { /* print_num: signed decimal */
        if (vm->io.putc != NULL) {
            char buf[8];
            int n = 0;
            int32_t v = (int32_t)(int16_t)args[0];
            uint32_t u;
            if (v < 0) {
                vm->io.putc(vm->io.user, '-');
                u = (uint32_t)(-(int64_t)v);
            } else {
                u = (uint32_t)v;
            }
            do {
                buf[n++] = (char)('0' + (int)(u % 10u));
                u /= 10u;
            } while (u != 0u);
            while (n > 0) {
                vm->io.putc(vm->io.user, (uint8_t)buf[--n]);
            }
        }
        goto next;
    }
    case 0x07: { /* random: 1..range / -1..range / 0 = reseed */
        int32_t range = (int32_t)(int16_t)args[0];
        uint32_t mag;
        int32_t res;
        uint8_t dest = z_fetch_byte(vm);
        if (range == 0) {
            z_write_var(vm, dest, 0);
            goto next;
        }
        mag = (range > 0) ? (uint32_t)range : (uint32_t)(-(range + 1)) + 1u;
        {
            uint32_t v = z_random_next(vm) % mag;
            res = (range > 0) ? (int32_t)(v + 1u)
                              : -((int32_t)v + 1);
        }
        z_write_var(vm, dest, (uint16_t)(int16_t)res);
        goto next;
    }
    case 0x08: /* push */
        z_write_var(vm, 0, args[0]);
        goto next;
    case 0x09: /* pull (variable) */
        z_write_var(vm, (uint8_t)args[0], z_read_var(vm, 0));
        goto next;
    case 0x0A: /* split_window (v3 hint): no-op */
        goto next;
    case 0x0B: /* set_window: no-op */
        goto next;
    case 0x13: /* output_stream (v3): main/transcript/off — no-op */
        goto next;
    case 0x14: /* input_stream: no-op */
        goto next;
    case 0x15: /* sound_effect: no-op */
        goto next;
    default:
        return z_stop(vm, ZVM_STOP_UNIMPL, "undefined VAR opcode");
    }
    goto next;

post:
    if (r != ZVM_OK) {
        return r;
    }
    if (done) {
        goto next;
    }
    return z_stop(vm, ZVM_STOP_UNIMPL, "undefined 2OP opcode");

next:
    return (vm->stop_reason != ZVM_OK) ? vm->stop_reason : ZVM_OK;
}

int zvm_run(struct z_vm *vm)
{
    for (;;) {
        int r = zvm_step(vm);
        if (r != ZVM_OK) {
            return r;
        }
    }
}

/* ------------------------------------------------------------------------
 * Startup (zmach06e §2.11: single initial frame, empty locals and stack;
 * z-spec10 §11.2: initial PC = header word $08 + 8 in v1-4)
 * ------------------------------------------------------------------------ */

int zvm_init(struct z_vm *vm, const uint8_t *image, uint32_t image_len,
             const struct z_io *io)
{
    int err;

    /* Caller pre-fills vm->m.mem / vm->m.mem_size. */
    err = z_load(&vm->m, image, image_len);
    if (err != Z_OK) {
        return err;
    }

    vm->pc = z_initial_pc(&vm->m);
    vm->sp = 0;
    vm->nframes = 1;
    vm->frames[0].resume_pc = 0;
    vm->frames[0].base_sp = 0;
    vm->frames[0].nlocals = 0;
    vm->frames[0].pending_dest = 0xFF;

    vm->rng_state = 0x9E3779B9u;
    vm->inst_count = 0;
    vm->stop_reason = ZVM_OK;
    vm->msg = NULL;
    vm->trace = 0;

    if (io != NULL) {
        vm->io = *io;
    } else {
        vm->io.user = NULL;
        vm->io.putc = NULL;
        vm->io.getc = NULL;
    }
    return Z_OK;
}
