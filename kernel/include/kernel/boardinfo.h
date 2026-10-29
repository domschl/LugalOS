#ifndef KERNEL_BOARDINFO_H
#define KERNEL_BOARDINFO_H

/* Which board is this? One answer for every surface that has to say so: /proc/board (readable
 * without a grant, so a discovery tool or the web page's sign-in screen can ask before it has a
 * key), the `board` shell command, the console banner, the boot log and the admin page.
 *
 * Only facts that identify a build and a board, never a secret or a network fact:
 *
 *   preset    the build directory's name (rp2350-wifi, esp32p4, rv64-smp): which software was built
 *   board     the board file it was built for (esp32p4-nano): what LUGALOS_BOARD_ID names
 *   hardware  a human description of the board (Waveshare ESP32-P4-NANO)
 *   chip      the silicon, with its revision where the chip reports one (ESP32-P4 v1.3)
 *   version, build, tainted    as /proc/version and /proc/buildid
 *   name, uid                  who it is (kernel/identity.h); uid is "none" before one exists
 */

#include <stdint.h>

/* The key=value text of /proc/board, NUL-terminated. Returns its length. */
uint32_t board_info_text(char *out, uint32_t cap);

/* One line: "<preset> v<version> (<hardware>, <chip>) <name> uid <uid>". */
uint32_t board_info_line(char *out, uint32_t cap);

#endif
