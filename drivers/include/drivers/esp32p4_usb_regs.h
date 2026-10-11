/* ESP32-P4 register addresses and fields used by drivers/usbhost_esp32p4.c (the
 * OTG HS core's clocks and resets, its UTMI PHY's control bits).
 * 47.9, plan/phase47_esp32p4_lcd7b_ribbon.md.
 *
 * GENERATED -- do not edit by hand. Regenerate with
 *
 *     python3 tools/gen/p4_regs.py < tools/gen/p4_usb_regs.txt
 *
 * which evaluates ESP-IDF's own macros (components/soc/esp32p4/register/
 * hw_ver1 and hw_ver3, bases included) and fails if the two revisions
 * disagree on any address, shift or mask listed -- except the registers
 * marked "v3 only", which are taken from hw_ver3 alone: the PHY's SUSPENDM
 * register was renamed and the software-controlled pulldowns are new in v3,
 * and this driver only runs on the LCD-7B's v3.2. Names are IDF's. */
#ifndef LUGALOS_DRIVERS_ESP32P4_USB_REGS_H
#define LUGALOS_DRIVERS_ESP32P4_USB_REGS_H
#define HP_SYS_CLKRST_SOC_CLK_CTRL1_REG              0x500e6018u
#define HP_SYS_CLKRST_REG_USB_OTG20_SYS_CLK_EN_S     16u
#define HP_SYS_CLKRST_REG_USB_OTG20_SYS_CLK_EN_V     0x1u
#define LP_CLKRST_HP_USB_CLKRST_CTRL1_REG            0x50111048u
#define LP_CLKRST_RST_EN_USB_OTG20_PHY_S             1u
#define LP_CLKRST_RST_EN_USB_OTG20_PHY_V             0x1u
#define LP_CLKRST_RST_EN_USB_OTG20_S                 2u
#define LP_CLKRST_RST_EN_USB_OTG20_V                 0x1u
#define LP_CLKRST_USB_OTG20_PHYREF_CLK_SRC_SEL_S     28u
#define LP_CLKRST_USB_OTG20_PHYREF_CLK_SRC_SEL_V     0x3u
#define LP_CLKRST_USB_OTG20_PHYREF_CLK_EN_S          30u
#define LP_CLKRST_USB_OTG20_PHYREF_CLK_EN_V          0x1u
/* v3 only: HP_SYSTEM_SYS_USBOTG20_CTRL_REG's fields as hw_ver3 has them */
#define HP_SYSTEM_SYS_USBOTG20_CTRL_REG              0x500e515cu
#define HP_SYSTEM_SYS_OTG_SUSPENDM_S                 21u
#define HP_SYSTEM_SYS_OTG_SUSPENDM_V                 0x1u
/* v3 only: LP_SYSTEM_REG_HP_USB_OTGHS_PHY_CTRL_REG's fields as hw_ver3 has them */
#define LP_SYSTEM_REG_HP_USB_OTGHS_PHY_CTRL_REG      0x50110204u
#define LP_SYSTEM_REG_HP_UTMIOTG_DPPULLDOWN_S        1u
#define LP_SYSTEM_REG_HP_UTMIOTG_DPPULLDOWN_V        0x1u
#define LP_SYSTEM_REG_HP_UTMIOTG_DMPULLDOWN_S        2u
#define LP_SYSTEM_REG_HP_UTMIOTG_DMPULLDOWN_V        0x1u

#endif
