/*
 * LugalOS Hardware Driver: Waveshare Pico-Clock-Green LED matrix (L2,
 * plan/phase11_pico_clock_green.md).
 *
 * Two SM16106 column shift registers + one SM5166P row-address decoder
 * driving an 8-row x 24-column LED matrix, plus an LDR (GP26/ADC0) for
 * auto-brightness. Protocol reverse-engineered from the vendor firmware at
 * ~/gith/Pico-Clock-Green (Pico-Clock-Green.c, define.h, ziku.h) -- see the
 * plan doc's L0 section for the citations.
 */

#ifndef DRIVERS_PICO_CLOCK_GREEN_H
#define DRIVERS_PICO_CLOCK_GREEN_H

#include <stdint.h>

/* GPIO/ADC bring-up and a blanked display buffer. Call once at boot,
 * mirroring st7735_init()/tm1638_init() (kernel/main.c). */
void pico_clock_green_init(void);

/* Raw 12-bit LDR reading (0-4095), the same single-shot conversion the
 * (task-internal) scan loop samples once per frame for auto-brightness. A
 * diagnostic primitive, not something the display loop needs -- exposed
 * so LDR polarity/threshold can be checked against real ambient light on
 * real hardware instead of assumed correct from the register-level port
 * (see plan/phase11_pico_clock_green.md L2's LDR_DARK_THRESHOLD note). */
uint16_t pico_clock_green_read_light(void);

/* L4 (plan/phase11_pico_clock_green.md): the blocking appliance loop behind
 * the `(clock)` Lisp primitive. Alternates the display between time (most
 * of the time) and temperature (briefly, only when the DS3231 is actually
 * detected), driving the ~1kHz row scan continuously. Returns on Ctrl-C,
 * same console_interrupt_requested()/_clear() convention chess_ui.c already
 * uses ([[standardized_interrupt_polling]]) -- not a new mechanism.
 * Allocates nothing on the heap, so there is nothing to free on return
 * ([[heap_stateless_user_programs]] is satisfied trivially, not by effort).
 *
 * M4.5, plan/phase12_microkernel_migration.md, Part B: as a task, this
 * whole loop runs as ONE chan_call() -- not one per row-scan step, which
 * would put chan_call() on a ~1kHz hot path nothing else in this codebase
 * comes close to. The caller (whatever evaluates `(clock)`) simply blocks
 * for the call's duration, same as any other chan_call(), just a much
 * longer one; the task-side loop still polls Ctrl-C every ~1ms internally,
 * so responsiveness is unchanged. show_time()/show_temperature_c()/clear()/
 * the per-row scan step used to be separate public entry points here; none
 * of them had a caller outside this driver, so they are file-internal now
 * rather than wire ops nothing would ever address individually. */
void pico_clock_green_run(void);

/* Must run after sched_init(); pico_clock_green_init() itself stays a
 * direct-hardware call (it runs before a task table exists). Returns the
 * task's pid, or -1. */
int pico_clock_green_task_start(void);

// M4.5 verify: how many chan_call()s the shared "clock" task has served
// since boot -- see drivers/spisd_rp2350.c's g_blk_calls comment.
uint32_t pico_clock_green_task_call_count(void);

#endif /* DRIVERS_PICO_CLOCK_GREEN_H */
