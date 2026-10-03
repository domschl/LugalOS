/*
 * The IP stack's receive path (net/arp.c, ipv4.c, icmp.c, udp.c, tcp.c) on
 * the host, under ASan/UBSan or valgrind (phase 40 review).
 *
 * Every frame on the wire reaches ip_input() or arp_input() -- from a gateway
 * on Ethernet, or anyone on the same WLAN -- before anything has decided it is
 * trustworthy. This plays a peer: ARP, ping, UDP to a bound and an unbound
 * port, and a TCP connection to the 9P listener carrying data and closing,
 * with valid checksums so the frames get past the first checks. Then those
 * frames with bytes changed -- checksums recomputed half the time, so the
 * mutation reaches the protocol code rather than dying at the checksum --
 * cut short, or replaced by random bytes, with time moving so the timers
 * run. A frame may be dropped; nothing may touch memory it does not own.
 *
 * net/stack.c (the poll loop, configuration, the netif registry) is stood in
 * for below: it is plumbing, and the parsers are what take input.
 *
 * Usage: net_host [iterations [seed]].
 */

#include "net/ip.h"
#include "net/net_internal.h"
#include "net/tcp.h"
#include "fs/p9_link.h"
#include "shim.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- the stand-in for net/stack.c --- */

static net_state_t g_net;
static netif_t g_nif = { .name = "host0", .mac = { 0x02, 0, 0, 0, 0, 1 } };
static uint8_t g_tx[NETIF_FRAME_MAX];
static uint32_t g_tx_count;
static uint8_t g_last_tx[NETIF_FRAME_MAX];
static uint32_t g_last_tx_len;

net_state_t *net_mutable_state(void) { return &g_net; }
const net_state_t *net_state(void) { return &g_net; }
bool net_configured(void) { return g_net.configured; }
const uint8_t *net_broadcast_mac(void) {
    static const uint8_t b[NETIF_MAC_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return b;
}
uint8_t *net_tx_payload(void) { return g_tx + ETH_HDR_LEN; }
uint32_t net_tx_payload_max(void) { return NETIF_FRAME_MAX - ETH_HDR_LEN; }
int net_tx_send(const uint8_t dst_mac[NETIF_MAC_LEN], uint16_t ethertype, uint32_t payload_len) {
    if (payload_len > net_tx_payload_max()) { fprintf(stderr, "FAIL: tx of %u bytes\n", payload_len); abort(); }
    memcpy(g_tx, dst_mac, NETIF_MAC_LEN);
    memcpy(g_tx + 6, g_nif.mac, NETIF_MAC_LEN);
    g_tx[12] = (uint8_t)(ethertype >> 8);
    g_tx[13] = (uint8_t)ethertype;
    g_last_tx_len = ETH_HDR_LEN + payload_len;
    memcpy(g_last_tx, g_tx, g_last_tx_len);
    g_tx_count++;
    return 0;
}
void netif_mac_str(const uint8_t mac[NETIF_MAC_LEN], char *out) {
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Time the harness moves, so ARP ageing and TCP's timers run. */
static uint64_t g_now_ms = 1000;
uint64_t time_get_ms(void) { return g_now_ms; }

/* An accepted connection is a 9P link: echo whatever frames arrive, so the
 * TCP receive and send paths both carry data. */
int p9_link_service(p9_link_t *link) {
    static uint8_t buf[4096];
    int n = 0;
    while (link->poll && link->poll(link) > 0 && n++ < 8) {
        int r = link->recv_frame(link, buf, sizeof(buf));
        if (r <= 0) break;
        (void)link->send_frame(link, buf, (uint32_t)(r > 64 ? 64 : r));
    }
    return 0;
}
void p9_link_unregister_background(p9_link_t *link) { (void)link; }

static uint32_t g_udp_rx;
static void udp_rx(void *ctx, const uint8_t src[IPV4_LEN], uint16_t sport, const uint8_t *d, uint32_t len) {
    (void)ctx; (void)src; (void)sport;
    volatile uint8_t sink = 0;
    for (uint32_t i = 0; i < len; i++) sink ^= d[i];   /* touch every byte it claims */
    (void)sink;
    g_udp_rx++;
}

/* --- building the peer's frames --- */

static const uint8_t ME[4] = { 10, 0, 2, 15 }, PEER[4] = { 10, 0, 2, 2 };
static const uint8_t PEER_MAC[6] = { 0x02, 0, 0, 0, 0, 2 };

static uint16_t csum(const uint8_t *p, uint32_t len, uint32_t sum) {
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Recomputes every checksum a frame carries, after a mutation. */
static void fix_checksums(uint8_t *f, uint32_t len) {
    if (len < 34 || f[12] != 0x08 || f[13] != 0x00) return;
    uint8_t *ip = f + 14;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4u;
    if (ihl < 20 || 14 + ihl > len) return;
    ip[10] = ip[11] = 0;
    uint16_t c = csum(ip, ihl, 0);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    uint32_t total = (uint32_t)(ip[2] << 8 | ip[3]);
    if (total < ihl || 14 + total > len) return;
    uint8_t *l4 = ip + ihl;
    uint32_t l4len = total - ihl;
    uint32_t pseudo = (uint32_t)(ip[12] << 8 | ip[13]) + (uint32_t)(ip[14] << 8 | ip[15]) +
                      (uint32_t)(ip[16] << 8 | ip[17]) + (uint32_t)(ip[18] << 8 | ip[19]) +
                      ip[9] + l4len;
    if (ip[9] == IP_PROTO_TCP && l4len >= 20) {
        l4[16] = l4[17] = 0; c = csum(l4, l4len, pseudo); l4[16] = (uint8_t)(c >> 8); l4[17] = (uint8_t)c;
    } else if (ip[9] == IP_PROTO_UDP && l4len >= 8) {
        l4[6] = l4[7] = 0; c = csum(l4, l4len, pseudo); l4[6] = (uint8_t)(c >> 8); l4[7] = (uint8_t)c;
    } else if (ip[9] == IP_PROTO_ICMP && l4len >= 4) {
        l4[2] = l4[3] = 0; c = csum(l4, l4len, 0); l4[2] = (uint8_t)(c >> 8); l4[3] = (uint8_t)c;
    }
}

static uint32_t eth(uint8_t *f, uint16_t type) {
    memcpy(f, g_nif.mac, 6); memcpy(f + 6, PEER_MAC, 6);
    f[12] = (uint8_t)(type >> 8); f[13] = (uint8_t)type;
    return 14;
}

static uint32_t ipv4(uint8_t *f, uint8_t proto, uint32_t l4len) {
    uint32_t o = eth(f, ETHERTYPE_IPV4);
    uint8_t *ip = f + o;
    memset(ip, 0, 20);
    ip[0] = 0x45; ip[2] = (uint8_t)((20 + l4len) >> 8); ip[3] = (uint8_t)(20 + l4len);
    ip[8] = 64; ip[9] = proto;
    memcpy(ip + 12, PEER, 4); memcpy(ip + 16, ME, 4);
    return o + 20;
}

static uint32_t arp_req(uint8_t *f) {
    uint32_t o = eth(f, ETHERTYPE_ARP);
    memset(f, 0xff, 6);
    uint8_t *a = f + o;
    a[0] = 0; a[1] = 1; a[2] = 8; a[3] = 0; a[4] = 6; a[5] = 4; a[6] = 0; a[7] = 1;
    memcpy(a + 8, PEER_MAC, 6); memcpy(a + 14, PEER, 4); memset(a + 18, 0, 6); memcpy(a + 24, ME, 4);
    return o + 28;
}

static uint32_t ping(uint8_t *f, uint32_t datalen) {
    uint32_t o = ipv4(f, IP_PROTO_ICMP, 8 + datalen);
    uint8_t *ic = f + o;
    memset(ic, 0, 8 + datalen);
    ic[0] = 8; ic[5] = 1; ic[7] = 1;
    for (uint32_t i = 0; i < datalen; i++) ic[8 + i] = (uint8_t)i;
    fix_checksums(f, o + 8 + datalen);
    return o + 8 + datalen;
}

static uint32_t udp(uint8_t *f, uint16_t dport, uint32_t datalen) {
    uint32_t o = ipv4(f, IP_PROTO_UDP, 8 + datalen);
    uint8_t *u = f + o;
    u[0] = 0x30; u[1] = 0x39; u[2] = (uint8_t)(dport >> 8); u[3] = (uint8_t)dport;
    u[4] = (uint8_t)((8 + datalen) >> 8); u[5] = (uint8_t)(8 + datalen);
    for (uint32_t i = 0; i < datalen; i++) u[8 + i] = (uint8_t)('A' + i % 26);
    fix_checksums(f, o + 8 + datalen);
    return o + 8 + datalen;
}

#define TCP_PORT 564
static uint16_t g_sport = 0xC001;
static uint32_t tcp(uint8_t *f, uint8_t flags, uint32_t seq, uint32_t ack, uint32_t datalen) {
    uint32_t o = ipv4(f, IP_PROTO_TCP, 20 + datalen);
    uint8_t *t = f + o;
    memset(t, 0, 20);
    t[0] = (uint8_t)(g_sport >> 8); t[1] = (uint8_t)g_sport; t[2] = TCP_PORT >> 8; t[3] = TCP_PORT & 0xff;
    t[4] = (uint8_t)(seq >> 24); t[5] = (uint8_t)(seq >> 16); t[6] = (uint8_t)(seq >> 8); t[7] = (uint8_t)seq;
    t[8] = (uint8_t)(ack >> 24); t[9] = (uint8_t)(ack >> 16); t[10] = (uint8_t)(ack >> 8); t[11] = (uint8_t)ack;
    t[12] = 0x50; t[13] = flags; t[14] = 0x20; t[15] = 0x00;
    /* 9P frames: a 4-byte little-endian size, then the rest. */
    for (uint32_t i = 0; i < datalen; i++) t[20 + i] = (uint8_t)(i % 7 == 0 ? 7 : i);
    if (datalen >= 4) { t[20] = 7; t[21] = t[22] = t[23] = 0; }
    fix_checksums(f, o + 20 + datalen);
    return o + 20 + datalen;
}

static uint32_t seq_of(const uint8_t *f) { const uint8_t *t = f + 34; return (uint32_t)t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7]; }

/* --- running --- */

#define MAXF 24
static uint8_t g_f[MAXF][NETIF_FRAME_MAX];
static uint32_t g_flen[MAXF];
static int g_nf;

static void deliver(const uint8_t *f, uint32_t len) {
    if (len >= 14 && f[12] == 0x08 && f[13] == 0x06) arp_input(f, len);
    else ip_input(f, len);
    g_now_ms += 37;
    tcp_service();
}

static void reset_stack(void) {
    memset(&g_net, 0, sizeof(g_net));
    g_net.nif = &g_nif;
    g_net.configured = true;
    memcpy(g_net.ip, ME, 4);
    g_net.mask[0] = g_net.mask[1] = g_net.mask[2] = 255;
    memcpy(g_net.gw, PEER, 4);
}

/* A real exchange, built frame by frame from the stack's own replies, so
 * the TCP sequence numbers are the ones it chose. */
static void build_session(void) {
    g_nf = 0;
    g_flen[g_nf] = arp_req(g_f[g_nf]); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = ping(g_f[g_nf], 56); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = ping(g_f[g_nf], 1400); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = udp(g_f[g_nf], 7777, 100); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = udp(g_f[g_nf], 9, 10); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;   /* no one bound */

    uint32_t iss = 1000;
    g_flen[g_nf] = tcp(g_f[g_nf], 0x02, iss, 0, 0); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;   /* SYN */
    uint32_t their = (g_last_tx_len >= 54) ? seq_of(g_last_tx) : 0;
    g_flen[g_nf] = tcp(g_f[g_nf], 0x10, iss + 1, their + 1, 0); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = tcp(g_f[g_nf], 0x18, iss + 1, their + 1, 7); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = tcp(g_f[g_nf], 0x18, iss + 8, their + 1, 1000); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;
    g_flen[g_nf] = tcp(g_f[g_nf], 0x18, iss + 8, their + 1, 1000); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++; /* retransmit */
    g_flen[g_nf] = tcp(g_f[g_nf], 0x18, iss + 5000, their + 1, 20); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++; /* out of window */
    g_flen[g_nf] = tcp(g_f[g_nf], 0x11, iss + 1008, their + 1, 0); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++; /* FIN */
    g_flen[g_nf] = tcp(g_f[g_nf], 0x04, iss + 1009, 0, 0); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;          /* RST */
    g_flen[g_nf] = tcp(g_f[g_nf], 0x02, iss + 7, 0, 0); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;            /* SYN again */
    g_flen[g_nf] = tcp(g_f[g_nf], 0x12, 5, 6, 0); deliver(g_f[g_nf], g_flen[g_nf]); g_nf++;                  /* stray SYN-ACK */
}

static volatile sig_atomic_t g_iter = -1;
static uint64_t g_seed0;
static void on_alarm(int sig) {
    (void)sig;
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "FAIL: stack did not return, input %d (seed %llu)\n",
                     (int)g_iter, (unsigned long long)g_seed0);
    (void)!write(2, msg, (size_t)n);
    _exit(3);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    signal(SIGALRM, on_alarm);
    g_seed0 = seed;

    reset_stack();
    if (udp_bind(7777, udp_rx, NULL) != 0 || tcp_listen(TCP_PORT) != 0) {
        fprintf(stderr, "FAIL: could not bind\n");
        return 1;
    }
    uint32_t tx0 = g_tx_count;
    build_session();
    uint32_t replies = g_tx_count - tx0;
    printf("net_host: %d-frame session, %u replies, %u UDP datagrams, %u TCP accepted, "
           "drops short/csum/port %u/%u/%u\n", g_nf, replies, g_udp_rx, tcp_accepted_total(),
           g_net.drop_short, g_net.drop_checksum, g_net.drop_no_port);
    if (replies < 4 || g_udp_rx != 1 || tcp_accepted_total() < 1) {
        fprintf(stderr, "FAIL: the session did not reach the stack (replies %u, udp %u, tcp %u)\n",
                replies, g_udp_rx, tcp_accepted_total());
        return 1;
    }

    static uint8_t f[NETIF_FRAME_MAX];
    uint64_t s = seed | 1;
    for (int it = 0; it < iterations; it++) {
        g_iter = it;
        alarm(5);
        if (it & 1) {
            /* Established: a fresh connection, then segments with the right
             * sequence numbers and a mutated header or payload, then a reset. */
            g_sport = (uint16_t)(1024 + it % 60000);
            uint32_t iss = host_rand(&s);
            uint32_t len = tcp(f, 0x02, iss, 0, 0);
            deliver(f, len);
            uint32_t their = (g_last_tx_len >= 54) ? seq_of(g_last_tx) : 0;
            len = tcp(f, 0x10, iss + 1, their + 1, 0);
            deliver(f, len);
            uint32_t sent = 0;
            for (int k = 0, n = 1 + (int)(host_rand(&s) % 5u); k < n; k++) {
                uint32_t dl = host_rand(&s) % 1200u;
                len = tcp(f, (uint8_t)(0x18 | (host_rand(&s) % 4u == 0 ? (host_rand(&s) & 0x3f) : 0)),
                          iss + 1 + sent, their + 1, dl);
                for (int m = 0, nm = (int)(host_rand(&s) % 4u); m < nm; m++)
                    f[34 + host_rand(&s) % (len - 34)] = (uint8_t)host_rand(&s);
                fix_checksums(f, len);
                deliver(f, len);
                sent += dl;
            }
            len = tcp(f, 0x04, iss + 1 + sent, 0, 0);
            deliver(f, len);
            g_now_ms += 3000;   /* past TIME_WAIT: the two slots come back */
            tcp_service();
            alarm(0);
            continue;
        }
        int burst = 1 + (int)(host_rand(&s) % 6u);
        for (int b = 0; b < burst; b++) {
            int k = (int)(host_rand(&s) % (uint32_t)g_nf);
            uint32_t len = g_flen[k];
            memcpy(f, g_f[k], len);
            uint32_t r = host_rand(&s);
            int nm = 1 + (int)(r % 6u);
            for (int m = 0; m < nm; m++) {
                uint32_t pick = host_rand(&s);
                switch (pick % 6u) {
                    case 0: if (len) f[host_rand(&s) % len] = (uint8_t)host_rand(&s); break;
                    case 1: f[14 + host_rand(&s) % 40u % (len > 14 ? len - 14 : 1)] = (uint8_t)host_rand(&s); break; /* headers */
                    case 2: len = host_rand(&s) % (len + 1); break;                                   /* cut short */
                    case 3: if (len >= 18) { f[16] = (uint8_t)host_rand(&s); f[17] = (uint8_t)host_rand(&s); } break; /* IP total length */
                    case 4: if (len >= 48) { f[46] = (uint8_t)host_rand(&s); } break;                /* TCP data offset */
                    default: len = 14 + host_rand(&s) % (NETIF_FRAME_MAX - 14);                      /* random */
                             for (uint32_t i = 14; i < len; i++) f[i] = (uint8_t)host_rand(&s);
                             f[12] = 0x08; f[13] = (host_rand(&s) & 1) ? 0x00 : 0x06;
                             if (f[13] == 0x00 && len >= 34) { f[14] = 0x45; memcpy(f + 30, ME, 4); }
                             break;
                }
            }
            if (r & 0x100) fix_checksums(f, len);
            deliver(f, len);
        }
        g_now_ms += host_rand(&s) % 5000u;   /* timers: retransmission, TIME_WAIT, ARP age */
        tcp_service();
        alarm(0);
    }
    printf("net_host: %d mutated bursts (seed %llu), %u TCP accepted in all, no fault\n",
           iterations, (unsigned long long)seed, tcp_accepted_total());
    return 0;
}
