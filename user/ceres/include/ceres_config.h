/* user/ceres/include/ceres_config.h — Ceres Alpha Configuration & Video Dimensions */
#ifndef CERES_CONFIG_H
#define CERES_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* Virtual Playfield Dimensions (16:9 widescreen arcade standard) */
#define CERES_VIRTUAL_W       640
#define CERES_VIRTUAL_H       360

/* Target Hardware Physical Presets */
#define CERES_PRESET_LCD7B_W  1024
#define CERES_PRESET_LCD7B_H  600

#define CERES_PRESET_TAB5_W   1280
#define CERES_PRESET_TAB5_H   720

/* Default Dimensions */
#define CERES_DEFAULT_W       CERES_VIRTUAL_W
#define CERES_DEFAULT_H       CERES_VIRTUAL_H

/* Simulation Timing */
#define CERES_TARGET_FPS      60
#define CERES_FRAME_TIME_MS   (1000 / CERES_TARGET_FPS) /* ~16.66 ms */

/* RGB565 Helper Macros (5 bits Red, 6 bits Green, 5 bits Blue) */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define RGB565_R(c) (((c) >> 8) & 0xF8)
#define RGB565_G(c) (((c) >> 3) & 0xFC)
#define RGB565_B(c) (((c) << 3) & 0xF8)

/* Chromakey Transparency Color (Classic Magenta / Fuchsia) */
#define CERES_CHROMA_KEY      0xF81F

/* Palette Constants */
#define COLOR_SPACE_BLACK        RGB565(4, 4, 8)
#define COLOR_STAR_WHITE         RGB565(250, 250, 255)
#define COLOR_STAR_DIM           RGB565(120, 120, 140)
#define COLOR_STAR_BLUE          RGB565(140, 180, 255)

/* Rock & Cavern Strata Palette */
#define COLOR_ROCK_DARK          RGB565(40, 32, 28)
#define COLOR_ROCK_MID           RGB565(80, 68, 56)
#define COLOR_ROCK_LIGHT         RGB565(130, 110, 90)
#define COLOR_ROCK_EDGE          RGB565(190, 165, 130)

/* Ship & Laser Palette */
#define COLOR_SHIP_BODY          RGB565(230, 235, 240)
#define COLOR_SHIP_COCKPIT       RGB565(0, 220, 255)
#define COLOR_SHIP_TRIM          RGB565(240, 50, 50)
#define COLOR_THRUSTER_ORANGE    RGB565(255, 140, 20)
#define COLOR_THRUSTER_YELLOW    RGB565(255, 230, 50)

#define COLOR_LASER_CYAN         RGB565(50, 240, 255)
#define COLOR_BOMB_YELLOW        RGB565(255, 220, 30)

/* Enemy & Threat Palette */
#define COLOR_ENEMY_UFO_RED      RGB565(230, 40, 40)
#define COLOR_ENEMY_UFO_GOLD     RGB565(240, 190, 30)
#define COLOR_MISSILE_WHITE      RGB565(245, 245, 245)
#define COLOR_METEOR_BROWN       RGB565(150, 80, 40)
#define COLOR_EXPLOSION_ORANGE   RGB565(255, 120, 20)

/* Fuel, Survivor & Objective Palette */
#define COLOR_FUEL_BLUE          RGB565(40, 140, 255)
#define COLOR_SURVIVOR_GREEN     RGB565(50, 230, 70)
#define COLOR_TRACTOR_BEAM       RGB565(60, 255, 160)
#define COLOR_HUD_TEXT           RGB565(240, 240, 240)
#define COLOR_HUD_ALERT          RGB565(255, 60, 60)

#endif /* CERES_CONFIG_H */
