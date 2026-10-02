# Phase 40 — The actionable backlog

**Status: a list, not yet a plan (2026-10-02).** Created by phase 38's 38.0b,
which sorted `plan/open_issues.md` into actionable entries and unexplained
intermittents. The actionable entries that phase 38 does not take land here,
in the order below. Each item's description, trigger and fix are in its
`open_issues.md` entry; this file only orders them and is elaborated into a
proper plan when the phase starts.

**Numbering.** Phase 39 is PSRAM on the ESP32-P4 and waits for the
ESP32-P4-WIFI6-Touch-LCD-7B board. If that board has not arrived when phase 38
ends, this phase can go first; the number is kept either way.

**Order.** The harness first, because it is what turns the intermittents of
`open_issues.md` part B into evidence; then the hazards (a deadlock nothing
can see, a board that hangs); then usability; then features.

| # | Item (`open_issues.md` heading) | Needs on the bench |
|---|---|---|
| 1 | The runner cannot see inside a stuck guest | -- (QEMU) |
| 2 | Two hand-rolled yielding locks are outside the wait-for graph | Pico 2 W, ENC28J60 gateway |
| 3 | Pulling the SD card out of a running board hangs it | LCD-7 |
| 4 | A trailing slash breaks path resolution below a mount root | -- (QEMU) |
| 5 | `cc` searches only /ram0 for a relative `#include` | -- (QEMU) |
| 6 | `K3` checks pin values from a table, not from the build | two RP2350 personas |
| 7 | C6/C7's exact heap comparison is disturbed by background allocation | clock board |
| 8 | The clock display flickers while the radio comes up | clock board |
| 9 | No clean way to leave a BSS before re-joining | Pico 2 W |
| 10 | `mqttd` has no file-backed source | gateway + sensor node |
| 11 | Pressure is published as station pressure | P4 + BME280; **owner decision** on publishing both values |

Taken by phase 38 instead: the USB serial number (38.1), libc's byte loops
(38.3), `sizereport`'s blind spot (38.4), and the chess hot path (38.9).
