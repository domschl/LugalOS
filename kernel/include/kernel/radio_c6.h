#ifndef LUGALOS_KERNEL_RADIO_C6_H
#define LUGALOS_KERNEL_RADIO_C6_H

/* Starts the ESP32-C6 Wi-Fi radio (with an SSID and a 64-hex derived PSK: also associate; both NULL: scan only) in its U-mode domain and reports how far it
 * got on the console (45.6). Returns 0 when esp_wifi_init_internal succeeded. */
int radio_c6_start(const char *ssid, const char *psk);
/* Trace every kernel-object call the radio makes (kernel/kobj_sys.c). */
void radio_c6_trace(int on);
void radio_c6_stats(void);
void radio_c6_probe(void);
/* The wlan0 interface, once the radio has joined a network (NULL before). */
struct netif;
struct netif *radio_c6_netif(void);

#endif
