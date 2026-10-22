#ifndef MOON_SPRITES_H
#define MOON_SPRITES_H

#include <stdint.h>
#include "moon_blitter.h"

#define MOON_KEY_COLOR 0x0000

#define SPRITE_LANDER_W 64
#define SPRITE_LANDER_H 64
extern const uint16_t g_sprite_lander[4096];
extern const moon_surface_t g_surf_lander;

#define SPRITE_SOLAR_ARRAY_W 64
#define SPRITE_SOLAR_ARRAY_H 48
extern const uint16_t g_sprite_solar_array[3072];
extern const moon_surface_t g_surf_solar_array;

#endif /* MOON_SPRITES_H */
