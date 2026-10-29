/* The setup access point (plan/phase52, 52.12): while an unclaimed node's setup window is open, the Pico 2 W's
 * radio is a WPA2 access point whose passphrase is the node's setup secret (the label, the QR code), serving
 * the setup page to whoever joins. A claim, a timeout or `setup stop` ends it, and the radio goes back to
 * being a station -- joining the network the owner just entered.
 *
 * One task owns the transitions. The radio is reset to freshly loaded firmware each way (about 25 s): an
 * access point and a station configure the chip so differently that carrying one over into the other is a
 * source of faults this one-time exchange has no need to take on. */

#include "lugalos_config.h"
#include "kernel/setup.h"

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_WL_CS_GPIO) && defined(CONFIG_NETWORK_CLAIM)

#include "drivers/cyw43.h"
#include "kernel/identity.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "net/dhcpd.h"
#include "net/httpd.h"
#include "net/ip.h"
#include <string.h>

#define AP_CHANNEL        6
#define AP_CLOSE_GRACE_MS 5000u    /* after the window closes: the page is still reading the result */

static const uint8_t AP_IP[4]   = { 192, 168, 4, 1 };
static const uint8_t AP_MASK[4] = { 255, 255, 255, 0 };
static const uint8_t AP_NONE[4] = { 0, 0, 0, 0 };

/* single task: setupap */
static bool     g_up;
static bool     g_httpd_ours;       /* httpd was started by this task, so this task stops it */
static uint64_t g_closing_since_ms;

/* "LugalOS-" and the last two bytes of the UID, in capitals: on the label beside the secret
 * (tools/provision.py prints the same). A node without a UID uses its MAC. */
static void ssid_for(char *out, uint32_t cap) {
    uint8_t id[NODE_UID_LEN];
    const uint8_t *b;
    uint8_t lo, hi;
    if (node_uid(id)) { hi = id[NODE_UID_LEN - 2]; lo = id[NODE_UID_LEN - 1]; }
    else { b = node_mac(); hi = b[4]; lo = b[5]; }
    ksnprintf(out, cap, "LugalOS-%02X%02X", hi, lo);
}

static bool bring_up(void) {
    char secret[NODE_SETUP_SECRET_MAX + 1], ssid[16];
    if (!node_setup_secret(secret, sizeof secret)) return false;
    ssid_for(ssid, sizeof ssid);
    printk("[SetupAP] opening \"%s\"\n", ssid);

    cyw43_sta_hold(true);
    dhcp_hold(true);
    bool ok = cyw43_gspi_probe() && cyw43_ap_start(ssid, secret, AP_CHANNEL);
    memset(secret, 0, sizeof secret);
    if (!ok) {
        printk("[SetupAP] could not start the access point\n");
        dhcp_hold(false);
        cyw43_sta_hold(false);
        return false;
    }
    net_set_address(AP_IP, AP_MASK, AP_NONE);
    static const dhcpd_config_t cfg = { .server_ip = { 192, 168, 4, 1 }, .mask = { 255, 255, 255, 0 },
                                        .first_ip = { 192, 168, 4, 10 }, .portal_uri = "http://" SETUP_AP_IP_STR "/" };
    if (setup_net_services_start(&cfg) != 0) printk("[SetupAP] no address server: clients need static addresses\n");
    setup_net_set(AP_IP, AP_MASK);
    if (!httpd_running(NULL)) g_httpd_ours = httpd_start(80) == 0;
    g_up = true;
    g_closing_since_ms = 0;
    return true;
}

static void tear_down(void) {
    printk("[SetupAP] closing\n");
    setup_net_clear();
    setup_net_services_stop();
    if (g_httpd_ours) { httpd_stop(); g_httpd_ours = false; }
    cyw43_ap_stop();
    net_clear_address();
    dhcp_hold(false);
    cyw43_sta_hold(false);
    g_up = false;
    /* A station again: fresh firmware, and the supervisor if the owner just gave the node a network. */
    char ssid[NODE_WLAN_SSID_MAX + 1];
    if (cyw43_gspi_probe() && node_wlan_ssid(ssid, sizeof ssid)) (void)cyw43_autostart_task_start();
}

static void setup_ap_task(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(500);
        bool want = setup_ap_wanted();
        if (want && !g_up) {
            (void)bring_up();
            if (!g_up) task_sleep_ms(30000);       /* do not hammer a radio that will not start */
        } else if (g_up && want) {
            g_closing_since_ms = 0;
        } else if (g_up && !want) {
            uint64_t now = time_get_ms();
            if (!g_closing_since_ms) g_closing_since_ms = now;
            else if (now - g_closing_since_ms >= AP_CLOSE_GRACE_MS) tear_down();
        }
    }
}

void setup_ap_start(void) {
    if (task_create_sized("setupap", setup_ap_task, NULL, 4) < 0) printk("[SetupAP] could not start the task\n");
}

#else
void setup_ap_start(void) { }
#endif
