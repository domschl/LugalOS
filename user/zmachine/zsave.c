/* user/zmachine/zsave.c — save/restore of the complete VM state (v3)
 *
 * v1-3 SAVE/RESTORE hand "the dynamic memory plus the call stack" to
 * the interpreter (zmach06e RESTORE/SAVE); the byte format is an
 * interpreter choice.  This is LugalOS's own format — self-consistent
 * (files are produced and consumed by this interpreter), big-endian
 * fields, and deliberately not Quetzal (a v5-era container).
 *
 * Layout:
 *   "LZS3" | fmt u16 | release u16 | serial 6 |
 *   pc u32 | rng u32 | sp u32 | nframes u32 | dyn_len u32 |
 *   frames[nframes]: resume_pc u32 | base_sp u32 | nlocals u8 |
 *                    pending_dest u8 | locals[nlocals] u16 |
 *   stack[sp] u16 | dynamic bytes
 *
 * Freestanding: no libc.
 */
#include "zvm.h"

#define ZFMT 1u

/* --- little byte-stream helpers (big-endian on disk) --------------- */

static void p16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)(v & 0xFFu);
}

static void p32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v >> 24);
    *(*p)++ = (uint8_t)((v >> 16) & 0xFFu);
    *(*p)++ = (uint8_t)((v >> 8) & 0xFFu);
    *(*p)++ = (uint8_t)(v & 0xFFu);
}

static uint16_t g16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((uint16_t)(*(*p)) << 8);
    v = (uint16_t)(v | (*(*p + 1u)));
    *p += 2u;
    return v;
}

static uint32_t g32(const uint8_t **p)
{
    uint32_t v = (uint32_t)(*(*p)) << 24;
    v |= (uint32_t)(*(*p + 1u)) << 16;
    v |= (uint32_t)(*(*p + 2u)) << 8;
    v |= (uint32_t)(*(*p + 3u));
    *p += 4u;
    return v;
}

uint32_t zvm_serialize(struct z_vm *vm, uint8_t *buf, uint32_t cap,
                       uint32_t saved_pc)
{
    uint8_t *p = buf;
    uint8_t *end = buf + cap;
    uint32_t dyn = vm->m.static_base;      /* v1-4: end of dynamic mem */
    uint32_t i;
    struct z_frame *f;

    if (dyn > ZVM_DYN_MAX) {
        return 0u;
    }
    if (cap < ZVM_SAVEBUF_MAX) {
        /* buffer must hold our worst case; caller uses sizeof */
        return 0u;
    }
    (void)end;

    /* header */
    *p++ = 'L'; *p++ = 'Z'; *p++ = 'S'; *p++ = '3';
    p16(&p, ZFMT);
    p16(&p, z_release(&vm->m));
    {
        uint8_t serial[7];
        z_serial(&vm->m, (char *)serial);
        for (i = 0; i < 6u; i++) {
            *p++ = serial[i];
        }
    }
    p32(&p, saved_pc);
    p32(&p, vm->rng_state);
    p32(&p, vm->sp);
    p32(&p, vm->nframes);
    p32(&p, dyn);

    for (i = 0; i < vm->nframes; i++) {
        f = &vm->frames[i];
        p32(&p, f->resume_pc);
        p32(&p, f->base_sp);
        *p++ = f->nlocals;
        *p++ = f->pending_dest;
        {
            uint8_t l;
            for (l = 0; l < f->nlocals; l++) {
                p16(&p, f->locals[l]);
            }
            for (; l < Z_MAX_LOCALS; l++) {
                p16(&p, 0u);
            }
        }
    }
    for (i = 0; i < vm->sp; i++) {
        p16(&p, vm->stack[i]);
    }
    for (i = 0; i < dyn; i++) {
        *p++ = vm->m.mem[i];
    }
    return (uint32_t)(p - buf);
}

int zvm_deserialize(struct z_vm *vm, const uint8_t *buf, uint32_t len)
{
    const uint8_t *p = buf;
    const uint8_t *min = buf + 24u;
    uint32_t sp, nframes, dyn;
    uint32_t i;

    if (len < 24u) {
        return -1;
    }
    if (!(p[0] == 'L' && p[1] == 'Z' && p[2] == 'S' && p[3] == '3')) {
        return -1;
    }
    p += 4;
    if (g16(&p) != ZFMT) {
        return -1;
    }
    if (g16(&p) != z_release(&vm->m)) {
        return -1;
    }
    {
        uint8_t serial[7];
        z_serial(&vm->m, (char *)serial);
        for (i = 0; i < 6u; i++) {
            if (*p++ != serial[i]) {
                return -1;
            }
        }
    }
    {
        uint32_t pc = g32(&p);
        uint32_t rng = g32(&p);
        sp = g32(&p);
        nframes = g32(&p);
        dyn = g32(&p);
        if (sp > Z_STACK_WORDS || nframes == 0u || nframes > Z_MAX_FRAMES
            || dyn > vm->m.mem_size) {
            return -1;
        }
        /* conservative frame-size bound: full fixed slots, no trust */
        if (len < (uint32_t)(p - min) + 24u
            + nframes * 40u + sp * 2u + dyn) {
            return -1;
        }
        for (i = 0; i < nframes; i++) {
            struct z_frame *f = &vm->frames[i];
            uint8_t l;
            f->resume_pc = g32(&p);
            f->base_sp = g32(&p);
            f->nlocals = *p++;
            f->pending_dest = *p++;
            /* Each frame's stack base within the restored stack, and no
             * lower than its caller's: a return sets sp to it, and the
             * pop that follows indexes stack[] with no other check. */
            if (f->nlocals > Z_MAX_LOCALS || f->base_sp > sp
                || (i > 0u && f->base_sp < vm->frames[i - 1u].base_sp)) {
                return -1;
            }
            for (l = 0; l < Z_MAX_LOCALS; l++) {
                f->locals[l] = g16(&p);
            }
        }
        for (i = 0; i < sp; i++) {
            vm->stack[i] = g16(&p);
        }
        for (i = 0; i < dyn; i++) {
            vm->m.mem[i] = *p++;
        }
        vm->pc = pc;
        vm->rng_state = rng;
        vm->sp = sp;
        vm->nframes = nframes;
        vm->stop_reason = ZVM_OK;
        vm->msg = NULL;
        return 0;
    }
}
