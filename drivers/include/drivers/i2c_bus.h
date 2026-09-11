#ifndef DRIVERS_I2C_BUS_H
#define DRIVERS_I2C_BUS_H

#include "lugalos_config.h"
#include <stdbool.h>
#include <stdint.h>

/* The I2C bus itself: one controller, one owning task, one generic transfer
 * (category E, plan/phase30_driver_framework.md).
 *
 * ## Why this is its own file
 *
 * All of this used to live in `drivers/i2c_rtc.c`, which meant the bus was a
 * part of the *RTC driver*: a sensor reached the bus by including an RTC's
 * header, `drivers/at24c32.c` kept a second copy of the controller registers
 * rather than depend on one, and the message about whether a controller
 * existed at all was a property of whether a clock chip had been found.
 * Thirteen files included `i2c_rtc.h`, most of them wanting only this.
 *
 * ## What a bus is, as distinct from a device
 *
 * I2C is **1:n** -- several devices share one pair of wires and take turns per
 * transaction -- which is what makes an owner necessary. SPI on these boards
 * is 1:1 by board design (`cmake/board-rp2350.cmake`: *"the SD card bus. Not
 * a dev_wire_t entry -- nothing else ever"*), so it needs no equivalent and
 * does not have one.
 *
 * Note this is a third relationship, not the one `kernel/device.h` models.
 * `dev_wire_t` describes a wire whose sharers are *mutually exclusive*; these
 * devices are all live at once.
 *
 * ## The rule for anything new on this bus
 *
 * **Add a file, not an opcode.** A device driver is ordinary M-mode code that
 * calls `i2c_xfer()` -- see `drivers/bme280.c`, and now `drivers/i2c_rtc.c`
 * and `drivers/at24c32.c` too. The task below serves exactly one operation,
 * and that is what keeps it from having to understand any device: it used to
 * serve seven, six of them device-specific, which forced a second copy of
 * every register map into `.utext` for the U-mode half to use.
 */

/* Whether this build has a real I2C controller behind the calls below.
 *
 * Named as a capability rather than spelled `#if defined(CONFIG_BOARD_RP2350)`
 * at each site, because that is what those sites meant and it stopped being
 * what it said the moment a second board grew a controller: the ESP32-P4
 * spent E7 announcing "No I2C controller on this target" while answering a
 * bus scan (plan/phase27_esp32p4_bringup.md E7). A board that gains one adds
 * itself here, once. */
#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32P4)
#define I2C_HAVE_CONTROLLER 1
#else
#define I2C_HAVE_CONTROLLER 0
#endif

/* The bounds `i2c_xfer()` enforces, public because its callers size buffers
 * against them. Set by the largest single transaction any device on this bus
 * performs, which is an AT24C32 page write: two address bytes plus a 32-byte
 * page. */
#define I2C_XFER_WMAX 40u
#define I2C_XFER_RMAX 64u

/* Brings up the controller: clocks, reset, pads, master mode, timing.
 * Idempotent, and a no-op where there is no controller. Must run before any
 * device is probed. */
void i2c_bus_init(void);

/* One transfer: write `wlen` bytes, then read `rlen`; either half may be
 * empty. Routed through the owning task once it is running, and taken
 * straight to the hardware before that task exists -- which is what lets a
 * device driver be ordinary M-mode code that knows nothing about the task.
 *
 * This is the whole device-facing surface of the bus. */
bool i2c_xfer(uint8_t addr, const uint8_t *w, uint32_t wlen,
              uint8_t *r, uint32_t rlen);

/* Prints which addresses answer. Diagnostic: it probes the controller
 * directly rather than going through the task, which is why its answers can
 * differ from a device's -- see tests/hw/README.md on what that split means
 * when it appears. */
void i2c_scan_bus(void);

/* The owning task. Must run after sched_init(); every device keeps working
 * through direct hardware access if this fails or has not run yet, same as
 * every other M4.5 driver-task conversion. Returns the task's pid, or -1. */
int  i2c_task_start(void);

/* How many chan_call()s the task has served since boot -- see
 * drivers/spisd_rp2350.c's g_blk_calls comment for the reasoning. */
uint32_t i2c_task_call_count(void);

/* Internal to the bus and its isolation test; not for device drivers. */
bool i2c_task_alive(void);
int  i2c_task_call(const uint8_t *req, uint32_t req_len,
                   uint8_t *resp, uint32_t resp_max);

#if defined(CONFIG_BOARD_RP2350)
/* Drives a deliberate out-of-domain store from the bus task's U-mode half and
 * reports whether it was contained. */
bool i2c_isolation_test(uintptr_t *out_canary, bool *out_exited_clean);
/* `i2cdiag` -- see its definition for how to read the result. */
void i2c_rp2350_diag(uint8_t addr, uint8_t reg);
#endif
#if defined(CONFIG_BOARD_ESP32P4)
void i2c_p4_diag(void);
void i2c_p4_last_status(uint32_t *int_raw, uint32_t *sr);
#endif

#endif // DRIVERS_I2C_BUS_H
