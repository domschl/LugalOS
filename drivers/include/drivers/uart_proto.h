#ifndef LUGALOS_DRIVERS_UART_PROTO_H
#define LUGALOS_DRIVERS_UART_PROTO_H

/* The console UART's wire protocol (G4, plan/phase30_driver_framework.md).
 *
 * Three drivers serve a console UART over chan_call() -- uart_16550.c (QEMU),
 * uart_rp2350.c and uart_esp32p4.c -- and all three had these three opcodes
 * defined privately. The opcodes are one thing now.
 *
 * ## The protocol is NOT the same on all three, and that is not an oversight
 *
 * G4 set out to give the three drivers one task half. Two of them can have
 * it; the third cannot, for a reason worth stating rather than working
 * around.
 *
 *   'H'      -> has-char query, non-consuming. resp: 1 byte, 0 or 1.
 *   'W', ... -> write the req_len-1 bytes following the opcode. resp: empty.
 *
 *   'R'      -> read. **Two different operations**, depending on where the
 *               serving task runs:
 *
 *               *Kernel-mode servers* (uart_16550.c, uart_esp32p4.c) block
 *               until a character arrives and reply with 1 byte: the
 *               character. Waiting happens in the server.
 *
 *               *The U-mode server* (uart_rp2350.c) cannot block -- U-mode
 *               code has no task_block() and no irq_save() -- so it reads if
 *               ready and replies with 2 bytes: a status (1 if a character
 *               was read, 0 if none was ready) and the character, valid only
 *               when the status is 1. Waiting happens in the *client's* loop
 *               instead (see uart_getc() in that file).
 *
 * So the difference is a consequence of the execution context, not of the
 * device: a serve loop that may not block cannot offer a blocking read. That
 * is also why uart_rp2350.c keeps its own serve loop rather than joining
 * drivers/driver_task.c's -- along with two other U-mode constraints its
 * body has to satisfy and a kernel-mode one does not (no string literals,
 * which land in .rodata outside the granted domain; no switch, whose jump
 * table does the same).
 *
 * A protocol that looked uniform here and was not would be worse than one
 * that says where it differs. */

#include <stdint.h>

#define UART_REQ_HASCHAR ((uint8_t)'H')
#define UART_REQ_READ    ((uint8_t)'R')
#define UART_REQ_WRITE   ((uint8_t)'W')

/* 'R''s reply on a U-mode server: (status, char). The kernel-mode servers
 * reply with 1 byte and never need this. */
#define UART_RESP_CAP    2

#endif /* LUGALOS_DRIVERS_UART_PROTO_H */
