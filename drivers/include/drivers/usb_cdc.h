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

// Console CDC Port (/dev/ttyACM0)
void usb_cdc_putc(char c);
char usb_cdc_getc(void);
bool usb_cdc_has_char(void);

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
