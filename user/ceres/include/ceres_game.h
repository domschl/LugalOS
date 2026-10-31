/* user/ceres/include/ceres_game.h — Ceres Alpha State Machine & Game Engine */
#ifndef CERES_GAME_H
#define CERES_GAME_H

#include <stdint.h>
#include <stdbool.h>
#include "ceres_config.h"
#include "ceres_platform.h"
#include "ceres_blitter.h"
#include "ceres_sprites.h"
#include "ceres_terrain.h"
#include "ceres_entities.h"

typedef enum {
    CERES_STATE_TITLE = 0,
    CERES_STATE_PLAYING,
    CERES_STATE_PAUSED,
    CERES_STATE_GAMEOVER,
    CERES_STATE_VICTORY,
} ceres_game_state_t;

#define CERES_SECTOR_COUNT        6
#define CERES_SECTOR_LENGTH_COLS  1800

typedef enum {
    CERES_SECTOR_MOUNTAIN = 1,
    CERES_SECTOR_CAVERN   = 2,
    CERES_SECTOR_TRENCH   = 3,
    CERES_SECTOR_BASE     = 4,
    CERES_SECTOR_LABYRINTH= 5,
    CERES_SECTOR_STRONGHOLD = 6,
} ceres_sector_t;

/* Active Player Craft State */
typedef struct {
    int32_t  x;             /* Screen X position (pixels) */
    int32_t  y;             /* Screen Y position (pixels) */
    int32_t  vx;            /* Horizontal velocity */
    int32_t  vy;            /* Vertical velocity */
    int      bank;          /* Pitch tilt: -1 (dive), 0 (level), +1 (climb) */
    int32_t  fuel;          /* Fuel remaining: 0..1000 (milliscale, 1000 = 100.0%) */
    int32_t  lives;         /* Ships in reserve */
    uint32_t score;         /* Current score */
    uint32_t hi_score;      /* High score */
    int      rescued;       /* Survivors rescued in current sector */
    int      total_rescued; /* Total survivors in current sector */
    int      sector;        /* Current sector index (1..6) */
    bool     thruster_on;   /* Thruster animation active */
    int      thrust_boost;  /* -1 = brake/left, 0 = normal, +1 = forward/boost */
    bool     laser_fired;   /* Indicator: laser fired */
    bool     bomb_dropped;  /* Indicator: bomb dropped */
    uint32_t weapon_flash_ms; /* Visual flash timer for weapon test */

    /* Search & Rescue Tractor Scoop */
    bool     tractor_active;   /* Low-altitude tractor beam active */
    int32_t  tractor_target_x; /* Screen X of targeted survivor */
    int32_t  tractor_target_y; /* Screen Y of targeted survivor */

    /* Life & Respawn */
    bool     alive;            /* Ship is operational */
    int      respawn_timer;    /* Frames until respawn after explosion */
    int      invincible_frames;/* Flashing invulnerability after respawn */
    bool     low_fuel_alert;   /* Fuel < 20% alert indicator */
} ceres_player_t;

/* Top-level Game Context */
typedef struct {
    ceres_game_state_t state;
    bool                  running;
    int                   width;
    int                   height;
    uint32_t              frame_count;
    uint32_t              fps;
    uint32_t              last_fps_update;
    uint32_t              frames_since_fps;
    uint32_t              scroll_x;     /* Camera scroll distance (pixels) */

    /* Input State */
    bool                  key_up;
    bool                  key_down;
    bool                  key_left;
    bool                  key_right;
    bool                  key_fire;
    bool                  key_bomb;

    /* Player Entity */
    ceres_player_t     player;

    /* Projectiles & Particle Entities */
    ceres_entities_t   entities;

    /* Streaming Cavern & Parallax Engine */
    ceres_terrain_t    terrain;

    /* PSRAM Unpacked Sprite Pixel Buffers */
    ceres_sprite_buffers_t sprite_bufs;

    /* Performance Metrics */
    uint32_t              last_frame_us;
    uint32_t              last_render_us;

    /* On-Screen Notification Banner (e.g. HEROIC RESCUE! +1000) */
    uint16_t              banner_timer;
    char                  banner_text[32];

    /* Volume Toast Notification & Entry State */
    uint16_t              volume_toast_frames; /* Frames remaining for volume toast */
    bool                  volume_entry_active; /* Waiting for 0-9 after 'v' pressed */
    uint16_t              volume_entry_frames; /* Timeout for waiting for 0-9 */
    uint8_t               volume_percent;      /* 0..100 */

    /* Sector & Meltdown State */
    uint32_t              sector_start_col; /* Starting column of current sector */
    uint32_t              sector_length;    /* Length of sector in columns */
    uint32_t              loop_count;       /* Completed game loops */
    uint16_t              meltdown_timer;   /* Base Meltdown alarm frames */
    uint8_t               screen_shake;     /* Screen tremor intensity (pixels) */
    bool                  core_spawned;     /* Sector 6 core has been spawned */
} ceres_game_t;

/* Engine Lifecycle Hooks */
bool ceres_game_init(ceres_game_t *g, int w, int h);
void ceres_game_handle_event(ceres_game_t *g, const ceres_event_t *ev);
void ceres_game_update(ceres_game_t *g, uint32_t delta_ms);
void ceres_game_render(ceres_game_t *g, uint16_t *fb, int w, int h, int stride);

#endif /* CERES_GAME_H */
