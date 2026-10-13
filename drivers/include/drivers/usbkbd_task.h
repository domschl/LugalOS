#ifndef LUGALOS_DRIVERS_USBKBD_TASK_H
#define LUGALOS_DRIVERS_USBKBD_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/piousb.h"

/* The `kbd` task's state -- 36.8, drivers/usbkbd_host.c. On the RP2350 it
 * lives in the task's own 1 KB grant; the kernel only reads it (`kbd`,
 * `kbdlog`, and 36.9's console input, which consumes the event ring).
 *
 * 47.9: what is above the bus --
 * enumeration, the hub, the boot keyboard, the console input and the
 * reports -- is drivers/usbkbd_host.c, shared; a board brings a *port*, the
 * five functions at the end, which speak one USB transaction at a time in
 * drivers/piousb.h's vocabulary (PIOUSB_OP_*, PIOUSB_OK/NAK/STALL/...).
 * The RP2350's port is the PIO-USB engine (drivers/usbkbd_rp2350.c, U-mode);
 * the LCD-7B's is the OTG HS core (drivers/usbhost_esp32p4.c, kernel). */

#include "lugalos_config.h"

#if (defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)) || \
    (defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_USBHOST_DWC2))
#define USBKBD_HOST_PRESENT 1
#endif

#define USBKBD_MAX_DEV   4
#define USBKBD_MAX_PORTS 8
#define USBKBD_EV_RING   64u     /* a power of two */

enum {
    USBKBD_PH_WAIT = 0, USBKBD_PH_ROOTRESET, USBKBD_PH_DESC8, USBKBD_PH_ADDRESS,
    USBKBD_PH_DESC18, USBKBD_PH_CONFIG, USBKBD_PH_SETCONFIG, USBKBD_PH_PROTOCOL,
    USBKBD_PH_HUBDESC, USBKBD_PH_HUBPOWER, USBKBD_PH_PORTRESET, USBKBD_PH_RUNNING,
    USBKBD_PH_OFF,
};

typedef struct {
    uint8_t  addr;           /* 0: slot free */
    uint8_t  hub_port;       /* 0: at the root */
    uint8_t  full_speed;
    uint8_t  dev_class;
    uint16_t vid, pid;
    uint8_t  mps0, cfg_value, is_hub, hub_ep;
    uint8_t  is_kbd, kbd_iface, kbd_ep, kbd_mps;
    uint8_t  kbd_interval, toggle;
    uint8_t  high_speed;     /* 47.9: a high-speed device (a USB 2.0 hub) */
    uint8_t  _pad;
    uint32_t last_poll;      /* ms */
} usbkbd_dev_t;

typedef struct {
    piousb_shared_t  *usb;           /* RP2350: set by the kernel before the task runs */
    volatile uint32_t off_request;   /* kernel: 1 = hand the port back and wait */
    volatile uint32_t phase;         /* USBKBD_PH_* */
    volatile uint32_t last_error;    /* phase << 16 | status << 8 | address or port */
    volatile uint32_t enumerations, failures, root_resets, root_detaches, detaches;
    volatile uint32_t polls, reports, naks, rollovers, lowspeed_seen, xfers, retries;
    uint8_t           hub_ports, hub_addr, next_addr;
    uint8_t           ls;            /* the device being talked to is low speed */
    /* 47.9: and where it is -- 0 at the root (or the hub itself), else the
     * hub port in front of it -- and whether it is high speed. A full- or
     * low-speed device behind a high-speed hub is reached through the hub's
     * transaction translator (split transactions, drivers/usbhost_esp32p4.c);
     * the RP2350's full-speed engine has no use for either. */
    uint8_t           tt_port, hs, _pad2[2];
    uint16_t          port_status[USBKBD_MAX_PORTS];
    usbkbd_dev_t      dev[USBKBD_MAX_DEV];
    uint8_t           report[8];     /* the last boot report */
    uint8_t           desc18[20];    /* 47.9: the last device descriptor read (18 bytes) */
    volatile uint32_t ev_head;       /* written by the task */
    uint32_t          ev[USBKBD_EV_RING];
    uint8_t           buf[256];      /* descriptor scratch */
} usbkbd_state_t;

/* After sched_init(), with the USB engine running: start the task. It takes
 * the port from the kernel (usbprobe then answers "busy"). */
int usbkbd_start(void);
const usbkbd_state_t *usbkbd_state(void);

/* `kbd`: what was found, where enumeration stands. `kbdlog [s]`: events. */
void usbkbd_report(void);
void usbkbd_dump_desc(void);

/* `kbd off` / `kbd on`: the task hands the port back to the kernel (for
 * usbprobe and its loopback) and waits; on, it takes it back and starts
 * over from a bus reset. Returns false if the task did not answer. */
bool usbkbd_set_off(bool off);
void usbkbd_log(uint32_t seconds);

/* --- 47.9: the port a board provides ---------------------------------------
 *
 * Called from the task (on the RP2350 in U-mode: these live in .kbdtext and
 * reach the kernel only by ecall). */

/* One transaction: PIOUSB_OP_SETUP (8 bytes, DATA0), _IN (expect DATA1 if
 * `data1`; up to in_max bytes into in[], *in_len says how many), _OUT (`len`
 * bytes, DATA1 if `data1`), or _RESET (the root port). k->ls says the device
 * is low speed, k->hs high speed, and k->tt_port which port of the hub at
 * k->hub_addr it is behind (0: the root port, or the hub itself). A port's
 * RESET may set k->ls/k->hs for the root device. Returns PIOUSB_*: a NAK is
 * PIOUSB_NAK, never retried here. */
uint32_t usbkbd_port_xfer(usbkbd_state_t *k, uint32_t op, uint32_t addr, uint32_t ep, uint32_t data1,
                          const uint8_t *data, uint32_t len, uint8_t *in, uint32_t in_max, uint32_t *in_len);
uint32_t usbkbd_port_ms(void);
void     usbkbd_port_sleep(uint32_t ms);
/* Something is on the root port and it is enabled, and the kernel has not
 * asked for the port back: the session goes on. */
bool     usbkbd_port_up(usbkbd_state_t *k);

/* --- 47.9: the shared host, which the port's task runs ----------------------
 *
 *   for (;;) { usbkbd_host_forget(k); wait until something is attached;
 *              if (usbkbd_host_session(k)) k->root_detaches++; }
 *
 * A session resets the root port, enumerates what is there (and behind it, if
 * it is a hub), and polls the keyboards until usbkbd_port_up() goes false.
 * False if it failed before it got going (it has slept a second by then). */
void usbkbd_host_forget(usbkbd_state_t *k);
bool usbkbd_host_session(usbkbd_state_t *k);

/* Kernel side, after the task is created: the state the reports and the
 * console read, and the keyboard as a console input source (36.9). */
void usbkbd_host_attach(usbkbd_state_t *k);

/* 47.9: `usb [regs]` on the ESP32-P4's OTG HS port (drivers/usbhost_esp32p4.c):
 * the controller, the port, and the last device's descriptors. */
void usbhost_command(const char *args);

#endif /* LUGALOS_DRIVERS_USBKBD_TASK_H */
