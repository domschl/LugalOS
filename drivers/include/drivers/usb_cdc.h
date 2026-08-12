#ifndef DRIVERS_USB_CDC_H
#define DRIVERS_USB_CDC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/p9_link.h"
#include "lugalos_config.h"

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

/* UCI Chess CDC Port (/dev/ttyACM2), J5 (plan/phase10_chess_completion.md).
 * Unlike ACM0 (always the console) and ACM1 (always P9), this endpoint's
 * USB descriptor is always present when CONFIG_ENABLE_CHESS is compiled in
 * (a host always enumerates the third port -- there is no way to make an
 * interface not-exist-until-later without forcing a full USB re-enumeration,
 * which this driver deliberately does not do), but its hardware is left
 * completely unconfigured -- unresponsive, nothing read or written -- until
 * usb_cdc_uci_ensure_init() runs. That only happens from chess_uci_run()
 * (chess_ui.c), itself only reachable via the `chess-uci` Lisp primitive, so
 * nothing on this port is active unless a user (typically usr_init.lisp)
 * explicitly asks for it. "Chess is just one specific application" (the
 * user's own framing) is why this lives under CONFIG_ENABLE_CHESS rather
 * than being board-level infrastructure like usbcon/usbnet in kernel/board.c. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_CHESS
void usb_cdc_uci_ensure_init(void);
bool usb_cdc_uci_has_char(void);
uint32_t usb_cdc_uci_read(uint8_t *buf, uint32_t max);
int usb_cdc_uci_write(const uint8_t *buf, uint32_t len);
#endif

#endif // DRIVERS_USB_CDC_H
