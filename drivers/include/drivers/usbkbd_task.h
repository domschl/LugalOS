#ifndef LUGALOS_DRIVERS_USBKBD_TASK_H
#define LUGALOS_DRIVERS_USBKBD_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/piousb.h"

/* The `kbd` task's state -- 36.8, drivers/usbkbd_rp2350.c. It lives in the
 * task's own 1 KB grant; the kernel only reads it (`kbd`, `kbdlog`, and
 * 36.9's console input, which consumes the event ring). */

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
    uint8_t  kbd_interval, toggle, _pad[2];
    uint32_t last_poll;      /* ms */
} usbkbd_dev_t;

typedef struct {
    piousb_shared_t  *usb;           /* set by the kernel before the task runs */
    volatile uint32_t off_request;   /* kernel: 1 = hand the port back and wait */
    volatile uint32_t phase;         /* USBKBD_PH_* */
    volatile uint32_t last_error;    /* phase << 16 | status << 8 | address or port */
    volatile uint32_t enumerations, failures, root_resets, root_detaches, detaches;
    volatile uint32_t polls, reports, naks, rollovers, lowspeed_seen, xfers, retries;
    uint8_t           hub_ports, hub_addr, next_addr;
    uint8_t           ls;            /* the device being talked to is low speed */
    uint16_t          port_status[USBKBD_MAX_PORTS];
    usbkbd_dev_t      dev[USBKBD_MAX_DEV];
    uint8_t           report[8];     /* the last boot report */
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

#endif /* LUGALOS_DRIVERS_USBKBD_TASK_H */
