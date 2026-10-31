/* user/ceres/include/ceres_sprites.h — Pre-rendered Sci-Fi Sprites */
#ifndef CERES_SPRITES_H
#define CERES_SPRITES_H

#include <stdint.h>
#include "ceres_blitter.h"

#define CERES_KEY_COLOR 0x0000

#define SPRITE_FUEL_SILO_W 32
#define SPRITE_FUEL_SILO_H 24
extern const uint16_t g_palette_fuel_silo[16];
extern const uint8_t  g_packed_fuel_silo[384];

#define SPRITE_SAM_SILO_W 24
#define SPRITE_SAM_SILO_H 16
extern const uint16_t g_palette_sam_silo[16];
extern const uint8_t  g_packed_sam_silo[192];

#define SPRITE_SAM_ROCKET_W 12
#define SPRITE_SAM_ROCKET_H 24
extern const uint16_t g_palette_sam_rocket[16];
extern const uint8_t  g_packed_sam_rocket[144];

#define SPRITE_RADAR_DISH_W 24
#define SPRITE_RADAR_DISH_H 24
extern const uint16_t g_palette_radar_dish[16];
extern const uint8_t  g_packed_radar_dish[288];

#define SPRITE_CRYO_BUNKER_W 32
#define SPRITE_CRYO_BUNKER_H 24
extern const uint16_t g_palette_cryo_bunker[16];
extern const uint8_t  g_packed_cryo_bunker[384];

#define SPRITE_SURVIVOR_W 16
#define SPRITE_SURVIVOR_H 18
extern const uint16_t g_palette_survivor[16];
extern const uint8_t  g_packed_survivor[144];

#define SPRITE_PLAYER_SHIP_W 28
#define SPRITE_PLAYER_SHIP_H 14
extern const uint16_t g_palette_player_ship[16];
extern const uint8_t  g_packed_player_ship[196];

#define SPRITE_SAUCER_W 24
#define SPRITE_SAUCER_H 14
extern const uint16_t g_palette_saucer[16];
extern const uint8_t  g_packed_saucer[168];

#define SPRITE_FIGHTER_W 20
#define SPRITE_FIGHTER_H 12
extern const uint16_t g_palette_fighter[16];
extern const uint8_t  g_packed_fighter[120];

#define SPRITE_METEOR_W 18
#define SPRITE_METEOR_H 18
extern const uint16_t g_palette_meteor[16];
extern const uint8_t  g_packed_meteor[162];

#define SPRITE_HARVESTER_W 24
#define SPRITE_HARVESTER_H 20
extern const uint16_t g_palette_harvester[16];
extern const uint8_t  g_packed_harvester[240];

#define SPRITE_PARACHUTIST_W 16
#define SPRITE_PARACHUTIST_H 24
extern const uint16_t g_palette_parachutist[16];
extern const uint8_t  g_packed_parachutist[192];

/* PSRAM Decompression Buffers (owned by ceres_game_t) */
typedef struct {
    uint16_t fuel_silo[768];
    uint16_t sam_silo[384];
    uint16_t sam_rocket[288];
    uint16_t radar_dish[576];
    uint16_t cryo_bunker[768];
    uint16_t survivor[288];
    uint16_t player_ship[392];
    uint16_t saucer[336];
    uint16_t fighter[240];
    uint16_t meteor[324];
    uint16_t harvester[480];
    uint16_t parachutist[384];
} ceres_sprite_buffers_t;

/* Live RGB565 Surfaces (pointing into ceres_sprite_buffers_t) */
typedef struct {
    ceres_surface_t fuel_silo;
    ceres_surface_t sam_silo;
    ceres_surface_t sam_rocket;
    ceres_surface_t radar_dish;
    ceres_surface_t cryo_bunker;
    ceres_surface_t survivor;
    ceres_surface_t player_ship;
    ceres_surface_t saucer;
    ceres_surface_t fighter;
    ceres_surface_t meteor;
    ceres_surface_t harvester;
    ceres_surface_t parachutist;
} ceres_surfaces_t;

/* Global surfaces accessed during gameplay */
extern ceres_surface_t g_surf_fuel_silo;
extern ceres_surface_t g_surf_sam_silo;
extern ceres_surface_t g_surf_sam_rocket;
extern ceres_surface_t g_surf_radar_dish;
extern ceres_surface_t g_surf_cryo_bunker;
extern ceres_surface_t g_surf_survivor;
extern ceres_surface_t g_surf_player_ship;
extern ceres_surface_t g_surf_saucer;
extern ceres_surface_t g_surf_fighter;
extern ceres_surface_t g_surf_meteor;
extern ceres_surface_t g_surf_harvester;
extern ceres_surface_t g_surf_parachutist;

/* Fast PSRAM Unpacker called once at initialization */
void ceres_sprites_unpack(ceres_sprite_buffers_t *bufs, ceres_surfaces_t *surfs);

#endif /* CERES_SPRITES_H */
