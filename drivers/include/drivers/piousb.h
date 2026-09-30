#ifndef LUGALOS_DRIVERS_PIOUSB_H
#define LUGALOS_DRIVERS_PIOUSB_H

#include <stdbool.h>
#include <stdint.h>

/* The PIO-USB host engine -- 36.7, plan/phase36_rp2350_lcd7_terminal.md §3.2.
 *
 * Core 1 runs the bus: SOF every millisecond, bus reset, and one transaction
 * at a time. It knows nothing about descriptors, hubs or HID. Core 0 hands it
 * transactions through a request ring and collects the results from a
 * completion ring; both are single-producer, single-consumer and lock-free,
 * because the two sides share no lock (core 1 is not in the scheduler).
 *
 * A transaction is one USB exchange -- token, data, handshake:
 *   SETUP  8 bytes DATA0 to (addr, 0)            -> ACK, or an error
 *   IN     from (addr, ep), DATA0/1 expected     -> DATA + bytes, NAK or STALL
 *   OUT    `len` bytes DATAx to (addr, ep)       -> ACK, NAK or STALL
 *   RESET  SE0 on the port for 20 ms, then idle  -> the device's speed
 * A NAK completes the transaction: retrying is the caller's decision, the
 * one it needs for interrupt polling anyway. */

#define PIOUSB_MAX_PACKET 64u

enum {
    PIOUSB_OP_SETUP = 1,
    PIOUSB_OP_IN    = 2,
    PIOUSB_OP_OUT   = 3,
    PIOUSB_OP_RESET = 4,
    /* Diagnostic: send a SETUP token with the decoder running, so it decodes
     * our own packet off the wire; data[] returns what it read. TX and RX
     * proven together without a device's cooperation. */
    PIOUSB_OP_LOOPBACK = 5,
};

enum {
    PIOUSB_OK        = 0,   /* ACK (SETUP/OUT), valid DATA (IN), reset done */
    PIOUSB_NAK       = 1,
    PIOUSB_STALL     = 2,
    PIOUSB_TIMEOUT   = 3,   /* no answer within the bus turnaround */
    PIOUSB_CRC       = 4,   /* a DATA packet with a bad CRC (not ACKed) */
    PIOUSB_PROTOCOL  = 5,   /* an answer that is no valid PID here */
    PIOUSB_NODEV     = 6,   /* no device, or the port is not enabled */
    PIOUSB_TOGGLE    = 7,   /* IN: DATA0/1 other than expected (ACKed, dropped) */
    PIOUSB_ENGINE    = 8,   /* core 0 side: the engine is not running/answering */
    PIOUSB_BUSY      = 9,   /* core 0 side: the kbd task owns the port */
};

typedef struct {
    uint8_t  op;            /* PIOUSB_OP_* */
    uint8_t  addr;
    uint8_t  ep;            /* endpoint number, without the direction bit */
    uint8_t  data1;         /* OUT: send DATA1; IN: expect DATA1 */
    uint16_t len;           /* OUT/SETUP: bytes in data[]; IN: ignored */
    uint8_t  status;        /* completion: PIOUSB_OK, ... */
    uint8_t  pid;           /* completion: the PID the device answered with */
    uint16_t rx_len;        /* completion: IN payload bytes in data[] */
    uint8_t  speed;         /* completion of RESET: 1 full speed, 0 low speed */
    uint8_t  low_speed;     /* 36.8: a low-speed device behind a hub: PRE, 1.5 Mbit/s */
    uint8_t  data[PIOUSB_MAX_PACKET];
} piousb_xfer_t;

typedef struct {
    bool     running;       /* core 1 is in the engine loop */
    bool     connected;     /* something is on the port (J or K idle) */
    bool     enabled;       /* reset done, SOFs flowing */
    bool     full_speed;
    uint32_t sof_count;
    uint32_t frame;         /* 11-bit frame number of the last SOF */
    uint32_t attaches, detaches, resets;
    uint32_t xfers, acks, naks, stalls, timeouts, crc_errors, pid_errors, toggles;
    uint32_t turnaround_max_cycles;  /* last DATA byte seen -> our ACK released */
    uint32_t late_frames;            /* SOFs sent >50 us after their slot */
    uint32_t rx_eop_misses;          /* our own EOP never flagged by the RX side */
    uint32_t rx_eop_wait_max_cycles; /* longest wait for that flag */
    uint32_t dbg_rx_flags;           /* RX IRQ flags after the last reply wait */
    uint32_t dbg_rx_bytes;           /* bytes that reply wait read */
    uint32_t dbg_reset_line;         /* line state sampled during the last reset */
    uint32_t loops;                  /* engine loop iterations (liveness) */
    uint32_t core1_stack_used;
} piousb_status_t;

/* The block core 1 shares with its one submitter (36.8): the transaction ring
 * and the engine's status, 512 bytes on a 512-byte boundary so that the U-mode
 * `kbd` task's domain can grant exactly it and nothing else.
 *
 * One ring whose slots complete in place: the submitter fills
 * slot[submitted % PIOUSB_RING] and advances `submitted`; core 1 runs
 * slot[completed % PIOUSB_RING], writes the result into the same slot and
 * advances `completed`. Each index has one writer, so there is no lock -- and
 * there is exactly one submitter at a time, named by `owner`. */
#define PIOUSB_RING        4u
#define PIOUSB_OWNER_KERNEL 0u      /* usbprobe, from the shell */
#define PIOUSB_OWNER_KBD    1u      /* the kbd task (drivers/usbkbd_rp2350.c) */
typedef struct {
    volatile uint32_t submitted;
    volatile uint32_t completed;
    volatile uint32_t owner;
    uint32_t          _pad;
    piousb_xfer_t     slot[PIOUSB_RING];
    piousb_status_t   st;           /* written by core 1 */
} piousb_shared_t;

/* The shared block and its extent, for a domain grant. NULL/0 where there is
 * no PIO-USB port. */
piousb_shared_t *piousb_shared(void);
void piousb_shared_region(uintptr_t *base, uintptr_t *size);

/* Boot: set up PIO0 (TX) and PIO1 (RX) on the port's pins and launch the
 * engine on core 1. 0 on success. */
int piousb_init(void);

/* One transaction, synchronously: submits `x`, yields until it completes (or
 * `timeout_us` passes: PIOUSB_ENGINE), and copies the result back into `x`.
 * Returns x->status. One caller at a time (a lock serialises them). */
int piousb_xfer(piousb_xfer_t *x, uint32_t timeout_us);

void piousb_status(piousb_status_t *st);

/* `usbprobe`: reset the port, read the device descriptor at address 0,
 * print it. `/proc/usbhost`: the counters above as key=value lines. */
void piousb_probe(void);
void piousb_loopback(void);
int piousb_proc_render(char *buf, uint32_t cap);

#endif /* LUGALOS_DRIVERS_PIOUSB_H */
