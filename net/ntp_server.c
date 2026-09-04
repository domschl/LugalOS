/* See net/include/net/ntp_server.h for what this serves and why it would
 * rather say "unsynchronised" than guess. */

#include "net/ntp_server.h"
#include "net/ip.h"
#include "kernel/time.h"
#include "kernel/discipline.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#include <stddef.h>

#if CONFIG_ENABLE_DCF77
#include "drivers/dcf77_service.h"
#endif

#define NTP_PKT_LEN       48
#define NTP_EPOCH_OFFSET  2208988800LL

/* Our clock's resolution as a signed power of two seconds, which is what the
 * precision field is. The wall clock keeps microseconds, and 2^-20 is 0.95 us
 * -- the nearest honest answer. Claiming finer would be claiming the counter
 * resolves something it does not. */
#define NTP_PRECISION  (-20)

/* Beyond this much accumulated uncertainty, holdover stops being a degraded
 * answer and becomes a wrong one, so the reply says so.
 *
 * A second: past that, a client is better served by ignoring us entirely and
 * asking someone else than by weighting a number this soft. It is deliberately
 * expressed as dispersion rather than as an age -- age alone would punish a
 * board whose rate is well learned exactly as hard as one whose is not, and
 * the difference between those two is the entire argument for P5. */
#define UNSYNC_DISPERSION_US 1000000u

static bool     g_running;
static uint32_t g_requests, g_malformed, g_unsync;
static uint8_t  g_last_stratum;
static uint32_t g_last_disp;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Microseconds since 1970 into NTP's 32.32 fixed point since 1900. */
static void us_to_ntp(int64_t us, uint8_t *p) {
    int64_t sec = us / 1000000LL, rem = us % 1000000LL;
    if (rem < 0) { rem += 1000000LL; sec -= 1; }
    put32(p, (uint32_t)(sec + NTP_EPOCH_OFFSET));
    put32(p + 4, (uint32_t)(((uint64_t)rem << 32) / 1000000ULL));
}

/* Microseconds into NTP's 16.16 "short" format, used by the delay and
 * dispersion fields. Saturates rather than wrapping: a dispersion so large it
 * overflows is still, emphatically, a large dispersion, and wrapping it to a
 * small one would turn the most alarming case into the most reassuring. */
static void us_to_short(uint32_t us, uint8_t *p) {
    uint64_t v = ((uint64_t)us << 16) / 1000000ULL;
    if (v > 0xFFFFFFFFULL) v = 0xFFFFFFFFULL;
    put32(p, (uint32_t)v);
}

static void ntp_server_cb(void *ctx, const uint8_t src_ip[IPV4_LEN],
                          uint16_t src_port, const uint8_t *data, uint32_t len) {
    (void)ctx;

    /* T2 first, before any validation. "When the request arrived" is a fact
     * about a packet that arrived, whatever the packet later turns out to
     * contain -- and validating first would fold the validation itself into
     * the timestamp. */
    int64_t t2 = time_epoch_us();

    if (len < NTP_PKT_LEN) { g_malformed++; return; }

    uint8_t li_vn_mode = data[0];
    uint8_t vn   = (uint8_t)((li_vn_mode >> 3) & 0x7u);
    uint8_t mode = (uint8_t)(li_vn_mode & 0x7u);
    /* Mode 3 is a client asking. Mode 1 (symmetric active) and anything else
     * is a peering relationship this does not offer, and answering it would
     * claim one. Versions 3 and 4 share this header layout exactly. */
    if (mode != 3u || vn < 3u || vn > 4u) { g_malformed++; return; }

    disc_status_t d;
    discipline_status(&d);

    bool synced = (d.state == DISC_TRACK || d.state == DISC_HOLDOVER)
                  && d.dispersion_us < UNSYNC_DISPERSION_US
                  && time_is_set();

    uint8_t reply[NTP_PKT_LEN];
    for (uint32_t i = 0; i < NTP_PKT_LEN; i++) reply[i] = 0;

    uint8_t li = 0;
#if CONFIG_ENABLE_DCF77
    /* The leap warning arrives on the air, in the frame itself, and is passed
     * straight through -- the one respect in which being DCF-disciplined beats
     * being GPS-fed, since nothing here has to be told about it out of band. */
    if (synced) {
        dcf_status_t ds;
        dcf77_service_status(&ds);
        if (ds.decoder.leap_announced) li = 1u;   /* last minute has 61 s */
    }
#endif
    if (!synced) li = 3u;                          /* alarm: unsynchronised */

    reply[0] = (uint8_t)((li << 6) | (vn << 3) | 4u);   /* mode 4 = server */
    reply[1] = synced ? 1u : 16u;                       /* stratum */
    reply[2] = data[2];                                 /* echo the poll */
    reply[3] = (uint8_t)(int8_t)NTP_PRECISION;

    /* Root delay is zero: a primary reference has no upstream server to have
     * been delayed by. The radio's own propagation is not root delay -- it is
     * calibrated out (CONFIG_DCF77_DELAY_US) and what remains of it is in the
     * dispersion, where uncertainty belongs. */
    us_to_short(0, &reply[4]);
    us_to_short(synced ? d.dispersion_us : UNSYNC_DISPERSION_US, &reply[8]);

    /* Reference id. Four ASCII characters for a primary reference; RFC 5905
     * registers "DCF" for Mainflingen. Unsynchronised leaves it zero rather
     * than naming a source we are not currently following. */
    if (synced) { reply[12] = 'D'; reply[13] = 'C'; reply[14] = 'F'; reply[15] = 0; }

    /* Reference timestamp: when the clock was last actually corrected, not
     * now. A client uses it to see how stale we are, so writing `now` here
     * would erase precisely the information holdover needs to convey. */
    int64_t ref_us = t2 - (int64_t)d.age_s * 1000000LL;
    us_to_ntp(synced ? ref_us : 0, &reply[16]);

    for (uint32_t i = 0; i < 8u; i++) reply[24 + i] = data[40 + i];  /* origin */
    us_to_ntp(t2, &reply[32]);                                       /* receive */

    g_requests++;
    if (!synced) g_unsync++;
    g_last_stratum = reply[1];
    g_last_disp = d.dispersion_us;

    /* T3 last, immediately before handing the packet to the stack, so the
     * transmit timestamp describes this packet's departure rather than the
     * moment we started building it. Everything above is a handful of
     * microseconds, but they are microseconds a client would otherwise fold
     * into its offset as if they were network delay. */
    us_to_ntp(time_epoch_us(), &reply[40]);
    udp_send(src_ip, src_port, 123, reply, NTP_PKT_LEN);
}

int ntp_server_start(void) {
    if (g_running) return 0;
    if (udp_bind(123, ntp_server_cb, NULL) != 0) return -1;
    g_running = true;
    printk("[NTP] serving on UDP 123 (stratum 1 while disciplined, "
           "LI 3 / stratum 16 otherwise)\n");
    return 0;
}

void ntp_server_status(ntp_server_stats_t *out) {
    if (!out) return;
    out->running = g_running;
    out->requests = g_requests;
    out->malformed = g_malformed;
    out->unsync_replies = g_unsync;
    out->last_stratum = g_last_stratum;
    out->last_disp_us = g_last_disp;
}
