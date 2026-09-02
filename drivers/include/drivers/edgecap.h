/*
 * Timestamped GPIO edge capture (P3,
 * plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * An interrupt on a pin, a TIMER0 read, and a bounded ring. That is the whole
 * of it, and it is a shared module rather than a part of the DCF-77 driver
 * for one reason: **RP2350 has a single interrupt for all of GPIO bank 0**
 * (IO_IRQ_BANK0, 21). Two drivers cannot each attach their own handler to it,
 * so the dispatch has to live in one place and hand edges to whoever
 * registered the pin. The DCF-77 receiver's OUT line and a GPS module's PPS
 * are the two users, and they want exactly the same thing.
 *
 * **Why capture at all**, when the DCF decoder already timestamps its own
 * transitions: because it timestamps them at whatever moment the application
 * loop happened to sample the pin, which is milliseconds of jitter that
 * varies with what the display and the radio are doing. An interrupt reading
 * TIMER0 is within a microsecond or two of the edge regardless. §3.2 held
 * this back for a long time on the grounds that the receiver's own jitter is
 * milliseconds and the precision had nowhere to go -- true, until §3.4 gave
 * it somewhere: a PPS edge is good to tens of nanoseconds, and comparing a
 * DCF mark against it is only worth doing if both are captured the same way.
 *
 * **The caller owns the storage and the pad.** A ring is passed in, not
 * allocated here, so each user sizes its own and nothing lands in .bss that a
 * board without a receiver would still pay for. Pin direction, pulls and
 * function are the driver's business too -- this touches only the interrupt
 * enable, so it cannot fight the driver that owns the pin.
 */

#ifndef DRIVERS_EDGECAP_H
#define DRIVERS_EDGECAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t t_us;    /* TIMER0 at the interrupt, not at the read */
    uint8_t  level;   /* the pin as it was sampled in the handler */
} edge_t;

typedef struct {
    edge_t  *buf;
    uint16_t cap;
    volatile uint16_t head;   /* written by the handler */
    volatile uint16_t tail;   /* written by the consumer */
    volatile uint32_t dropped;
    volatile uint32_t total;
    uint8_t  gpio;
    bool     active;
} edgecap_t;

/* Registers `gpio` and starts capturing both edges into `storage`.
 *
 * `cap` is in entries and must be at least 2 (one slot always stays empty, so
 * a ring of n holds n-1). Returns 0, or -1 if the registration table is full
 * or the arguments are unusable. Idempotent per pin: attaching one twice is
 * refused rather than silently doubling its handler. */
int edgecap_attach(edgecap_t *e, uint8_t gpio, edge_t *storage, uint16_t cap);

/* Takes the oldest unread edge. False when there is none. Single consumer --
 * two tasks popping the same ring would race on `tail`, and nothing here
 * stops them, because nothing here should need to. */
bool edgecap_pop(edgecap_t *e, edge_t *out);

/* How many edges were discarded because the ring was full, and how many have
 * ever arrived. A non-zero drop count is the honest signal that a consumer is
 * too slow or a line is noisier than its ring was sized for; it is deliberately
 * not hidden behind a return value nobody checks. */
uint32_t edgecap_dropped(const edgecap_t *e);
uint32_t edgecap_total(const edgecap_t *e);

/* Appends an edge. The interrupt handler's own path, exposed so a selftest can
 * drive the ring without a pin -- the ring is where the off-by-one lives, and
 * it should not need hardware to falsify. */
void edgecap_push(edgecap_t *e, uint64_t t_us, bool level);

/* `edgecapselftest`: the ring's own checks, on every target. */
void edgecap_selftest(void);

#endif /* DRIVERS_EDGECAP_H */
