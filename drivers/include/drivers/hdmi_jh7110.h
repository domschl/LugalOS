#ifndef LUGALOS_DRIVERS_HDMI_JH7110_H
#define LUGALOS_DRIVERS_HDMI_JH7110_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define JH7110_HDMI_BASE        0x29590000UL
#define JH7110_DSSCTRL_BASE     0x295B0000UL
#define JH7110_VOUTCRG_BASE     0x295C0000UL
#define JH7110_DC8200_BASE      0x29400000UL
#define JH7110_AON_PMU_BASE     0x17030000UL

typedef enum {
    HDMI_RES_1280_720P_60HZ  = 0,
    HDMI_RES_1920_1080P_60HZ = 1,
} hdmi_resolution_t;

typedef struct {
    bool     connected;         /* HPD detected */
    bool     edid_valid;        /* Valid EDID checksum & header */
    char     mfg_id[4];         /* 3-letter manufacturer code */
    uint16_t product_code;
    uint32_t serial_number;
    uint8_t  width_cm;          /* Screen width in cm */
    uint8_t  height_cm;         /* Screen height in cm */
    uint16_t dpi;               /* Computed dots per inch */
    uint16_t active_w;          /* Active horizontal pixels (e.g. 1920) */
    uint16_t active_h;          /* Active vertical pixels (e.g. 1080) */
    uint32_t pixel_clock_khz;   /* Pixel clock in kHz (e.g. 148500) */
    hdmi_resolution_t target_res;
    uint8_t  raw_edid[128];
} hdmi_monitor_info_t;

/* Initialize JH7110 VOUT power domain, clocks, resets, and HDMI transmitter */
int  jh7110_hdmi_init(void);

/* Check monitor connection and status */
bool jh7110_hdmi_is_connected(void);
const hdmi_monitor_info_t *jh7110_hdmi_get_monitor_info(void);

/* Shell diagnostics */
void jh7110_hdmi_cmd_status(void);
void jh7110_hdmi_cmd_edid(void);

#endif /* LUGALOS_DRIVERS_HDMI_JH7110_H */
