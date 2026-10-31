/* user/ceres/src/ceres_game.c — Ceres Alpha Game State Machine & Simulation Loop */

#include "ceres_game.h"
#include "ceres_sprites.h"
#include <string.h>

#define SHIP_SPEED 3

static const ceres_target_type_t s_spawn_pattern[] = {
    CERES_TARGET_FUEL_SILO,
    CERES_TARGET_SAM_SILO,
    CERES_TARGET_SURVIVOR,
    CERES_TARGET_RADAR,
    CERES_TARGET_FUEL_SILO,
    CERES_TARGET_CRYO_BUNKER,
    CERES_TARGET_SAM_SILO,
    CERES_TARGET_RADAR,
};

static void get_target_dims(ceres_target_type_t type, int *w, int *h) {
    switch (type) {
        case CERES_TARGET_FUEL_SILO:   *w = SPRITE_FUEL_SILO_W;   *h = SPRITE_FUEL_SILO_H;   break;
        case CERES_TARGET_SAM_SILO:    *w = SPRITE_SAM_SILO_W;    *h = SPRITE_SAM_SILO_H;    break;
        case CERES_TARGET_RADAR:       *w = SPRITE_RADAR_DISH_W;  *h = SPRITE_RADAR_DISH_H;  break;
        case CERES_TARGET_CRYO_BUNKER: *w = SPRITE_CRYO_BUNKER_W; *h = SPRITE_CRYO_BUNKER_H; break;
        case CERES_TARGET_SURVIVOR:    *w = SPRITE_SURVIVOR_W;    *h = SPRITE_SURVIVOR_H;    break;
        default:                       *w = 16;                   *h = 16;                   break;
    }
}

static int find_ground_spawn_site(ceres_game_t *g, uint32_t min_col, uint32_t max_col, int w, int h) {
    if (min_col + w >= max_col) return -1;

    /* First pass: search for a naturally flat plateau (CERES_COL_FLAT) with sufficient clearance */
    for (uint32_t x = min_col; x + w <= max_col; x++) {
        uint32_t r0 = x & CERES_RING_MASK;
        if (!(g->terrain.ring[r0].flags & CERES_COL_FLAT)) continue;
        if (g->terrain.ring[r0].flags & CERES_COL_STRUCTURE) continue;

        uint16_t floor_y = g->terrain.ring[r0].floor_y;
        uint16_t ceil_y = g->terrain.ring[r0].ceil_y;
        if (floor_y - ceil_y < h + 44) continue;

        bool ok = true;
        for (int dx = 0; dx < w; dx++) {
            uint32_t r = (x + dx) & CERES_RING_MASK;
            if (g->terrain.ring[r].floor_y != floor_y || (g->terrain.ring[r].flags & CERES_COL_STRUCTURE)) {
                ok = false;
                x += dx;
                break;
            }
        }
        if (ok) return (int)x;
    }

    /* Second pass: if we have searched > 160 cols without a naturally flat plateau,
     * find the gentlest section and carve a level pad */
    if (max_col - min_col >= 160) {
        for (uint32_t x = min_col; x + w <= max_col; x++) {
            uint32_t r0 = x & CERES_RING_MASK;
            if (g->terrain.ring[r0].flags & CERES_COL_STRUCTURE) continue;
            uint16_t ceil_y = g->terrain.ring[r0].ceil_y;
            uint16_t floor_y = g->terrain.ring[r0].floor_y;
            if (floor_y - ceil_y < h + 44) continue;

            int min_fy = floor_y;
            int max_fy = floor_y;
            for (int dx = 1; dx < w; dx++) {
                uint32_t r = (x + dx) & CERES_RING_MASK;
                int fy = g->terrain.ring[r].floor_y;
                if (fy < min_fy) min_fy = fy;
                if (fy > max_fy) max_fy = fy;
            }
            if (max_fy - min_fy <= 10) {
                return (int)x;
            }
        }
    }

    return -1;
}

static bool spawn_target_at_site(ceres_game_t *g, ceres_target_type_t type, int site) {
    if (site < 0) return false;
    int w = 0, h = 0;
    get_target_dims(type, &w, &h);

    /* Sample highest ground elevation (smallest Y) across footprint to prevent rock clipping */
    int base_y = 360;
    for (int dx = 0; dx < w; dx++) {
        uint32_t r = ((uint32_t)site + dx) & CERES_RING_MASK;
        int fy = (int)g->terrain.ring[r].floor_y;
        if (fy < base_y) base_y = fy;
    }

    /* Level bedrock under the building footprint and guarantee overhead clearance */
    for (int dx = 0; dx < w; dx++) {
        uint32_t r = ((uint32_t)site + dx) & CERES_RING_MASK;
        g->terrain.ring[r].floor_y = (uint16_t)base_y;
        g->terrain.ring[r].flags |= CERES_COL_STRUCTURE;
        if (g->terrain.ring[r].ceil_y > base_y - h - 40) {
            g->terrain.ring[r].ceil_y = (uint16_t)(base_y - h - 40);
        }
    }

    /* Spawn entity at the leveled bedrock base */
    return ceres_entities_spawn_target(&g->entities, type, site, base_y);
}

bool ceres_game_init(ceres_game_t *g, int w, int h) {
    if (!g) return false;
    memset(g, 0, sizeof(*g));

    g->width = (w > 0) ? w : CERES_VIRTUAL_W;
    g->height = (h > 0) ? h : CERES_VIRTUAL_H;
    g->state = CERES_STATE_PLAYING;
    g->running = true;
    g->fps = 60;

    /* Initialize Player */
    g->player.x = 80;
    g->player.y = g->height / 2;
    g->player.bank = 0;
    g->player.fuel = 1000;          /* 100.0% */
    g->player.lives = 3;
    g->player.score = 0;
    g->player.hi_score = 10000;
    g->player.sector = 1;
    g->player.rescued = 0;
    g->player.total_rescued = 3;
    g->player.thruster_on = true;
    g->player.thrust_boost = 0;
    g->player.alive = true;
    g->player.respawn_timer = 0;
    g->player.invincible_frames = 60;
    g->player.tractor_active = false;
    g->player.low_fuel_alert = false;

    /* Unpack 4-Bit Flash Sprites into PSRAM Buffers */
    ceres_sprites_unpack(&g->sprite_bufs, NULL);

    /* Initialize Projectile, Missile & Target Pools */
    ceres_entities_init(&g->entities);

    /* Initialize Streaming Cavern Heightfield Engine */
    ceres_terrain_init(&g->terrain, g->player.sector, 0x19810425);

    /* Pre-spawn initial ground installations across opening landscape on flat plateaus */
    uint32_t search_col = 140;
    for (int i = 0; i < 4; i++) {
        ceres_target_type_t t_type = s_spawn_pattern[g->entities.spawn_cycle % 8];
        int w = 0, h = 0;
        get_target_dims(t_type, &w, &h);

        int site = find_ground_spawn_site(g, search_col, g->terrain.generated_col_end - 40, w, h);
        if (site != -1) {
            spawn_target_at_site(g, t_type, site);
            g->entities.spawn_cycle++;
            search_col = (uint32_t)site + 130;
        } else {
            search_col += 130;
        }
    }
    g->entities.next_spawn_world_x = search_col;

    return true;
}

void ceres_game_handle_event(ceres_game_t *g, const ceres_event_t *ev) {
    if (!g || !ev) return;

    if (ev->type == CERES_EVENT_QUIT) {
        g->running = false;
        return;
    }

    if (ev->type == CERES_EVENT_KEY_DOWN) {
        switch (ev->key) {
            case CERES_KEY_UP:
            case CERES_KEY_W:
                g->key_up = true;
                break;
            case CERES_KEY_DOWN:
            case CERES_KEY_S:
                g->key_down = true;
                break;
            case CERES_KEY_LEFT:
            case CERES_KEY_A:
                g->key_left = true;
                break;
            case CERES_KEY_RIGHT:
            case CERES_KEY_D:
                g->key_right = true;
                break;
            case CERES_KEY_SPACE:
                g->key_fire = true;
                if (g->state == CERES_STATE_GAMEOVER) {
                    /* Restart Game */
                    ceres_game_init(g, g->width, g->height);
                    return;
                }
                if (g->player.alive && ceres_entities_fire_laser(&g->entities, INT_TO_FIXED(g->player.x), INT_TO_FIXED(g->player.y))) {
                    g->player.laser_fired = true;
                    g->player.weapon_flash_ms = 8;
                }
                break;
            case CERES_KEY_B:
            case CERES_KEY_K:
            case CERES_KEY_RETURN:
                g->key_bomb = true;
                if (g->player.alive && ceres_entities_drop_bomb(&g->entities, INT_TO_FIXED(g->player.x), INT_TO_FIXED(g->player.y))) {
                    g->player.bomb_dropped = true;
                    g->player.weapon_flash_ms = 8;
                }
                break;
            case CERES_KEY_R:
                if (g->state == CERES_STATE_GAMEOVER) {
                    ceres_game_init(g, g->width, g->height);
                }
                break;
            case CERES_KEY_P:
                if (g->state == CERES_STATE_PLAYING) {
                    g->state = CERES_STATE_PAUSED;
                } else if (g->state == CERES_STATE_PAUSED) {
                    g->state = CERES_STATE_PLAYING;
                }
                break;
            case CERES_KEY_Q:
            case CERES_KEY_ESCAPE:
                g->running = false;
                break;
            default:
                break;
        }
    } else if (ev->type == CERES_EVENT_KEY_UP) {
        switch (ev->key) {
            case CERES_KEY_UP:
            case CERES_KEY_W:
                g->key_up = false;
                break;
            case CERES_KEY_DOWN:
            case CERES_KEY_S:
                g->key_down = false;
                break;
            case CERES_KEY_LEFT:
            case CERES_KEY_A:
                g->key_left = false;
                break;
            case CERES_KEY_RIGHT:
            case CERES_KEY_D:
                g->key_right = false;
                break;
            case CERES_KEY_SPACE:
                g->key_fire = false;
                break;
            case CERES_KEY_B:
            case CERES_KEY_K:
            case CERES_KEY_RETURN:
                g->key_bomb = false;
                break;
            default:
                break;
        }
    }
}

void ceres_game_update(ceres_game_t *g, uint32_t delta_ms) {
    if (!g || g->state != CERES_STATE_PLAYING) return;

    g->frame_count++;
    g->frames_since_fps++;
    g->last_fps_update += delta_ms;
    if (g->last_fps_update >= 1000) {
        g->fps = (g->frames_since_fps * 1000) / g->last_fps_update;
        g->frames_since_fps = 0;
        g->last_fps_update = 0;
    }

    /* 1. Player Death & Respawn Sequencing */
    if (!g->player.alive) {
        if (g->player.respawn_timer > 0) {
            g->player.respawn_timer--;
            if (g->player.respawn_timer == 0) {
                if (g->player.lives >= 0) {
                    /* Respawn ship at safe mid-screen altitude */
                    g->player.alive = true;
                    g->player.x = 80;
                    g->player.y = g->height / 2;
                    g->player.fuel = 1000;
                    g->player.thruster_on = true;
                    g->player.invincible_frames = 90;
                } else {
                    g->state = CERES_STATE_GAMEOVER;
                }
            }
        }
    }

    g->scroll_x += 2;

    /* 2. Advance Streaming Cavern Terrain & Parallax (2.0 pixels/frame) */
    ceres_terrain_advance(&g->terrain, INT_TO_FIXED(2));

    /* 3. Stream Dynamic Ground Targets ahead of camera onto flat plateaus */
    while (g->entities.next_spawn_world_x + 40 <= g->terrain.generated_col_end) {
        ceres_target_type_t t_type = s_spawn_pattern[g->entities.spawn_cycle % 8];
        int w = 0, h = 0;
        get_target_dims(t_type, &w, &h);

        uint32_t max_search = g->terrain.generated_col_end - 36;
        int site = find_ground_spawn_site(g, g->entities.next_spawn_world_x, max_search, w, h);
        if (site != -1) {
            spawn_target_at_site(g, t_type, site);
            g->entities.spawn_cycle++;
            g->entities.next_spawn_world_x = (uint32_t)site + 130;
        } else {
            /* Wait for camera to advance and generate more columns */
            break;
        }
    }

    /* 4. Player Directional Movement & Flight Physics */
    if (g->player.alive) {
        int dx = 0;
        int dy = 0;
        if (g->key_up)    dy -= SHIP_SPEED;
        if (g->key_down)  dy += SHIP_SPEED;
        if (g->key_left)  dx -= SHIP_SPEED;
        if (g->key_right) dx += SHIP_SPEED;

        /* Unpowered Flame-out Glide when fuel reaches 0 */
        if (g->player.fuel == 0) {
            g->player.thruster_on = false;
            /* Inability to climb; gentle downward gravitational pull */
            if (dy < 0) dy = 0;
            dy += 1;
        } else {
            g->player.thruster_on = true;
        }

        g->player.x += dx;
        g->player.y += dy;

        if (dy < 0) {
            g->player.bank = 1;  /* Climbing */
        } else if (dy > 0) {
            g->player.bank = -1; /* Diving */
        } else {
            g->player.bank = 0;  /* Level flight */
        }

        if (dx > 0) {
            g->player.thrust_boost = 1;  /* Forward boost */
        } else if (dx < 0) {
            g->player.thrust_boost = -1; /* Retro brake */
        } else {
            g->player.thrust_boost = 0;
        }

        /* Horizontal Flight Zone Boundary Clamping */
        int min_x = 32;
        int max_x = g->width - 128;
        if (g->player.x < min_x) g->player.x = min_x;
        if (g->player.x > max_x) g->player.x = max_x;

        /* Dynamic Vertical Clearance Sampling */
        int ceil_y = CERES_HUD_HEIGHT;
        int floor_y = g->height - 12;
        int c0 = 0, f0 = 0, c1 = 0, f1 = 0, c2 = 0, f2 = 0;
        ceres_terrain_sample(&g->terrain, g->player.x, &c0, &f0);
        ceres_terrain_sample(&g->terrain, g->player.x + 10, &c1, &f1);
        ceres_terrain_sample(&g->terrain, g->player.x - 10, &c2, &f2);

        if (c0 > ceil_y) ceil_y = c0;
        if (c1 > ceil_y) ceil_y = c1;
        if (c2 > ceil_y) ceil_y = c2;
        if (f0 < floor_y) floor_y = f0;
        if (f1 < floor_y) floor_y = f1;
        if (f2 < floor_y) floor_y = f2;

        /* Unpowered Fuel Exhaustion Crash */
        if (g->player.fuel == 0 && g->player.y + 6 >= floor_y) {
            g->player.alive = false;
            g->player.respawn_timer = 60;
            g->player.lives--;
            ceres_entities_spawn_explosion(&g->entities, INT_TO_FIXED(g->player.x), INT_TO_FIXED(g->player.y), 2);
            if (g->player.lives < 0) {
                g->state = CERES_STATE_GAMEOVER;
            }
        } else {
            /* Normal flight corridor limits */
            int ship_top_limit = ceil_y + 6;
            int ship_bottom_limit = floor_y - 6;
            if (g->player.y < ship_top_limit) g->player.y = ship_top_limit;
            if (g->player.y > ship_bottom_limit) g->player.y = ship_bottom_limit;
        }

        /* Continuous Fire / Bombing when holding down key */
        if (g->key_fire) {
            if (ceres_entities_fire_laser(&g->entities, INT_TO_FIXED(g->player.x), INT_TO_FIXED(g->player.y))) {
                g->player.laser_fired = true;
                g->player.weapon_flash_ms = 8;
            }
        }
        if (g->key_bomb) {
            if (ceres_entities_drop_bomb(&g->entities, INT_TO_FIXED(g->player.x), INT_TO_FIXED(g->player.y))) {
                g->player.bomb_dropped = true;
                g->player.weapon_flash_ms = 8;
            }
        }

        /* Weapon Flash Countdown */
        if (g->player.weapon_flash_ms > 0) {
            if (delta_ms >= g->player.weapon_flash_ms) {
                g->player.weapon_flash_ms = 0;
                g->player.laser_fired = false;
                g->player.bomb_dropped = false;
            } else {
                g->player.weapon_flash_ms -= delta_ms;
            }
        }

        /* Trailing Thruster Embers */
        if (g->player.thruster_on && (g->frame_count % 6) == 0) {
            ceres_entities_spawn_thruster_ember(&g->entities, INT_TO_FIXED(g->player.x - 9), INT_TO_FIXED(g->player.y));
        }

        /* Steady Fuel Consumption (~12 units / sec at 60 FPS) */
        if ((g->frame_count % 5) == 0 && g->player.fuel > 0) {
            g->player.fuel--;
        }
        g->player.low_fuel_alert = (g->player.fuel <= 200 && g->player.fuel > 0);

        if (g->player.invincible_frames > 0) {
            g->player.invincible_frames--;
        }
    }

    /* 5. Update Projectiles, Bombs, SAMs & Particle Lifecycles */
    ceres_entities_update(&g->entities, &g->terrain, g->width, g->height);

    /* 6. Scan for Low-Altitude Tractor Beam Proximity */
    g->player.tractor_active = false;
    if (g->player.alive) {
        int camera_col = (int)g->terrain.camera_col;
        for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
            const ceres_ground_target_t *t = &g->entities.targets[i];
            if (!t->active || t->type != CERES_TARGET_SURVIVOR) continue;

            int sx = t->world_x - camera_col;
            if (sx < 0 || sx >= g->width) continue;

            int surv_cx = sx + t->w / 2;
            int surv_cy = t->y - t->h / 2;
            int dx = g->player.x - surv_cx;
            if (dx < 0) dx = -dx;

            if (dx <= 32 && g->player.y < surv_cy && (surv_cy - g->player.y) <= 85) {
                g->player.tractor_active = true;
                g->player.tractor_target_x = surv_cx;
                g->player.tractor_target_y = surv_cy;
                break;
            }
        }
    }

    /* 7. Resolve Weapon, Tractor & SAM Collisions */
    ceres_interaction_t res = ceres_entities_resolve_collisions(
        &g->entities, &g->terrain,
        g->player.x - SPRITE_PLAYER_SHIP_W / 2,
        g->player.y - SPRITE_PLAYER_SHIP_H / 2,
        SPRITE_PLAYER_SHIP_W,
        SPRITE_PLAYER_SHIP_H,
        g->player.tractor_active
    );

    /* Process Interaction Results */
    if (res.fuel_delta > 0) {
        g->player.fuel += res.fuel_delta;
        if (g->player.fuel > 1000) g->player.fuel = 1000;
    }
    if (res.score_delta > 0) {
        g->player.score += (uint32_t)res.score_delta;
        if (g->player.score > g->player.hi_score) {
            g->player.hi_score = g->player.score;
        }
    }
    if (res.survivor_rescued) {
        g->player.rescued++;
    }
    if (res.heroic_rescue) {
        g->banner_timer = 90;
        const char *msg = "HEROIC RESCUE! +1000";
        for (int i = 0; i < 31 && msg[i]; i++) {
            g->banner_text[i] = msg[i];
            g->banner_text[i + 1] = '\0';
        }
    }
    if (g->banner_timer > 0) {
        g->banner_timer--;
    }

    /* Fatal SAM Rocket Impact */
    if (res.player_hit && g->player.alive && g->player.invincible_frames == 0) {
        g->player.alive = false;
        g->player.respawn_timer = 60;
        g->player.lives--;
        ceres_entities_spawn_explosion(&g->entities, INT_TO_FIXED(g->player.x), INT_TO_FIXED(g->player.y), 2);
        if (g->player.lives < 0) {
            g->state = CERES_STATE_GAMEOVER;
        }
    }
}

/* Render Ship with Pitch Tilt & Animated Thruster Plume */
static void render_player_ship(const ceres_surface_t *surf, int sx, int sy, int bank, bool thruster, int boost, uint32_t frame) {
    int pitch_dy = 0;
    if (bank > 0) pitch_dy = -1;      /* Nose up */
    else if (bank < 0) pitch_dy = 1;  /* Nose down */

    /* Blit Pre-rendered Sci-Fi Interceptor Sprite */
    int ship_draw_x = sx - SPRITE_PLAYER_SHIP_W / 2;
    int ship_draw_y = sy - SPRITE_PLAYER_SHIP_H / 2 + pitch_dy;
    ceres_blit_sprite(surf, ship_draw_x, ship_draw_y, &g_surf_player_ship, CERES_KEY_COLOR);

    /* Thruster Exhaust Plume (Flickering orange & yellow flame with boost scaling) */
    if (thruster) {
        int base_len = 4;
        if (boost > 0) base_len = 8;
        else if (boost < 0) base_len = 2;

        int flame_len = base_len + (int)(frame % 4);
        uint16_t flame_col = (frame & 1) ? COLOR_THRUSTER_YELLOW : COLOR_THRUSTER_ORANGE;

        int nozzle_x = ship_draw_x - 1;
        int nozzle_y = sy + pitch_dy;

        /* Nozzle white core */
        ceres_blit_pixel(surf, nozzle_x, nozzle_y, COLOR_STAR_WHITE);
        /* Main plume */
        ceres_blit_line(surf, nozzle_x - 1, nozzle_y, nozzle_x - 1 - flame_len, nozzle_y, flame_col);
        ceres_blit_line(surf, nozzle_x - 1, nozzle_y - 1, nozzle_x - flame_len, nozzle_y - 1, COLOR_THRUSTER_ORANGE);
    }
}

void ceres_game_render(ceres_game_t *g, uint16_t *fb, int w, int h, int stride) {
    if (!g || !fb) return;

    ceres_surface_t surf = ceres_surface_make(fb, w, h, stride);

    /* 1. Render Streaming Cavern Heightfield & Parallax Starfield */
    ceres_terrain_render(&g->terrain, &surf, w, h);

    /* 2. Render Search & Rescue Ventral Tractor Beam */
    if (g->player.alive && g->player.tractor_active) {
        int bx = g->player.x;
        int by = g->player.y + 6;
        int tx = g->player.tractor_target_x;
        int ty = g->player.tractor_target_y;

        /* Outer luminous cyan tractor beam cone */
        ceres_blit_line(&surf, bx - 6, by, tx - 8, ty, COLOR_LASER_CYAN);
        ceres_blit_line(&surf, bx + 6, by, tx + 8, ty, COLOR_LASER_CYAN);
        /* Inner pulsing white energy shaft */
        ceres_blit_line(&surf, bx, by, tx, ty, (g->frame_count & 2) ? COLOR_STAR_WHITE : COLOR_SURVIVOR_GREEN);
    }

    /* 3. Render Ground Installations, SAM Rockets, Projectiles & Particles */
    ceres_entities_render(&g->entities, &g->terrain, &surf);

    /* 4. Render Player Craft (with Invulnerability Flash) */
    if (g->player.alive) {
        bool flash_hide = (g->player.invincible_frames > 0) && ((g->player.invincible_frames / 4) & 1);
        if (!flash_hide) {
            render_player_ship(&surf, g->player.x, g->player.y, g->player.bank,
                               g->player.thruster_on, g->player.thrust_boost, g->frame_count);
        }
    }

    /* 5. Arcade HUD Header */
    ceres_blit_fill_rect(&surf, 0, 0, w, 32, RGB565(12, 12, 20));
    ceres_blit_line(&surf, 0, 31, w - 1, 31, RGB565(60, 60, 90));

    /* Row 1: Score, Hi-Score, Sector, Ships */
    ceres_blit_text_fmt(&surf, 8, 4, COLOR_HUD_TEXT, 0, true,
                           "SCORE: %06u   HI: %06u   SECTOR: %d   SHIPS: ",
                           g->player.score, g->player.hi_score, g->player.sector);

    /* Ship Icons in HUD */
    int ship_icon_x = 420;
    for (int i = 0; i < g->player.lives; i++) {
        ceres_blit_text(&surf, ship_icon_x + i * 24, 4, "[>", COLOR_SHIP_COCKPIT, 0, true);
    }

    /* Live Frame Rate in HUD */
    ceres_blit_text_fmt(&surf, w - 80, 4, COLOR_STAR_DIM, 0, true, "%2u FPS", g->fps);

    /* Row 2: Fuel Gauge Bar and Rescue Counter */
    ceres_blit_text(&surf, 8, 18, "FUEL [", COLOR_HUD_TEXT, 0, true);

    /* Temporary In-Game Notification Banner */
    if (g->banner_timer > 0 && g->state == CERES_STATE_PLAYING) {
        int bw = 200;
        int bh = 22;
        int bx = (w - bw) / 2;
        int by = 36;
        ceres_blit_fill_rect(&surf, bx, by, bw, bh, RGB565(8, 24, 8));
        ceres_blit_rect_outline(&surf, bx, by, bw, bh, COLOR_SURVIVOR_GREEN);
        ceres_blit_text(&surf, bx + 16, by + 3, g->banner_text, COLOR_STAR_WHITE, 0, true);
    }

    int fuel_bar_x = 8 + 6 * CERES_FONT_W;
    int fuel_bar_w = 120;
    int fuel_fill = (g->player.fuel * fuel_bar_w) / 1000;
    if (fuel_fill < 0) fuel_fill = 0;
    if (fuel_fill > fuel_bar_w) fuel_fill = fuel_bar_w;

    uint16_t fuel_color;
    if (g->player.fuel > 500) {
        fuel_color = COLOR_FUEL_BLUE;
    } else if (g->player.fuel > 200) {
        fuel_color = RGB565(255, 180, 0); /* Amber warning */
    } else {
        /* Flashing Alert Red */
        fuel_color = ((g->frame_count / 15) & 1) ? COLOR_HUD_ALERT : RGB565(80, 10, 10);
    }

    ceres_blit_fill_rect(&surf, fuel_bar_x, 20, fuel_fill, 10, fuel_color);
    ceres_blit_rect_outline(&surf, fuel_bar_x, 20, fuel_bar_w, 10, COLOR_STAR_DIM);
    ceres_blit_text(&surf, fuel_bar_x + fuel_bar_w + 4, 18, "]", COLOR_HUD_TEXT, 0, true);

    /* Fuel Alert Badges */
    if (g->player.fuel == 0) {
        ceres_blit_text(&surf, fuel_bar_x + fuel_bar_w + 14, 18, "[NO FUEL - GLIDE!]", COLOR_HUD_ALERT, 0, true);
    } else if (g->player.low_fuel_alert && ((g->frame_count / 15) & 1)) {
        ceres_blit_text(&surf, fuel_bar_x + fuel_bar_w + 14, 18, "[LOW FUEL]", COLOR_HUD_ALERT, 0, true);
    }

    /* Rescue Status in HUD */
    ceres_blit_text_fmt(&surf, 320, 18,
                           COLOR_SURVIVOR_GREEN, 0, true,
                           "RESCUED: %d/%d", g->player.rescued, g->player.total_rescued);

    /* Live Projectile Counters in HUD */
    ceres_blit_text_fmt(&surf, w - 210, 18, COLOR_LASER_CYAN, 0, true,
                           "LASER: [SPC] %u", g->entities.active_lasers);
    ceres_blit_text_fmt(&surf, w - 90, 18, COLOR_BOMB_YELLOW, 0, true,
                           "BOMB: %u/2", g->entities.active_bombs);

    /* 6. Game Over Overlay */
    if (g->state == CERES_STATE_GAMEOVER) {
        int gw = 260;
        int gh = 64;
        int gx = (w - gw) / 2;
        int gy = (h - gh) / 2;
        ceres_blit_fill_rect(&surf, gx, gy, gw, gh, RGB565(16, 8, 8));
        ceres_blit_rect_outline(&surf, gx, gy, gw, gh, COLOR_HUD_ALERT);
        ceres_blit_text(&surf, gx + 80, gy + 16, "GAME OVER", COLOR_HUD_ALERT, 0, true);
        ceres_blit_text(&surf, gx + 20, gy + 38, "PRESS SPACE OR 'R' TO RETRY", COLOR_STAR_WHITE, 0, true);
    } else if (g->state == CERES_STATE_PAUSED) {
        int pw = 336;
        int ph = 78;
        int px = (w - pw) / 2;
        int py = (h - ph) / 2;
        ceres_blit_fill_rect(&surf, px, py, pw, ph, RGB565(8, 8, 16));
        ceres_blit_rect_outline(&surf, px, py, pw, ph, COLOR_LASER_CYAN);
        ceres_blit_text(&surf, px + 120, py + 8, "== PAUSED ==", COLOR_STAR_WHITE, 0, true);
        ceres_blit_text(&surf, px + 24, py + 26, "MOVE: ARROWS / WASD", COLOR_HUD_TEXT, 0, true);
        ceres_blit_text(&surf, px + 24, py + 42, "FIRE: SPACE (LASER)  B/ENTER (BOMB)", COLOR_HUD_TEXT, 0, true);
        ceres_blit_text(&surf, px + 24, py + 58, "P: RESUME   R: RETRY   Q: QUIT", COLOR_STAR_DIM, 0, true);
    }
}
