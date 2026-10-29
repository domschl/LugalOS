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

#define SPRITE_BATTERY_BANK_W 64
#define SPRITE_BATTERY_BANK_H 48
extern const uint16_t g_sprite_battery_bank[3072];
extern const moon_surface_t g_surf_battery_bank;

#define SPRITE_HABITAT_DOME_W 96
#define SPRITE_HABITAT_DOME_H 80
extern const uint16_t g_sprite_habitat_dome[7680];
extern const moon_surface_t g_surf_habitat_dome;

#define SPRITE_SINTERER_W 64
#define SPRITE_SINTERER_H 48
extern const uint16_t g_sprite_sinterer[3072];
extern const moon_surface_t g_surf_sinterer;

#define SPRITE_O2_EXTRACTOR_W 64
#define SPRITE_O2_EXTRACTOR_H 48
extern const uint16_t g_sprite_o2_extractor[3072];
extern const moon_surface_t g_surf_o2_extractor;

#define SPRITE_HYDROLOX_PLANT_W 96
#define SPRITE_HYDROLOX_PLANT_H 80
extern const uint16_t g_sprite_hydrolox_plant[7680];
extern const moon_surface_t g_surf_hydrolox_plant;

#define SPRITE_CRYO_DEWAR_W 64
#define SPRITE_CRYO_DEWAR_H 48
extern const uint16_t g_sprite_cryo_dewar[3072];
extern const moon_surface_t g_surf_cryo_dewar;

#endif /* MOON_SPRITES_H */
