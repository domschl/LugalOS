#ifndef LUGALOS_RADIO_MAIN_H
#define LUGALOS_RADIO_MAIN_H

#include <stdint.h>

/* What the kernel hands the radio's main thread, and what comes back: one
 * page, in the radio's own domain, written by U-mode and read by the kernel.
 * The kernel never follows a pointer in it. */
#define RADIO_STAGE_NONE   0
#define RADIO_STAGE_FAILED 1
#define RADIO_STAGE_OSI    2   /* heap and tables up */
#define RADIO_STAGE_COEX   3
#define RADIO_STAGE_INIT   4   /* esp_wifi_init_internal returned 0 */

typedef struct {
    void    *arena;            /* the radio heap, in the radio's domain */
    uint32_t arena_bytes;
    volatile uint32_t stage;
    volatile int32_t  rc_init;
} radio_ctx_t;

void radio_main(uintptr_t arg);

#endif
