/* What IDF's wpa_supplicant expects of its OS port (utils/os.h, port/os_xtensa.c), as the
 * radio domain's own (45.7, plan/phase45_esp32c6.md). The supplicant is compiled into the
 * radio's text; these are the few services it reaches for, served from the shim:
 * time and randomness through the kernel by ecall, nothing else. */

#include "radio_redirect.h"
#include "osi_impl.h"
#include <stdint.h>
#include <stddef.h>

struct os_time { long sec; long usec; };

int os_get_time(struct os_time *t) {
    int64_t us = radio_osi_esp_timer_get_time();
    t->sec = (long)(us / 1000000);
    t->usec = (long)(us % 1000000);
    return 0;
}

void os_sleep(long sec, long usec) { radio_osi_task_delay((uint32_t)(sec * 1000 + usec / 1000)); }

int os_get_random(unsigned char *buf, size_t len) { return radio_osi_get_random(buf, len); }

/* The supplicant only compares times it took from os_get_time; calendar time is not
 * used on a station. */
long mktime(void *tm) { (void)tm; return 0; }
void *gmtime(const long *t) { (void)t; return 0; }

/* WPS is not built. */
void *wps_get_wps_sm_cb(void) { return 0; }

/* The authenticator side (SoftAP) is not built; esp_wpa_main.c's wpa_ap_rx_eapol still names it. */
void wpa_receive(void *auth, void *sm, uint8_t *data, size_t len) { (void)auth; (void)sm; (void)data; (void)len; }
