#ifndef KERNEL_SETUP_H
#define KERNEL_SETUP_H

/* Claiming a node, and changing its owner part over the network (kernel/setup.c;
 * plan/phase52_remote_admin.md 1.3, 1.4 and 52.10).
 *
 * The identity record has a factory part (uid, device key, release key, setup secret) that only
 * the console and tools/provision.py write, and an owner part (name, WLAN, IP, grants, ...) that
 * the network may write once the node is claimed, with the `admin` grant. A node with no admin
 * grant is UNCLAIMED. While a setup window is OPEN an unclaimed node takes one `claim` (the
 * owner's key, as the `owner` admin grant, and optionally a name and a WLAN) from a client that
 * knows its setup secret; that claims it, closes the window, and from then on only an `admin`
 * grant may write /dev/owner. An owner reset (console or button) returns it to UNCLAIMED with
 * the factory part intact.
 *
 * Two files, only in a build with CONFIG_NETWORK_CLAIM (a build without it cannot be claimed
 * over the network, whatever a bug does to the state machine):
 *
 *   /dev/setup  read: state, the seconds the window has left, failed attempts, the last result.
 *               write: `claim SECRET KEYHEX [name NAME] [wlan SSID PSKHEX]`.
 *   /dev/owner  admin only. read: name, ssid, the grants (no keys). write: `name NAME`,
 *               `wlan SSID PSKHEX`, `grant NAME HEX [ANAME] [ro|rw|admin]`, `ungrant NAME`,
 *               `setup start`, `setup stop`. An owner reset is not here: it is physical.
 *
 * The identity store can only be touched from a task that may claim the scratch buffer, and the
 * 9P server task is not one, so a write is queued for a worker task and its result read back
 * from /dev/setup.
 */

#include <stdbool.h>
#include <stdint.h>
#include "kernel/identity.h"

#define SETUP_WINDOW_S      900u    /* how long a setup window stays open */
#define SETUP_MAX_ATTEMPTS  5u      /* failed claims that close it until the next trigger */

/* The setup access point's own address and network (kernel/setup_ap.c). */
#define SETUP_AP_IP_STR "192.168.4.1"

typedef enum { SETUP_UNCLAIMED, SETUP_OPEN, SETUP_CLAIMED } setup_state_t;

/* The task that raises and lowers the setup access point with the window (a Pico 2 W; a no-op elsewhere). */
void setup_ap_start(void);

/* Boot task, after the grants are loaded: opens the window on an unclaimed node that has a setup
 * secret, and starts the worker. */
void setup_init(void);

/* The button task (kernel/setup_button.c): a long press opens a setup window, a longer one resets
 * the owner part. A no-op on a board that names no button. */
void setup_button_start(void);

setup_state_t setup_state(void);
const char   *setup_state_name(setup_state_t s);

/* --- The setup network (52.12) ---
 *
 * The one network the claim door opens on: the setup access point's, or any network the console names with
 * `setup net`. An unauthenticated attach to /dev/setup is accepted only from a peer on it (fs/9p.c asks
 * through the link, kernel/setup.c answers), and secrets may cross it unencrypted because the owner stands
 * next to the node, inside a WPA2 link keyed by the setup secret. */
void setup_net_set(const uint8_t ip[4], const uint8_t mask[4]);
void setup_net_clear(void);
bool setup_net_contains(const uint8_t ip[4]);

/* Should the setup access point be up right now? A window is open, and either someone asked for it (the
 * console, a button) or the node has no Wi-Fi of its own to join: a node provisioned with credentials and
 * no owner boots as the station it was set up to be. Polled by kernel/setup_ap.c. */
bool setup_ap_wanted(void);

/* After a claim the setup network stays up for up to SETUP_HOLD_S seconds, so the page that shows the new
 * owner key stays reachable while the claimer saves it. The page ends the hold through `GET /setup-done`
 * (net/httpd.c); setup_stop() ends it too. */
#define SETUP_HOLD_S 600u
void setup_ap_release(void);

/* Opens a window of SETUP_WINDOW_S seconds, with the attempts counted again. False in a build
 * without CONFIG_NETWORK_CLAIM, or on a node with no setup secret to claim with. */
bool setup_start(void);
void setup_stop(void);

/* The owner reset: wipes the owner part, empties the RAM copy of the grants, and leaves the node
 * unclaimed with its factory part. Console and buttons only. */
node_id_result_t setup_reset_owner(void);

/* The node has an address (net_set_address(), called with no lock held). On a board with
 * CONFIG_HTTPD_WHEN_CLAIMED a claimed node then serves its admin page: the owner signs in at NAME.local
 * with their key. A claim made on the console while the node is up takes effect at its next address. */
void setup_address_set(void);

/* May an attach with no authentication reach /dev/setup? Only with a window open, on a link the
 * caller says is the setup access point's (fs/9p.c asks). */
bool setup_unauth_attach_ok(void);

/* The files. The write returns at once with the number of bytes taken, or -1 if it was
 * refused (no window, a claim already pending, a malformed line). */
uint32_t setup_ctl_read(char *out, uint32_t cap);
int      setup_ctl_write(const char *buf, uint32_t len);
uint32_t owner_ctl_read(char *out, uint32_t cap);
int      owner_ctl_write(const char *buf, uint32_t len);

#endif
