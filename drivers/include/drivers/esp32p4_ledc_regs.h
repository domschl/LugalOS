/* ESP32-P4 register addresses and fields used by drivers/dsi_esp32p4.c's
 * backlight PWM (the LEDC, its clocks, the GPIO matrix).
 * 47.11, plan/phase47_esp32p4_lcd7b_ribbon.md.
 *
 * GENERATED -- do not edit by hand. Regenerate with
 *
 *     python3 tools/gen/p4_regs.py < tools/gen/p4_ledc_regs.txt
 *
 * which evaluates ESP-IDF's own macros (components/soc/esp32p4/register/
 * hw_ver1 and hw_ver3, bases included) and fails if the two revisions
 * disagree on any address, shift or mask listed. Names are IDF's. */
#ifndef LUGALOS_DRIVERS_ESP32P4_LEDC_REGS_H
#define LUGALOS_DRIVERS_ESP32P4_LEDC_REGS_H
#define HP_SYS_CLKRST_SOC_CLK_CTRL3_REG              0x500e6020u
#define HP_SYS_CLKRST_REG_LEDC_APB_CLK_EN_S          0u
#define HP_SYS_CLKRST_REG_LEDC_APB_CLK_EN_V          0x1u
#define HP_SYS_CLKRST_HP_RST_EN1_REG                 0x500e60c4u
#define HP_SYS_CLKRST_REG_RST_EN_LEDC_S              29u
#define HP_SYS_CLKRST_REG_RST_EN_LEDC_V              0x1u
#define HP_SYS_CLKRST_PERI_CLK_CTRL22_REG            0x500e609cu
#define HP_SYS_CLKRST_REG_LEDC_CLK_SRC_SEL_S         0u
#define HP_SYS_CLKRST_REG_LEDC_CLK_SRC_SEL_V         0x3u
#define HP_SYS_CLKRST_REG_LEDC_CLK_EN_S              2u
#define HP_SYS_CLKRST_REG_LEDC_CLK_EN_V              0x1u
#define LEDC_CH0_CONF0_REG                           0x500d3000u
#define LEDC_TIMER_SEL_CH0_S                         0u
#define LEDC_TIMER_SEL_CH0_V                         0x3u
#define LEDC_SIG_OUT_EN_CH0_S                        2u
#define LEDC_SIG_OUT_EN_CH0_V                        0x1u
#define LEDC_IDLE_LV_CH0_S                           3u
#define LEDC_IDLE_LV_CH0_V                           0x1u
#define LEDC_PARA_UP_CH0_S                           4u
#define LEDC_PARA_UP_CH0_V                           0x1u
#define LEDC_CH0_HPOINT_REG                          0x500d3004u
#define LEDC_CH0_DUTY_REG                            0x500d3008u
#define LEDC_DUTY_CH0_S                              0u
#define LEDC_DUTY_CH0_V                              0x1ffffffu
#define LEDC_CH0_CONF1_REG                           0x500d300cu
#define LEDC_DUTY_START_CH0_S                        31u
#define LEDC_DUTY_START_CH0_V                        0x1u
#define LEDC_TIMER0_CONF_REG                         0x500d30a0u
#define LEDC_TIMER0_DUTY_RES_S                       0u
#define LEDC_TIMER0_DUTY_RES_V                       0x1fu
#define LEDC_CLK_DIV_TIMER0_S                        5u
#define LEDC_CLK_DIV_TIMER0_V                        0x3ffffu
#define LEDC_TIMER0_PAUSE_S                          23u
#define LEDC_TIMER0_PAUSE_V                          0x1u
#define LEDC_TIMER0_RST_S                            24u
#define LEDC_TIMER0_RST_V                            0x1u
#define LEDC_TIMER0_PARA_UP_S                        26u
#define LEDC_TIMER0_PARA_UP_V                        0x1u
#define LEDC_CH0_GAMMA_CONF_REG                      0x500d3100u
#define LEDC_CH0_GAMMA_ENTRY_NUM_S                   0u
#define LEDC_CH0_GAMMA_ENTRY_NUM_V                   0x1fu
#define LEDC_CONF_REG                                0x500d3170u
#define LEDC_CLK_EN_S                                31u
#define LEDC_CLK_EN_V                                0x1u
#define GPIO_FUNC0_OUT_SEL_CFG_REG                   0x500e0558u
#define GPIO_FUNC0_OUT_SEL_S                         0u
#define GPIO_FUNC0_OUT_SEL_V                         0x1ffu
#define GPIO_FUNC0_OUT_INV_SEL_S                     9u
#define GPIO_FUNC0_OUT_INV_SEL_V                     0x1u

#endif
