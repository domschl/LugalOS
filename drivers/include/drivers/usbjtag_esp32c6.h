#ifndef DRIVERS_USBJTAG_ESP32C6_H
#define DRIVERS_USBJTAG_ESP32C6_H

#include <stdint.h>
#include <stdbool.h>

/* The C6's USB-Serial/JTAG console, shared with LLP (49.3): see the comment
 * above usbjtag_llp_arm() in drivers/usbjtag_esp32c6.c. Armed, the console's
 * bytes and LLP's SLIP frames share the port, and LLP's pump becomes the only
 * writer to it: it calls usbjtag_llp_write() for frames and
 * usbjtag_llp_service() every turn, which writes out queued console text. */
void usbjtag_llp_arm(bool on);
int  usbjtag_llp_read(uint8_t *buf, uint32_t max);
int  usbjtag_llp_write(const uint8_t *buf, uint32_t len);
/* A frame streamed one byte at a time (kernel/llp.c's llp_serial_emit()),
 * closed by usbjtag_llp_end(). */
void usbjtag_llp_putc(uint8_t b);
void usbjtag_llp_end(void);
void usbjtag_llp_service(void);
void usbjtag_llp_counts(uint32_t *llp_rx_drops, uint32_t *con_rx_drops, uint32_t *con_tx_drops);

#endif
