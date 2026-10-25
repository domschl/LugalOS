#ifndef KERNEL_OTA_H
#define KERNEL_OTA_H

/* Updating a node's own image over its namespace (kernel/ota.c).
 *
 * Two files, on every node whose board has a backend:
 *
 *   /dev/ota       ctl. Read: the node's update state as key=value lines.
 *                  Write: `begin SIZE`, `commit`, `reboot`, `confirm`, `abort`.
 *   /dev/otaimage  the update image (kernel/include/kernel/ota_image.h),
 *                  written in order from offset 0 after `begin`.
 *
 * `begin` names the image's size; the image streams into the slot this boot
 * is not running from -- emptied once the header has arrived and names this
 * board and chip, so another board's image is refused before anything is
 * erased; `commit` checks the SHA-256 of what is now in flash, then makes the
 * slot complete and asks the next boot to try it once. `reboot` restarts
 * into it. The new image runs on trial and confirms itself once the board
 * calls itself healthy
 * (ota_board_healthy()); a trial that does not is reset by the board's
 * watchdog, and the old slot boots again. Nothing here knows what chip it is
 * on: the backend (drivers/ota_<chip>.c) does the flash and the booting. */

#include <stdbool.h>
#include <stdint.h>
#include "kernel/ota_image.h"

int ota_ctl_write(const char *buf, uint32_t len);
uint32_t ota_ctl_read(char *out, uint32_t cap);
/* The image file: `len` bytes at `off`. The count, or -1. */
int ota_image_write(const void *buf, uint32_t len, uint64_t off);
/* Reads raw bytes from the inactive slot when idle (for factory slot backup). */
int ota_image_read(void *buf, uint32_t len, uint64_t off);
/* At boot: on a trial boot, the task that confirms the image or gives up on it. */
void ota_start(void);

/* The release key in the identity store changed (`otakey`): read it again.
 * Task context that may read the identity store -- not the 9P server. */
void ota_release_key_changed(void);
/* The console's override: the next image whose header arrives within
 * `window_s` seconds may be unsigned, once. Never reachable from the network
 * (the shell marks `otakey` privileged). */
void ota_allow_unsigned_once(uint32_t window_s);

/* --- the board's half (drivers/ota_<chip>.c) ------------------------------- */

typedef struct {
    uint32_t nslots;
    int      running;           /* the slot this boot runs from; -1: this boot cannot update */
    bool     trial;             /* running a new image that has not confirmed */
    bool     fell_back;         /* the last trial did not confirm */
    uint32_t part_max[2];       /* the largest part 0 / part 1 a slot holds */
    const char *chip;
    const char *layout;         /* "loader" or "idf" */
    uint32_t trial_s;           /* the watchdog's deadline for a trial */
} ota_board_info_t;

typedef struct {
    int  active;                /* the record's slots, -1 when there is no record */
    int  pending;
    bool tried;
    bool failed;
} ota_board_record_t;

bool ota_board_info(ota_board_info_t *out);
/* At boot, before anything else here: the board settles what a trial left behind. */
void ota_board_boot(void);
/* Invalidates `slot` (its header first) and gets ready to receive into it. */
int  ota_board_begin(int slot);
/* Part bytes in order: `off` counts from the first byte after the header. */
int  ota_board_write(int slot, const ota_image_header_t *h, uint32_t off, const void *buf, uint32_t len);
/* Everything received is in flash. */
int  ota_board_finish(int slot, const ota_image_header_t *h);
int  ota_board_read(int slot, const ota_image_header_t *h, uint32_t off, void *buf, uint32_t len);
int  ota_board_raw_read(int slot, uint32_t off, void *buf, uint32_t len);
/* Writes the header: from now on the slot holds a complete image. */
int  ota_board_seal(int slot, const ota_image_header_t *h);
/* 0 and the header when `slot` holds a sealed image. */
int  ota_board_header(int slot, ota_image_header_t *h);
int  ota_board_set_pending(int slot);
int  ota_board_clear_pending(void);
/* Switch next boot to the valid image already in the other slot. */
int  ota_board_switch(void);
/* The running trial is good: make it the active slot, disarm the watchdog. */
int  ota_board_confirm(void);
bool ota_board_record(ota_board_record_t *out);
/* The board's own test of a trial (its links up, say). */
bool ota_board_healthy(void);

#endif
