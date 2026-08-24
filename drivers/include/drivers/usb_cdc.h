#ifndef DRIVERS_USB_CDC_H
#define DRIVERS_USB_CDC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/p9_link.h"

void usb_cdc_init(void);
void usb_cdc_task(void);
void usb_cdc_debug_dump(void);
bool usb_cdc_is_connected(void);

// M4.5, plan/phase12_microkernel_migration.md, Part B: starts a dedicated
// background task that services usb_cdc_task() on its own continuous
// schedule, replacing the "whatever happens to be busy-waiting" opportunistic
// polling this driver used to depend on -- no chan_call() endpoint, same
// shape as heartbeat_task_start() (drivers/uart_rp2350.c): nothing calls
// this as a request/response operation. Must run after sched_init(). Not
// fatal if it fails: usb_cdc_task_alive() reports so, and the one caller
// that still needs to know (kernel/time.c's time_delay_us(), which also
// runs during early boot before this task exists) falls back to pumping
// usb_cdc_task() directly.
int usb_cdc_task_start(void);
bool usb_cdc_task_alive(void);

// Console CDC Port (/dev/ttyACM0)
void usb_cdc_putc(char c);
char usb_cdc_getc(void);
bool usb_cdc_has_char(void);

/* True if a Ctrl-C is sitting unread in the console ring, WITHOUT consuming
 * it -- see the definition in drivers/usb_cdc.c for why the non-consuming
 * part is load-bearing. */
bool usb_cdc_peek_interrupt(void);

// 9P Network Interconnect CDC Port (/dev/ttyACM1)
int usb_cdc_write_net(const uint8_t *buf, size_t len);
int usb_cdc_read_net(uint8_t *buf, size_t max_len);

// A3b link_usb_cdc (plan/phase5_distributed_design.md): a p9_link_t backed
// by ACM1/EP4's bulk data path. Unlike the shared-UART demux, this is a
// dedicated, physically separate channel (its own USB endpoint pair), so it
// needs no RX demultiplexing -- the same reasoning drivers/virtio_console.c
// already established for QEMU. Plain length-prefixed framing (9P's own
// 4-byte size header is enough over a reliable, ordered USB bulk pipe), not
// SLIP. Returns NULL when this isn't the RP2350 hardware target (no second
// CDC interface exists to back it).
p9_link_t *usb_cdc_get_net_link(void);

#endif // DRIVERS_USB_CDC_H
