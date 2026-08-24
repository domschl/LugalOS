#ifndef DRIVERS_UART_NET_H
#define DRIVERS_UART_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/p9_link.h"

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

void uart_net_init(void);

// SLIP Framing Helper Functions
int slip_encode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max);
int slip_decode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max);

// Transmits a SLIP-framed 9P request over UART / USB serial channel and receives 9P response
int uart_net_send_9p(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max);

// High-level 9P RPC over UART serial transport
int uart_net_rpc(const char *write_payload, char *read_out_buf, uint32_t read_max);

// A3's link_uart_slip backend: a p9_link_t that speaks SLIP-framed 9P
// directly over the physical UART (drivers/uart.h's uart_getc()/uart_putc()).
// This is the same wire the interactive console uses, so servicing it is
// mutually exclusive with the shell (see kernel/shell.c's `p9serve` command,
// A3a "headless" mode) -- there is no RX demultiplexing (that's A3b, see
// below).
p9_link_t *uart_slip_get_link(void);

/* One received byte into a SLIP frame accumulator. Returns 1 when `frame`
 * holds a complete frame of `*frame_len` bytes, 0 otherwise; `escaping`
 * carries the escape state across calls. Shared between the console link
 * above and the gateway's UART1 downlink (N5,
 * plan/phase18_networking_and_auth.md). */
int slip_feed(uint8_t c, uint8_t *frame, uint32_t frame_cap,
              uint32_t *frame_len, bool *escaping);

/* N5: UART1 as a dedicated 9P downlink to another board -- SLIP-framed, no
 * console duties, its own pins. Init runs from kernel_main(); the link is
 * NULL until it has. See drivers/uart1_link_rp2350.c for why this is its own
 * small driver rather than a second instance of the console UART's. */
void uart1_link_init(void);
p9_link_t *uart1_get_link(void);

/* Raw wire test: dump UART1's own registers, send a plain-text burst, then
 * report whatever arrives for `listen_ms`. Run it on both boards to find out
 * which direction is silent. */
void uart1_wire_test(unsigned listen_ms);

// --- A3b: shared-wire demux (plan/phase5_distributed_design.md) ---
//
// Lets the interactive console and SLIP-framed 9P traffic coexist on one
// physical UART -- the single-cable RP2350/CP2102 story A3a's headless mode
// doesn't cover. The platform UART driver (drivers/uart_16550.c,
// drivers/uart_rp2350.c) owns the actual hardware registers; this module
// owns the demux state machine (byte ring for console bytes + SLIP frame
// assembler) so it's implemented once, not once per platform.
//
// Framing rule: a byte stream where SLIP_END (0xC0) delimits 9P frames the
// same way link_uart_slip already does. Bytes seen while *not* accumulating
// an open frame go to the console ring; a SLIP_END starts frame
// accumulation; the next SLIP_END ends it. This relies on 0xC0 never being
// a byte a human types at a keyboard (true for plain ASCII input, including
// every VT100/xterm arrow-key escape sequence this kernel's line editor
// parses) -- see drivers/uart_net.c for the fuller tradeoff note. Disabled
// by default (uart_has_char()/uart_getc() behave exactly as before, reading
// hardware directly) so nothing about existing console behavior changes
// unless a user explicitly opts in via the `p9share` shell command.
typedef bool (*uart_raw_has_char_fn)(void);
typedef uint8_t (*uart_raw_getc_fn)(void);

// Called once by the platform UART driver's uart_init(), supplying direct
// hardware byte access. Safe to call more than once (e.g. RP2350 also
// mirrors uart_init() logic); the most recent registration wins.
void uart_demux_init(uart_raw_has_char_fn has_char, uart_raw_getc_fn getc);

void uart_demux_set_enabled(bool enabled);
bool uart_demux_is_enabled(void);

// Pumps the demux (drains any hardware bytes ready right now, routing each
// into the console ring or the frame assembler) and reports whether a
// console byte is now available. Never blocks. Returns false if disabled or
// uninitialized.
bool uart_demux_console_has_char(void);
// Caller must have just confirmed uart_demux_console_has_char().
char uart_demux_console_getc(void);

// The demuxed p9_link_t. Its poll() is what actually drives the hardware
// byte draining above (console-side calls alone don't pump it when the
// console has nothing to read) -- register it with
// p9_link_register_background() to get 9P service while the shell sits
// idle at its prompt, exactly like drivers/virtio_console.c's link.
p9_link_t *uart_demux_get_link(void);

#endif // DRIVERS_UART_NET_H
