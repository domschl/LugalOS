/* user/zmachine/zvm.h — Z-machine Version 3 virtual CPU (LugalOS)
 *
 * Freestanding core: no libc.  All world interaction goes through the
 * z_io callback table, so the same object code runs in the host test
 * harness (libc stdio adapters in host_main.c) and in LugalOS firmware
 * (console adapters later).
 *
 * Semantics sources (external reference repo ~/gith/zmachine/docs/;
 * see README "Reference Documents — EXTERNAL" for sha256 and layout):
 *   zmach06e.txt — "The Z Machine" v0.6: opcode encodings (7.2), call
 *                  discipline (8.5), object table (8.6), verify.
 *   z-spec10.pdf — header (§11), stack (§6.3-6.4), objects (§12),
 *                  opcode tables (§14).
 *   spec-zip.pdf — instruction format cross-check (§2.2).
 */
#ifndef USER_ZMACHINE_ZVM_H
#define USER_ZMACHINE_ZVM_H

#include "zmem.h"

/* ------------------------------------------------------------------------
 * Sizes (zmach06e.txt §2.2 note: game stack usage per frame <= 4+locals,
 * total < 1024; we provision generously for host and firmware alike)
 * ------------------------------------------------------------------------ */
#define Z_STACK_WORDS   2048u
#define Z_MAX_FRAMES    128
#define Z_MAX_LOCALS    15      /* v3 limit */

/* save image bounds (see zsave.c); zork1 needs dyn $2c12 */
#define ZVM_DYN_MAX     0x8000u
#define ZVM_SAVEBUF_MAX (24u + (Z_MAX_FRAMES * 40u) + (Z_STACK_WORDS * 2u) \
                         + ZVM_DYN_MAX)

/* ------------------------------------------------------------------------
 * I/O callbacks.
 *
 * putc      : write one byte to the console output stream.
 * getc      : read one byte (or -1 when none available).  May block.


 * ------------------------------------------------------------------------ */
struct z_vm;  /* forward declaration for the callback signatures */

struct z_io {
    void *user;
    void (*putc)(void *user, uint8_t ch);
    int  (*getc)(void *user);
    /* v3 SAVE/RESTORE transport.  save writes len bytes, returns 0 on
     * success.  restore loads a save image into data (capacity cap),
     * sets *len, returns 0 on success; nonzero means "no save here". */
    int (*save)(void *user, const uint8_t *data, uint32_t len);
    int (*restore)(void *user, uint8_t *data, uint32_t cap, uint32_t *len);
};

/* ------------------------------------------------------------------------
 * Stop reasons (return value of zvm_step / zvm_run)
 * ------------------------------------------------------------------------ */
#define ZVM_OK              0   /* continue / keep stepping            */
#define ZVM_STOP_QUIT       1   /* quit instruction                    */
#define ZVM_STOP_RESTART    2   /* restart instruction                 */
#define ZVM_STOP_AREAD      3   /* aread reached (M4 implements)       */
#define ZVM_STOP_SAVE       4   /* save reached (M5 implements)        */
#define ZVM_STOP_RESTORE    5   /* restore reached (M5 implements)     */
#define ZVM_STOP_UNIMPL     6   /* unimplemented opcode (msg set)      */
#define ZVM_STOP_ERROR      7   /* runtime error (msg set)             */

struct z_frame {
    uint32_t resume_pc;         /* caller PC after instruction         */
    uint32_t base_sp;           /* stack pointer at routine entry      */
    uint8_t  nlocals;
    uint8_t  pending_dest;      /* result store target, 0xFF = none    */
    uint16_t locals[Z_MAX_LOCALS];
};

struct z_vm {
    struct z_machine m;

    uint32_t pc;

    uint16_t stack[Z_STACK_WORDS];
    uint32_t sp;

    struct z_frame frames[Z_MAX_FRAMES];
    uint32_t nframes;

    uint32_t rng_state;         /* deterministic PRNG (xorshift32)     */

    struct z_io io;

    uint64_t inst_count;
    int stop_reason;
    const char *msg;            /* last error / notice, for reporting  */

    uint8_t savebuf[ZVM_SAVEBUF_MAX];   /* SAVE/RESTORE staging area  */

    /* trace hook: called after each instruction fetch while enabled   */
    int trace;
};

/* Load the story into a caller-owned memory buffer and set up the start
 * frame (empty locals, empty stack, PC from header with the v3 +8 bias).
 * Returns Z_OK or a zmem error code. */
int zvm_init(struct z_vm *vm, const uint8_t *image, uint32_t image_len,
             const struct z_io *io);

/* Execute exactly one instruction.  Returns ZVM_OK to continue, or one
 * of the ZVM_STOP_* reasons. */
int zvm_step(struct z_vm *vm);

/* Execute until stop. */
int zvm_run(struct z_vm *vm);

/* Full-state serialization (zsave.c).  zvm_serialize returns the
 * image length or 0 on failure; saved_pc is the PC the restored game
 * resumes at (for SAVE that is the instruction after the branch
 * bytes).  zvm_deserialize returns 0 on success; on success the VM is
 * completely replaced by the image. */
uint32_t zvm_serialize(struct z_vm *vm, uint8_t *buf, uint32_t cap,
                       uint32_t saved_pc);
int zvm_deserialize(struct z_vm *vm, const uint8_t *buf, uint32_t len);

#endif /* USER_ZMACHINE_ZVM_H */
