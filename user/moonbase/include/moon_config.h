#ifndef MOON_CONFIG_H
#define MOON_CONFIG_H

#include <stdint.h>

/* Target Display Presets */
#define MOON_PRESET_LCD7B_W   1024
#define MOON_PRESET_LCD7B_H   600

#define MOON_PRESET_TAB5_W    1280
#define MOON_PRESET_TAB5_H    720

/* Default Dimensions (Host starts in LCD-7B preset) */
#define MOON_DEFAULT_WIDTH    MOON_PRESET_LCD7B_W
#define MOON_DEFAULT_HEIGHT   MOON_PRESET_LCD7B_H

/* Isometric Grid Dimensions (2:1 classic isometric ratio) */
#define MOON_TILE_W           64
#define MOON_TILE_H           32
#define MOON_TILE_HALF_W      (MOON_TILE_W / 2)
#define MOON_TILE_HALF_H      (MOON_TILE_H / 2)

/* Map Dimensions (in tiles) */
#define MOON_MAP_SIZE_X       64
#define MOON_MAP_SIZE_Y       64

/* Simulation Timing */
#define MOON_TARGET_FPS       60
#define MOON_FRAME_TIME_MS    (1000 / MOON_TARGET_FPS)
#define MOON_SIM_TICK_MS      100   /* 10 Hz simulation tick */

/* RGB565 Color Helper Macros (5 bits Red, 6 bits Green, 5 bits Blue) */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define RGB565_R(c) (((c) >> 8) & 0xF8)
#define RGB565_G(c) (((c) >> 3) & 0xFC)
#define RGB565_B(c) (((c) << 3) & 0xF8)

/* Common Theme Colors (Lunar Palette) */
#define COLOR_SPACE_BLACK     RGB565(5, 5, 8)
#define COLOR_REGOLITH_DARK   RGB565(45, 45, 48)
#define COLOR_REGOLITH_MID    RGB565(85, 85, 90)
#define COLOR_REGOLITH_LIGHT  RGB565(130, 130, 135)
#define COLOR_CRATER_SHADOW   RGB565(20, 20, 24)
#define COLOR_LUNAR_WHITE     RGB565(225, 230, 235)

#define COLOR_GOLD_FOIL       RGB565(212, 175, 55)
#define COLOR_SOLAR_BLUE      RGB565(30, 90, 180)
#define COLOR_LASER_CYAN      RGB565(0, 220, 255)
#define COLOR_ALERT_RED       RGB565(220, 40, 40)
#define COLOR_STATUS_GREEN    RGB565(40, 200, 60)
#define COLOR_ENERGY_YELLOW   RGB565(255, 210, 30)

#endif /* MOON_CONFIG_H */
