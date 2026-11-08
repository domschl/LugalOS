#ifndef LUGALOS_DRIVERS_DC8200_JH7110_H
#define LUGALOS_DRIVERS_DC8200_JH7110_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "drivers/hdmi_jh7110.h"

#define DC8200_BASE_ADDR        0x29400000UL
#define DC8200_REGS_HI          0x29400000UL
#define DC8200_REGS_MAIN        0x29400800UL

/* Display controller state */
typedef struct {
    bool     initialized;
    bool     panel_started;
    uint16_t width;
    uint16_t height;
    uint16_t stride_bytes;
    uint8_t  front_idx;
    uint8_t  back_idx;
    uint32_t front_fb_pa;
    uint32_t back_fb_pa;
    uint16_t *front_fb;     /* CPU pointer (uncached alias) */
    uint16_t *back_fb;      /* CPU pointer (uncached alias) */
    uint32_t frames_flipped;
    uint32_t vsync_count;
    uint32_t revision;
    uint32_t chip_cid;
} dc8200_state_t;

/* Bring up DC8200 display controller, VTG, and Layer 0 DMA scanout */
int  jh7110_dc8200_init(hdmi_resolution_t res);

/* Start lcdterm terminal on DC8200 panel */
int  jh7110_dc8200_term_start(void);
int  jh7110_dc8200_term_task_start(void);

/* Tear-free page flip on VSYNC */
void jh7110_dc8200_flip(void);

/* Test patterns & shell diagnostics */
void jh7110_dc8200_test_pattern(void);
void jh7110_dc8200_repaint(void);
void jh7110_dc8200_cmd_status(void);
void jh7110_dc8200_cmd_test(void);

/* Buffer accessors */
uint16_t *jh7110_dc8200_get_front_fb(void);
uint16_t *jh7110_dc8200_get_back_fb(void);
void      jh7110_dc8200_get_res(unsigned *w, unsigned *h);
const dc8200_state_t *jh7110_dc8200_get_state(void);

#endif /* LUGALOS_DRIVERS_DC8200_JH7110_H */
