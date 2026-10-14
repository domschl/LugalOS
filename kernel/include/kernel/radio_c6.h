#ifndef LUGALOS_KERNEL_RADIO_C6_H
#define LUGALOS_KERNEL_RADIO_C6_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the ESP32-C6 Wi-Fi radio (with an SSID and a 64-hex derived PSK: also associate; both NULL: scan only) in its U-mode domain and reports how far it
 * got on the console (45.6). Returns 0 when esp_wifi_init_internal succeeded. */
int radio_c6_start(const char *ssid, const char *psk);
/* Trace every kernel-object call the radio makes (kernel/kobj_sys.c). */
void radio_c6_trace(int on);
void radio_c6_stats(void);
void radio_c6_probe(void);
/* True while the radio's RF is up (esp_wifi_start done): the C6's RNG is a real entropy source only then
 * (kernel/random.c). False before the radio starts, after it failed, and on a build without the radio. */
bool radio_c6_rf_on(void);
/* True once the radio has been started in this boot (it can only start once). */
bool radio_c6_started(void);
/* `radio rejoin`: drop the association and join again. */
void radio_c6_rejoin(void);
/* At boot (kernel/main.c): a task that starts the radio and joins when the identity record holds WLAN
 * credentials, then supervises the link for the life of the board. */
void radio_c6_autostart(void);
/* The same, with credentials from elsewhere (an LLP host's identity record): an SSID and a 64-hex
 * derived PSK, held in RAM only until the radio has them. Returns at once; 0, or -1 when the radio
 * has already started (or is starting) in this boot, or the arguments are malformed. */
int radio_c6_join(const char *ssid, const char *psk_hex);
/* /dev/wlan, a ctl file: `join SSID PSKHEX`, `rejoin` or `scan` written (the count, or -1); reading
 * gives the station's state as key=value lines (state, ssid, bssid, channel, rssi, mac, joins,
 * last_disconnect). The same names wherever a node has a Wi-Fi station. */
int radio_c6_ctl_write(const char *buf, uint32_t len);
uint32_t radio_c6_ctl_read(char *out, uint32_t cap);
/* /proc/wlanscan: the last scan, one access point per line ("bssid channel rssi auth ssid"),
 * after a line `aps=N`. As many lines as fit in `cap`; whole lines only. */
uint32_t radio_c6_scan_render(char *out, uint32_t cap);
/* /proc/radio: this radio's own state -- how far it started, its return codes, frame counters
 * and rings -- as key=value lines. */
uint32_t radio_c6_proc_render(char *out, uint32_t cap);
/* The associated AP's signal in dBm, or -127 when not associated (for LLP's STATE). */
int radio_c6_rssi(void);
/* The wlan0 interface, once the radio has joined a network (NULL before). */
struct netif;
struct netif *radio_c6_netif(void);

#endif
