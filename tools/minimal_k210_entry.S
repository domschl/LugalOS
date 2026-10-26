/* K210 silicon truth probe (65.1) — startup and trap plumbing.
 *
 * Trap contract of this legacy custom RV64 core, as MEASURED on the
 * board (full table in plan/phase65_k210_support.md §1.5):
 *   - mtvec is writable and selects the handler window; _start points it
 *     at trap_entry and every cause (sync, msip, timer) arrives there.
 *   - mepc: hardware capture is full width; SOFTWARE WRITES ONLY THE
 *     LOW 16 BITS latch (mask scan). Re-targeting returns works because
 *     all code lives inside one 64 KiB window above 0x8000_0000.
 *   - mret pops the hardware trap latch and jumps to mepc (captured high
 *     | written low 16). mret with no latch set is a no-op. Resuming a
 *     handler by plain `jr` leaves the latch set and corrupts the NEXT
 *     mret; the epilogue below therefore always returns via mret,
 *     exactly like the vendor crt.S: csrw mepc, resume; mret.
 *   - BROM leaves mstatus.MIE clear (boot value 0x7800); interrupts need
 *     it set explicitly. CLINT msip/mtimecmp delivery and re-arm were
 *     verified through this epilogue.
 *
 * hart 1 (ISP boot): starts running this image seconds after hart 0, not
 * synchronously; the secondary path marks arrival, waits for a release
 * flag, enables its own MSIE/MIE. Flash boot (vendor hello_world) does
 * start both harts synchronously with the spin-flag + CLINT-IPI recipe.
 */

#define STK_BASE          0x80500000
#define STK_SHIFT         16
#define HART1_MAGIC_ADDR  0x80400000
#define HART1_MAGIC       0x4b324831

  .section .text.start, "ax", @progbits
  .globl _start
_start:
  csrw mie, zero
  csrw mip, zero

  csrr t0, mhartid
  li   t1, STK_BASE
  addi t2, t0, 1
  slli t2, t2, STK_SHIFT
  add  sp, t1, t2

  bnez t0, secondary

  la   t0, _bss_start
  la   t1, _bss_end
2:
  sd   zero, 0(t0)
  addi t0, t0, 8
  bltu t0, t1, 2b

  /* crt-style delegation clear (numeric CSR numbers; the names need S
   * extensions in the assembler). Faulted writes are impossible here
   * pre-trap; the SDK's crt.S writes these unconditionally. */
  li   t0, 0
  csrrw x0, 0x302, t0                /* medeleg */
  csrrw x0, 0x304, t0                /* mideleg */

  /* mtvec = our entry: the ROM handler leaves via mtvec, so this
   * gives the probe its trap entry without touching the ROM table. */
  la   t0, trap_entry
  csrw mtvec, t0

  call k210_probe_main
5:
  j 5b

secondary:
  /* Vendor model (entry.c/crt.S): BOTH harts start simultaneously at
   * 0x80000000; the image parks hart 1 with a memory flag and wakes it
   * with a CLINT IPI (register_core1 -> core_enable). Reproduce that
   * rendezvous here: flag layout at HART1_MAGIC_ADDR:
   *   +0 hart1-ran marker, +8 hart0 release, +16 hart1 in-IPI-loop   */
  li   t0, HART1_MAGIC_ADDR
  li   t1, HART1_MAGIC
  sd   t1, 0(t0)
20:
  ld   t2, 8(t0)
  beqz t2, 20b
  li   t1, 0x4b324831                  /* "K2H1" arrived-after-release */
  sd   t1, 16(t0)
  li   t3, 8
  csrs mie, t3                         /* MSIE for hart 1              */
  li   t3, 8
  csrs mstatus, t3                     /* global MIE                   */
21:
  j 21b                                /* real delivery lands in trap  */

  /* ---- trap entry (also the mret landing point) ------------------------
   * Entered from the ROM handler with the real mcause/mepc intact. The
   * frame layout matches C trap_frame_t: regs at 8*nr, mcause at 256,
   * mepc at 264. 'T' marks entry; if mcause bit63 is clear (sync) we
   * note whether mepc points at an ecall so the C side can resume by
   * jumping to mepc+4.
   */
  .align 6
trap_entry:
  addi sp, sp, -288
  sd  ra,   0(sp)
  sd  t0,  16(sp)
  sd  t1,  24(sp)
  addi t0, sp, 288
  sd  t0,   8(sp)                    /* original sp                  */
  sd  gp,  32(sp)
  sd  tp,  40(sp)
  sd  t2,  56(sp)
  sd  s0,  64(sp)
  sd  s1,  72(sp)
  sd  a0,  80(sp)
  sd  a1,  88(sp)
  sd  a2,  96(sp)
  sd  a3, 104(sp)
  sd  a4, 112(sp)
  sd  a5, 120(sp)
  sd  a6, 128(sp)
  sd  a7, 136(sp)
  sd  s2, 144(sp)
  sd  s3, 152(sp)
  sd  s4, 160(sp)
  sd  s5, 168(sp)
  sd  s6, 176(sp)
  sd  s7, 184(sp)
  sd  s8, 192(sp)
  sd  s9, 200(sp)
  sd  s10,208(sp)
  sd  s11,216(sp)
  sd  t3, 224(sp)
  sd  t4, 232(sp)
  sd  t5, 240(sp)
  sd  t6, 248(sp)
  csrr t0, mcause
  sd  t0, 256(sp)
  csrr t0, mepc
  sd  t0, 264(sp)

  li  t1, 0x38000000                 /* marker 'T' */
  li  t0, 'T'
  li  t3, 0x80000000
7:
  lw  t2, 0(t1)
  and t4, t2, t3
  bnez t4, 7b
  sw  t0, 0(t1)

  li  t0, 0x1800
  csrs mstatus, t0                   /* keep mret returning to M     */

  la  t0, g_trap_ticks
  ld  t1, 0(t0)
  addi t1, t1, 1
  sd  t1, 0(t0)

  mv  a0, sp
  call trap_c

  li  t1, 0x38000000                 /* marker 'c' */
  li  t0, 'c'
8:
  lw  t2, 0(t1)
  and t4, t2, t3
  bnez t4, 8b
  sw  t0, 0(t1)

  /* Epilogue: THE vendor contract, measured correct on this core:
   * csrw mepc, resume (only low 16 bits land; upper bits keep the
   * hardware capture, and all code lives in one 64K window above
   * 0x8000_0000) then mret, which pops the hardware trap latch. A
   * plain "jr" resume leaves the latch set so the next mret fires
   * from stale state (measured storm); never jr out of a trap. */
  ld  t0, 264(sp)
  csrw mepc, t0
  ld  ra,   0(sp)
  ld  t0,  16(sp)
  ld  t1,  24(sp)
  ld  gp,  32(sp)
  ld  tp,  40(sp)
  ld  t2,  56(sp)
  ld  s0,  64(sp)
  ld  s1,  72(sp)
  ld  a0,  80(sp)
  ld  a1,  88(sp)
  ld  a2,  96(sp)
  ld  a3, 104(sp)
  ld  a4, 112(sp)
  ld  a5, 120(sp)
  ld  a6, 128(sp)
  ld  a7, 136(sp)
  ld  s2, 144(sp)
  ld  s3, 152(sp)
  ld  s4, 160(sp)
  ld  s5, 168(sp)
  ld  s6, 176(sp)
  ld  s7, 184(sp)
  ld  s8, 192(sp)
  ld  s9, 200(sp)
  ld  s10,208(sp)
  ld  s11,216(sp)
  ld  t3, 224(sp)
  ld  t4, 232(sp)
  ld  t5, 240(sp)
  ld  t6, 248(sp)
  ld  sp, 8(sp)
  mret

  /* int try_begin(jmpb *buf): 0 on entry, 1 after a guarded throw. */
  .align 2
  .globl try_begin
try_begin:
  sd  ra,  0(a0)
  sd  sp,  8(a0)
  sd  s0, 16(a0)
  sd  s1, 24(a0)
  la  t0, g_try_buf
  sd  a0,  0(t0)
  li  a0, 0
  ret

  .align 2
  .globl try_disarm
try_disarm:
  la  t0, g_try_buf
  sd  zero, 0(t0)
  ret

  /* int mret_to_umode(void *target): MPP := U, mret to target.
   * Measurement: does mret honor mepc/privilege on this core? */
  .align 2
  .globl mret_to_umode
mret_to_umode:
  li   t0, 0x1800
  csrc mstatus, t0                 /* MPP = U                       */
  csrw mepc, a0
  mret
