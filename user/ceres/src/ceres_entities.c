/* user/ceres/src/ceres_entities.c — Entity Pools, Projectile Kinematics & Ground Targets */

#include "ceres_entities.h"
#include "ceres_sprites.h"
#include <string.h>

/* Fast deterministic PRNG without static state */
static inline uint32_t prng_step(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

void ceres_entities_init(ceres_entities_t *pool) {
    if (!pool) return;
    memset(pool, 0, sizeof(*pool));
    pool->next_spawn_world_x = 240;
    pool->spawn_cycle = 0;
    pool->aerial_spawn_timer = 90;
    pool->aerial_formation_cycle = 0;
}

bool ceres_entities_spawn_target(ceres_entities_t *pool, ceres_target_type_t type, int32_t world_x, int32_t ground_y) {
    if (!pool || type == CERES_TARGET_NONE) return false;

    int slot = -1;
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        if (!pool->targets[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return false;

    ceres_ground_target_t *t = &pool->targets[slot];
    t->active = true;
    t->type = type;
    t->world_x = world_x;
    t->y = ground_y;
    t->launched = false;
    t->cracked = false;
    t->anim_timer = 0;

    switch (type) {
        case CERES_TARGET_FUEL_SILO:
            t->w = SPRITE_FUEL_SILO_W;
            t->h = SPRITE_FUEL_SILO_H;
            break;
        case CERES_TARGET_RADAR:
            t->w = SPRITE_RADAR_DISH_W;
            t->h = SPRITE_RADAR_DISH_H;
            break;
        case CERES_TARGET_SAM_SILO:
            t->w = SPRITE_SAM_SILO_W;
            t->h = SPRITE_SAM_SILO_H;
            break;
        case CERES_TARGET_CRYO_BUNKER:
            t->w = SPRITE_CRYO_BUNKER_W;
            t->h = SPRITE_CRYO_BUNKER_H;
            break;
        case CERES_TARGET_SURVIVOR:
            t->w = SPRITE_SURVIVOR_W;
            t->h = SPRITE_SURVIVOR_H;
            break;
        default:
            t->w = 16;
            t->h = 16;
            break;
    }

    pool->active_targets++;
    return true;
}

bool ceres_entities_launch_sam(ceres_entities_t *pool, fixed32_t launch_x, fixed32_t launch_y) {
    if (!pool) return false;

    int slot = -1;
    for (int i = 0; i < CERES_MAX_SAM_ROCKETS; i++) {
        if (!pool->sam_rockets[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return false;

    ceres_sam_rocket_t *r = &pool->sam_rockets[slot];
    r->active = true;
    r->x = launch_x;
    r->y = launch_y;
    r->vx = -FIXED_ONE / 4;          /* Slight drift left with scroll */
    r->vy = SAM_INITIAL_VY_FIXED;     /* Initial upward burst */
    r->life_frames = 110;

    /* Launch blast smoke and flame */
    ceres_entities_spawn_explosion(pool, launch_x + INT_TO_FIXED(6), launch_y + INT_TO_FIXED(12), 0);
    pool->active_sam_rockets++;
    pool->total_sams_launched++;
    return true;
}

bool ceres_entities_spawn_aerial(ceres_entities_t *pool, ceres_aerial_type_t type, fixed32_t x, fixed32_t y) {
    if (!pool || type == CERES_AERIAL_NONE) return false;

    int slot = -1;
    for (int i = 0; i < CERES_MAX_AERIAL_ENEMIES; i++) {
        if (!pool->aerials[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return false;

    ceres_aerial_t *a = &pool->aerials[slot];
    a->active = true;
    a->type = type;
    a->x = x;
    a->y = y;
    a->base_y = y;
    a->anim_timer = 0;
    a->carrying_survivor = false;
    a->target_survivor_idx = -1;

    switch (type) {
        case CERES_AERIAL_SAUCER:
            a->w = SPRITE_SAUCER_W;
            a->h = SPRITE_SAUCER_H;
            a->vx = -INT_TO_FIXED(3);
            a->vy = 0;
            a->hp = 1;
            break;
        case CERES_AERIAL_FIGHTER:
            a->w = SPRITE_FIGHTER_W;
            a->h = SPRITE_FIGHTER_H;
            a->vx = -INT_TO_FIXED(4);
            a->vy = INT_TO_FIXED(1);
            a->hp = 1;
            break;
        case CERES_AERIAL_METEOR:
            a->w = SPRITE_METEOR_W;
            a->h = SPRITE_METEOR_H;
            a->vx = -INT_TO_FIXED(2) - (FIXED_ONE / 2);
            a->vy = FIXED_ONE / 4;
            a->hp = 2;
            break;
        case CERES_AERIAL_HARVESTER:
            a->w = SPRITE_HARVESTER_W;
            a->h = SPRITE_HARVESTER_H;
            a->vx = -INT_TO_FIXED(2);
            a->vy = 0;
            a->hp = 3;
            break;
        case CERES_AERIAL_PARACHUTE:
            a->w = SPRITE_PARACHUTIST_W;
            a->h = SPRITE_PARACHUTIST_H;
            a->vx = -FIXED_ONE / 2;
            a->vy = FIXED_ONE / 2;
            a->hp = 1;
            break;
        default:
            a->w = 16;
            a->h = 16;
            a->vx = -INT_TO_FIXED(2);
            a->vy = 0;
            a->hp = 1;
            break;
    }

    pool->active_aerials++;
    return true;
}

void ceres_entities_spawn_saucer_wave(ceres_entities_t *pool, fixed32_t start_x, fixed32_t base_y, int count) {
    if (!pool || count <= 0) return;
    for (int i = 0; i < count; i++) {
        fixed32_t sx = start_x + INT_TO_FIXED(i * 30);
        if (ceres_entities_spawn_aerial(pool, CERES_AERIAL_SAUCER, sx, base_y)) {
            for (int s = 0; s < CERES_MAX_AERIAL_ENEMIES; s++) {
                if (pool->aerials[s].active && pool->aerials[s].type == CERES_AERIAL_SAUCER && pool->aerials[s].x == sx) {
                    pool->aerials[s].anim_timer = (uint8_t)(i * 20);
                    break;
                }
            }
        }
    }
}

void ceres_entities_spawn_fighter(ceres_entities_t *pool, fixed32_t start_x, fixed32_t start_y) {
    ceres_entities_spawn_aerial(pool, CERES_AERIAL_FIGHTER, start_x, start_y);
}

void ceres_entities_spawn_meteor(ceres_entities_t *pool, fixed32_t start_x, fixed32_t start_y) {
    ceres_entities_spawn_aerial(pool, CERES_AERIAL_METEOR, start_x, start_y);
}

void ceres_entities_spawn_harvester(ceres_entities_t *pool, fixed32_t start_x, fixed32_t start_y) {
    ceres_entities_spawn_aerial(pool, CERES_AERIAL_HARVESTER, start_x, start_y);
}

bool ceres_entities_fire_laser(ceres_entities_t *pool, fixed32_t ship_x, fixed32_t ship_y) {
    if (!pool) return false;
    if (pool->laser_cooldown > 0) return false;

    /* Find 2 free laser slots for simultaneous twin pulse */
    int slot1 = -1;
    int slot2 = -1;
    for (int i = 0; i < CERES_MAX_LASERS; i++) {
        if (!pool->lasers[i].active) {
            if (slot1 == -1) slot1 = i;
            else if (slot2 == -1) { slot2 = i; break; }
        }
    }

    if (slot1 == -1 || slot2 == -1) return false;

    /* Upper Laser Beam */
    pool->lasers[slot1].active = true;
    pool->lasers[slot1].x = ship_x + INT_TO_FIXED(8);
    pool->lasers[slot1].y = ship_y - INT_TO_FIXED(3);
    pool->lasers[slot1].vx = LASER_SPEED_FIXED;
    pool->lasers[slot1].life_frames = 60;

    /* Lower Laser Beam */
    pool->lasers[slot2].active = true;
    pool->lasers[slot2].x = ship_x + INT_TO_FIXED(8);
    pool->lasers[slot2].y = ship_y + INT_TO_FIXED(3);
    pool->lasers[slot2].vx = LASER_SPEED_FIXED;
    pool->lasers[slot2].life_frames = 60;

    pool->laser_cooldown = CERES_LASER_COOLDOWN;
    pool->total_lasers_fired += 2;
    return true;
}

bool ceres_entities_drop_bomb(ceres_entities_t *pool, fixed32_t ship_x, fixed32_t ship_y) {
    if (!pool) return false;
    if (pool->bomb_cooldown > 0) return false;

    /* Arcade constraint: max 2 active bombs concurrently */
    if (pool->active_bombs >= 2) return false;

    int slot = -1;
    for (int i = 0; i < CERES_MAX_BOMBS; i++) {
        if (!pool->bombs[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return false;

    pool->bombs[slot].active = true;
    pool->bombs[slot].x = ship_x + INT_TO_FIXED(2);
    pool->bombs[slot].y = ship_y + INT_TO_FIXED(5);
    pool->bombs[slot].vx = BOMB_INITIAL_VX_FIXED;
    pool->bombs[slot].vy = BOMB_INITIAL_VY_FIXED;
    pool->bombs[slot].life_frames = 90;

    pool->bomb_cooldown = CERES_BOMB_COOLDOWN;
    pool->total_bombs_dropped++;
    return true;
}

void ceres_entities_spawn_sparks(ceres_entities_t *pool, fixed32_t x, fixed32_t y, uint16_t color, int count) {
    if (!pool || count <= 0) return;

    uint32_t seed = (uint32_t)x ^ ((uint32_t)y << 5) ^ (pool->total_bomb_detonations << 11);
    for (int i = 0; i < count; i++) {
        int slot = -1;
        for (int p = 0; p < CERES_MAX_PARTICLES; p++) {
            if (!pool->particles[p].active) {
                slot = p;
                break;
            }
        }
        if (slot == -1) break;

        uint32_t r1 = prng_step(&seed);
        uint32_t r2 = prng_step(&seed);

        int vx_raw = (int)(r1 % 500) - 250; /* -2.5 .. +2.5 */
        int vy_raw = (int)(r2 % 500) - 250;

        pool->particles[slot].active = true;
        pool->particles[slot].x = x;
        pool->particles[slot].y = y;
        pool->particles[slot].vx = (vx_raw * FIXED_ONE) / 100;
        pool->particles[slot].vy = (vy_raw * FIXED_ONE) / 100;
        pool->particles[slot].color = color;
        pool->particles[slot].life = 8 + (uint8_t)(r1 % 8);
        pool->particles[slot].max_life = pool->particles[slot].life;
    }
}

void ceres_entities_spawn_explosion(ceres_entities_t *pool, fixed32_t x, fixed32_t y, int size_preset) {
    if (!pool) return;
    int count = (size_preset > 0) ? 16 : 8;

    uint32_t seed = (uint32_t)x ^ ((uint32_t)y << 7) ^ 0xDEADBEEFu;
    for (int i = 0; i < count; i++) {
        int slot = -1;
        for (int p = 0; p < CERES_MAX_PARTICLES; p++) {
            if (!pool->particles[p].active) {
                slot = p;
                break;
            }
        }
        if (slot == -1) break;

        uint32_t r = prng_step(&seed);
        uint8_t angle = (uint8_t)(i * (256 / count) + (r & 7));
        fixed32_t speed = INT_TO_FIXED(1) + (fixed32_t)((r & 0x7FFF) % INT_TO_FIXED(2));

        pool->particles[slot].active = true;
        pool->particles[slot].x = x;
        pool->particles[slot].y = y;
        pool->particles[slot].vx = fixed_mul(fixed_cos(angle), speed);
        pool->particles[slot].vy = fixed_mul(fixed_sin(angle), speed);

        /* Flame color gradation based on index */
        uint16_t c;
        if ((i & 3) == 0)      c = COLOR_STAR_WHITE;
        else if ((i & 3) == 1) c = COLOR_THRUSTER_YELLOW;
        else if ((i & 3) == 2) c = COLOR_EXPLOSION_ORANGE;
        else                   c = COLOR_ENEMY_UFO_RED;

        pool->particles[slot].color = c;
        pool->particles[slot].life = 12 + (uint8_t)(r % 12);
        pool->particles[slot].max_life = pool->particles[slot].life;
    }
}

void ceres_entities_spawn_thruster_ember(ceres_entities_t *pool, fixed32_t x, fixed32_t y) {
    if (!pool) return;

    int slot = -1;
    for (int p = 0; p < CERES_MAX_PARTICLES; p++) {
        if (!pool->particles[p].active) {
            slot = p;
            break;
        }
    }
    if (slot == -1) return;

    uint32_t seed = (uint32_t)x ^ (uint32_t)y;
    uint32_t r = prng_step(&seed);

    pool->particles[slot].active = true;
    pool->particles[slot].x = x;
    pool->particles[slot].y = y;
    pool->particles[slot].vx = -INT_TO_FIXED(2) - (fixed32_t)((r & 0x3FFF));
    pool->particles[slot].vy = (fixed32_t)((int)(r % 400) - 200) * (FIXED_ONE / 400);
    pool->particles[slot].color = (r & 1) ? COLOR_THRUSTER_ORANGE : COLOR_THRUSTER_YELLOW;
    pool->particles[slot].life = 6 + (uint8_t)(r % 6);
    pool->particles[slot].max_life = pool->particles[slot].life;
}

void ceres_entities_update(ceres_entities_t *pool, const ceres_terrain_t *terrain, int playfield_w, int playfield_h) {
    if (!pool) return;

    if (pool->laser_cooldown > 0) pool->laser_cooldown--;
    if (pool->bomb_cooldown > 0) pool->bomb_cooldown--;

    /* 1. Update Lasers */
    pool->active_lasers = 0;
    for (int i = 0; i < CERES_MAX_LASERS; i++) {
        ceres_laser_t *laser = &pool->lasers[i];
        if (!laser->active) continue;

        laser->x += laser->vx;
        laser->life_frames--;

        int tip_x = FIXED_TO_INT(laser->x) + LASER_SPAN_PX;
        int tip_y = FIXED_TO_INT(laser->y);

        if (laser->life_frames <= 0 || tip_x >= playfield_w || tip_x < 0) {
            laser->active = false;
            continue;
        }

        /* Terrain collision check */
        if (terrain) {
            int ceil_y = 0;
            int floor_y = playfield_h;
            ceres_terrain_sample(terrain, tip_x, &ceil_y, &floor_y);

            /* Check if tip is hitting an active target or SAM before rock impact */
            bool hitting_entity = false;
            int camera_col = (int)terrain->camera_col;
            for (int t_idx = 0; t_idx < CERES_MAX_GROUND_TARGETS; t_idx++) {
                const ceres_ground_target_t *tgt = &pool->targets[t_idx];
                if (!tgt->active || tgt->type == CERES_TARGET_SURVIVOR) continue;
                int sx = tgt->world_x - camera_col;
                if (tip_x >= sx && tip_x <= sx + tgt->w && tip_y >= tgt->y - tgt->h && tip_y <= tgt->y) {
                    hitting_entity = true;
                    break;
                }
            }
            if (!hitting_entity) {
                for (int s_idx = 0; s_idx < CERES_MAX_SAM_ROCKETS; s_idx++) {
                    const ceres_sam_rocket_t *sam = &pool->sam_rockets[s_idx];
                    if (!sam->active) continue;
                    int rx = FIXED_TO_INT(sam->x);
                    int ry = FIXED_TO_INT(sam->y);
                    if (tip_x >= rx && tip_x <= rx + SPRITE_SAM_ROCKET_W && tip_y >= ry && tip_y <= ry + SPRITE_SAM_ROCKET_H) {
                        hitting_entity = true;
                        break;
                    }
                }
            }

            if (!hitting_entity && (tip_y <= ceil_y || tip_y >= floor_y)) {
                laser->active = false;
                pool->total_laser_rock_hits++;
                ceres_entities_spawn_sparks(pool, INT_TO_FIXED(tip_x), INT_TO_FIXED(tip_y), COLOR_LASER_CYAN, 3);
                continue;
            }
        }

        pool->active_lasers++;
    }

    /* 2. Update Bombs */
    pool->active_bombs = 0;
    for (int i = 0; i < CERES_MAX_BOMBS; i++) {
        ceres_bomb_t *bomb = &pool->bombs[i];
        if (!bomb->active) continue;

        /* Parabolic Acceleration: v_{y, t+1} = v_{y, t} + g */
        bomb->vy += BOMB_GRAVITY_FIXED;
        if (bomb->vy > BOMB_MAX_VY_FIXED) {
            bomb->vy = BOMB_MAX_VY_FIXED;
        }

        bomb->x += bomb->vx;
        bomb->y += bomb->vy;
        bomb->life_frames--;

        int bx = FIXED_TO_INT(bomb->x);
        int by = FIXED_TO_INT(bomb->y);

        if (bomb->life_frames <= 0 || bx < 0 || bx >= playfield_w || by >= playfield_h) {
            bomb->active = false;
            continue;
        }

        /* Terrain collision check */
        if (terrain) {
            int ceil_y = 0;
            int floor_y = playfield_h;
            ceres_terrain_sample(terrain, bx, &ceil_y, &floor_y);

            /* Check if bomb is hitting an active ground target before bedrock impact */
            bool hitting_target = false;
            int camera_col = (int)terrain->camera_col;
            for (int t_idx = 0; t_idx < CERES_MAX_GROUND_TARGETS; t_idx++) {
                const ceres_ground_target_t *tgt = &pool->targets[t_idx];
                if (!tgt->active || tgt->type == CERES_TARGET_SURVIVOR) continue;
                int sx = tgt->world_x - camera_col;
                if (bx >= sx && bx <= sx + tgt->w && by >= tgt->y - tgt->h - 4 && by <= tgt->y + 4) {
                    hitting_target = true;
                    break;
                }
            }

            if (!hitting_target) {
                if (by + BOMB_RADIUS_PX >= floor_y) {
                    /* Floor Detonation */
                    bomb->active = false;
                    pool->total_bomb_detonations++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(bx), INT_TO_FIXED(floor_y), 1);
                    continue;
                } else if (by - BOMB_RADIUS_PX <= ceil_y) {
                    /* Ceiling Detonation (Stalactite impact) */
                    bomb->active = false;
                    pool->total_bomb_detonations++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(bx), INT_TO_FIXED(ceil_y), 1);
                    continue;
                }
            }
        }

        pool->active_bombs++;
    }

    /* 3. Update Surface-to-Air Rockets (SAM) */
    pool->active_sam_rockets = 0;
    for (int i = 0; i < CERES_MAX_SAM_ROCKETS; i++) {
        ceres_sam_rocket_t *sam = &pool->sam_rockets[i];
        if (!sam->active) continue;

        sam->vy += SAM_ACCEL_VY_FIXED;
        if (sam->vy < SAM_MAX_VY_FIXED) {
            sam->vy = SAM_MAX_VY_FIXED;
        }

        sam->x += sam->vx;
        sam->y += sam->vy;
        sam->life_frames--;

        int sx = FIXED_TO_INT(sam->x);
        int sy = FIXED_TO_INT(sam->y);

        /* Trail rocket exhaust embers */
        if ((sam->life_frames & 1) == 0) {
            ceres_entities_spawn_sparks(pool, sam->x + INT_TO_FIXED(6), sam->y + INT_TO_FIXED(20), COLOR_THRUSTER_ORANGE, 1);
        }

        if (sam->life_frames <= 0 || sx < -20 || sx >= playfield_w + 20 || sy < CERES_HUD_HEIGHT) {
            sam->active = false;
            if (sy <= CERES_HUD_HEIGHT) {
                ceres_entities_spawn_explosion(pool, sam->x + INT_TO_FIXED(6), sam->y, 0);
            }
            continue;
        }

        /* Terrain ceiling check */
        if (terrain && sx >= 0 && sx < playfield_w) {
            int ceil_y = 0;
            ceres_terrain_sample(terrain, sx + 6, &ceil_y, NULL);
            if (sy <= ceil_y) {
                sam->active = false;
                ceres_entities_spawn_explosion(pool, sam->x + INT_TO_FIXED(6), INT_TO_FIXED(ceil_y), 0);
                continue;
            }
        }

        pool->active_sam_rockets++;
    }

    /* 4. Update Ground Target Lifecycles */
    pool->active_targets = 0;
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        ceres_ground_target_t *t = &pool->targets[i];
        if (!t->active) continue;

        t->anim_timer++;

        /* Check if target scrolled off screen left */
        if (terrain) {
            int sx = t->world_x - (int)terrain->camera_col;
            if (sx < -48) {
                t->active = false;
                continue;
            }
        }

        pool->active_targets++;
    }

    /* 5. Update Aerial Enemies */
    pool->active_aerials = 0;
    int camera_col = terrain ? (int)terrain->camera_col : 0;

    for (int a_idx = 0; a_idx < CERES_MAX_AERIAL_ENEMIES; a_idx++) {
        ceres_aerial_t *a = &pool->aerials[a_idx];
        if (!a->active) continue;

        a->anim_timer++;

        switch (a->type) {
            case CERES_AERIAL_SAUCER: {
                a->x += a->vx;
                /* Undulate with smooth sine wave */
                uint8_t phase = (uint8_t)(a->anim_timer * 3);
                a->y = a->base_y + fixed_mul(INT_TO_FIXED(22), fixed_sin(phase));
                break;
            }
            case CERES_AERIAL_FIGHTER: {
                a->x += a->vx;
                a->y += a->vy;
                /* Pull up from dive if reaching lower zone */
                if (a->y > a->base_y + INT_TO_FIXED(70)) {
                    a->vy = -FIXED_ONE / 2;
                }
                break;
            }
            case CERES_AERIAL_METEOR: {
                a->x += a->vx;
                a->y += a->vy;
                /* Occasional dust particle */
                if ((a->anim_timer % 8) == 0) {
                    ceres_entities_spawn_sparks(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h / 2), COLOR_METEOR_BROWN, 1);
                }
                break;
            }
            case CERES_AERIAL_HARVESTER: {
                if (!a->carrying_survivor) {
                    /* Look for a ground survivor to abduct */
                    int nearest_surv = -1;
                    int min_dx = 9999;
                    for (int t_idx = 0; t_idx < CERES_MAX_GROUND_TARGETS; t_idx++) {
                        const ceres_ground_target_t *tgt = &pool->targets[t_idx];
                        if (!tgt->active || tgt->type != CERES_TARGET_SURVIVOR) continue;
                        int sx = tgt->world_x - camera_col;
                        if (sx < 0 || sx >= playfield_w) continue;
                        int dx = FIXED_TO_INT(a->x) - sx;
                        if (dx < 0) dx = -dx;
                        if (dx < min_dx) {
                            min_dx = dx;
                            nearest_surv = t_idx;
                        }
                    }

                    if (nearest_surv != -1) {
                        ceres_ground_target_t *tgt = &pool->targets[nearest_surv];
                        int sx = tgt->world_x - camera_col;
                        int surv_y = tgt->y;

                        if (min_dx <= 24) {
                            /* Vertically aligned: descend with abduction beam */
                            a->vx = 0;
                            a->vy = INT_TO_FIXED(2);
                            a->x = INT_TO_FIXED(sx + tgt->w / 2 - a->w / 2);
                            a->y += a->vy;

                            if (FIXED_TO_INT(a->y) + a->h >= surv_y - 4) {
                                /* Grab survivor! */
                                a->carrying_survivor = true;
                                tgt->active = false;
                                pool->active_targets--;
                                a->vy = -INT_TO_FIXED(2); /* Ascend with victim! */
                                ceres_entities_spawn_sparks(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h), COLOR_THRUSTER_YELLOW, 8);
                            }
                        } else {
                            /* Move horizontally towards survivor */
                            a->vx = (FIXED_TO_INT(a->x) > sx) ? -INT_TO_FIXED(2) : INT_TO_FIXED(2);
                            a->vy = 0;
                            a->x += a->vx;
                        }
                    } else {
                        /* Normal patrol flight */
                        a->x += a->vx;
                    }
                } else {
                    /* Ascend towards space with abducted human */
                    a->y += a->vy;
                    if (a->y < -INT_TO_FIXED(24)) {
                        /* Escaped! Victim lost */
                        a->active = false;
                        continue;
                    }
                }
                break;
            }
            case CERES_AERIAL_PARACHUTE: {
                a->x += a->vx;
                a->y += a->vy;

                /* Check if safely landed on ground floor */
                if (terrain) {
                    int px = FIXED_TO_INT(a->x) + a->w / 2;
                    int floor_y = playfield_h;
                    if (px >= 0 && px < playfield_w) {
                        ceres_terrain_sample(terrain, px, NULL, &floor_y);
                    }
                    if (FIXED_TO_INT(a->y) + a->h >= floor_y) {
                        /* Safe landing: turns into a ground survivor waiting for rescue! */
                        ceres_entities_spawn_target(pool, CERES_TARGET_SURVIVOR, camera_col + px - SPRITE_SURVIVOR_W / 2, floor_y);
                        a->active = false;
                        ceres_entities_spawn_sparks(pool, a->x + INT_TO_FIXED(a->w / 2), INT_TO_FIXED(floor_y), COLOR_SURVIVOR_GREEN, 4);
                        continue;
                    }
                }
                break;
            }
            default:
                a->x += a->vx;
                break;
        }

        int ax = FIXED_TO_INT(a->x);
        int ay = FIXED_TO_INT(a->y);

        if (ax < -48 || ax > playfield_w + 140 || ay < -40 || ay > playfield_h + 40) {
            a->active = false;
            continue;
        }

        pool->active_aerials++;
    }

    /* 6. Aerial Enemy Formation Spawner */
    if (pool->aerial_spawn_timer > 0) {
        pool->aerial_spawn_timer--;
    } else {
        pool->aerial_spawn_timer = 200; /* Spawn formation every ~3.3 seconds */
        uint8_t wave = pool->aerial_formation_cycle % 4;
        pool->aerial_formation_cycle++;

        int spawn_x = playfield_w + 40;
        int spawn_y = 60 + (int)(prng_step(&pool->next_spawn_world_x) % 180);

        if (wave == 0) {
            /* Undulating Saucer Squadron (4 saucers) */
            ceres_entities_spawn_saucer_wave(pool, INT_TO_FIXED(spawn_x), INT_TO_FIXED(spawn_y), 4);
        } else if (wave == 1) {
            /* Fast Diving Interceptor */
            ceres_entities_spawn_fighter(pool, INT_TO_FIXED(spawn_x), INT_TO_FIXED(spawn_y - 30));
        } else if (wave == 2) {
            /* Alien Harvester Abductor */
            ceres_entities_spawn_harvester(pool, INT_TO_FIXED(spawn_x), INT_TO_FIXED(50));
        } else {
            /* Tumbling Basalt Meteor */
            ceres_entities_spawn_meteor(pool, INT_TO_FIXED(spawn_x), INT_TO_FIXED(spawn_y));
        }
    }

    /* 7. Update Particles */
    pool->active_particles = 0;
    for (int p = 0; p < CERES_MAX_PARTICLES; p++) {
        ceres_particle_t *part = &pool->particles[p];
        if (!part->active) continue;

        part->x += part->vx;
        part->y += part->vy;

        /* Slight drag & gravity */
        part->vy += (FIXED_ONE / 24);
        part->vx -= (FIXED_ONE / 8); /* Drift leftward with terrain scroll */

        if (part->life > 0) part->life--;
        if (part->life == 0) {
            part->active = false;
            continue;
        }

        int px = FIXED_TO_INT(part->x);
        int py = FIXED_TO_INT(part->y);
        if (px < 0 || px >= playfield_w || py < 0 || py >= playfield_h) {
            part->active = false;
            continue;
        }

        pool->active_particles++;
    }
}

ceres_interaction_t ceres_entities_resolve_collisions(ceres_entities_t *pool, const ceres_terrain_t *terrain,
                                                     int ship_x, int ship_y, int ship_w, int ship_h,
                                                     bool tractor_active) {
    ceres_interaction_t res;
    memset(&res, 0, sizeof(res));
    if (!pool || !terrain) return res;

    int camera_col = (int)terrain->camera_col;

    /* 1. Proximity Trigger: SAM Silos launch vertically as player approaches */
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        ceres_ground_target_t *t = &pool->targets[i];
        if (!t->active || t->type != CERES_TARGET_SAM_SILO || t->launched) continue;

        int sx = t->world_x - camera_col;
        int dist = sx - ship_x;
        if (dist > 0 && dist <= SAM_TRIGGER_DIST_PX) {
            t->launched = true;
            ceres_entities_launch_sam(pool, INT_TO_FIXED(sx + t->w / 2 - SPRITE_SAM_ROCKET_W / 2),
                                      INT_TO_FIXED(t->y - t->h));
            res.sam_launched = true;
        }
    }

    /* 2. Weapon vs Airborne SAM Rockets */
    for (int s = 0; s < CERES_MAX_SAM_ROCKETS; s++) {
        ceres_sam_rocket_t *sam = &pool->sam_rockets[s];
        if (!sam->active) continue;

        int rx = FIXED_TO_INT(sam->x);
        int ry = FIXED_TO_INT(sam->y);

        /* Laser collision with SAM Rocket */
        for (int l = 0; l < CERES_MAX_LASERS; l++) {
            ceres_laser_t *laser = &pool->lasers[l];
            if (!laser->active) continue;

            int laser_left = FIXED_TO_INT(laser->x);
            int laser_right = laser_left + LASER_SPAN_PX;
            int ly = FIXED_TO_INT(laser->y);

            if (laser_right >= rx && laser_left <= rx + SPRITE_SAM_ROCKET_W &&
                ly >= ry && ly <= ry + SPRITE_SAM_ROCKET_H) {
                laser->active = false;
                sam->active = false;
                ceres_entities_spawn_explosion(pool, sam->x + INT_TO_FIXED(6), sam->y + INT_TO_FIXED(12), 1);
                res.score_delta += 100;
                break;
            }
        }
    }

    /* 3. Bomb vs Ground Targets */
    for (int b = 0; b < CERES_MAX_BOMBS; b++) {
        ceres_bomb_t *bomb = &pool->bombs[b];
        if (!bomb->active) continue;

        int bx = FIXED_TO_INT(bomb->x);
        int by = FIXED_TO_INT(bomb->y);

        for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
            ceres_ground_target_t *t = &pool->targets[i];
            if (!t->active || t->type == CERES_TARGET_SURVIVOR) continue;

            int sx = t->world_x - camera_col;
            int target_top = t->y - t->h;

            if (bx >= sx && bx <= sx + t->w &&
                by >= target_top - 4 && by <= t->y + 4) {

                /* Direct Bomb Hit! */
                bomb->active = false;
                pool->total_bomb_detonations++;

                if (t->type == CERES_TARGET_FUEL_SILO) {
                    t->active = false;
                    pool->total_targets_destroyed++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 2);
                    res.fuel_delta += 250;
                    res.score_delta += 150;
                } else if (t->type == CERES_TARGET_RADAR) {
                    t->active = false;
                    pool->total_targets_destroyed++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 1);
                    res.score_delta += 100;
                } else if (t->type == CERES_TARGET_SAM_SILO) {
                    t->active = false;
                    pool->total_targets_destroyed++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 1);
                    res.score_delta += t->launched ? 50 : 150;
                } else if (t->type == CERES_TARGET_CRYO_BUNKER) {
                    /* Bunker Breach: liberates survivor inside! */
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 1);
                    ceres_entities_spawn_sparks(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top), COLOR_STAR_WHITE, 8);
                    t->type = CERES_TARGET_SURVIVOR;
                    t->w = SPRITE_SURVIVOR_W;
                    t->h = SPRITE_SURVIVOR_H;
                    t->world_x += (SPRITE_CRYO_BUNKER_W - SPRITE_SURVIVOR_W) / 2;
                    t->cracked = true;
                    res.bunker_breached = true;
                    res.score_delta += 100;
                }
                break;
            }
        }
    }

    /* 4. Laser vs Ground Targets */
    for (int l = 0; l < CERES_MAX_LASERS; l++) {
        ceres_laser_t *laser = &pool->lasers[l];
        if (!laser->active) continue;

        int laser_left = FIXED_TO_INT(laser->x);
        int laser_right = laser_left + LASER_SPAN_PX;
        int ly = FIXED_TO_INT(laser->y);

        for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
            ceres_ground_target_t *t = &pool->targets[i];
            if (!t->active || t->type == CERES_TARGET_SURVIVOR) continue;

            int sx = t->world_x - camera_col;
            int target_top = t->y - t->h;

            if (laser_right >= sx && laser_left <= sx + t->w &&
                ly >= target_top && ly <= t->y) {

                laser->active = false;
                if (t->type == CERES_TARGET_CRYO_BUNKER) {
                    /* Armored bunker deflects laser fire */
                    ceres_entities_spawn_sparks(pool, INT_TO_FIXED(laser_right), INT_TO_FIXED(ly), COLOR_LASER_CYAN, 4);
                } else if (t->type == CERES_TARGET_FUEL_SILO) {
                    t->active = false;
                    pool->total_targets_destroyed++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 2);
                    res.fuel_delta += 250;
                    res.score_delta += 150;
                } else if (t->type == CERES_TARGET_RADAR) {
                    t->active = false;
                    pool->total_targets_destroyed++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 1);
                    res.score_delta += 100;
                } else if (t->type == CERES_TARGET_SAM_SILO) {
                    t->active = false;
                    pool->total_targets_destroyed++;
                    ceres_entities_spawn_explosion(pool, INT_TO_FIXED(sx + t->w / 2), INT_TO_FIXED(target_top + t->h / 2), 1);
                    res.score_delta += t->launched ? 50 : 150;
                }
                break;
            }
        }
    }

    /* 5. Tractor Scoop vs Stranded Survivors */
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        ceres_ground_target_t *t = &pool->targets[i];
        if (!t->active || t->type != CERES_TARGET_SURVIVOR) continue;

        int sx = t->world_x - camera_col;
        if (sx < -20 || sx > CERES_VIRTUAL_W + 20) continue;

        int surv_cx = sx + t->w / 2;
        int surv_cy = t->y - t->h / 2;
        int ship_cx = ship_x + ship_w / 2;
        int ship_cy = ship_y + ship_h / 2;

        int dx = ship_cx - surv_cx;
        if (dx < 0) dx = -dx;

        /* Tractor proximity window: horizontal alignment and ship above survivor */
        if (dx <= 32 && ship_cy < surv_cy && (surv_cy - ship_cy) <= 85) {
            /* If within close scoop distance or tractor holds */
            if (tractor_active || (surv_cy - ship_cy) <= 55) {
                t->active = false;
                pool->total_survivors_rescued++;
                res.survivor_rescued = true;
                res.fuel_delta += 200;  /* +20% Fuel Bonus */
                res.score_delta += 500; /* Major rescue award */

                /* Golden rescue sparkle celebration */
                ceres_entities_spawn_sparks(pool, INT_TO_FIXED(surv_cx), INT_TO_FIXED(ship_cy + 8), COLOR_SURVIVOR_GREEN, 10);
                ceres_entities_spawn_sparks(pool, INT_TO_FIXED(surv_cx), INT_TO_FIXED(ship_cy + 8), COLOR_STAR_WHITE, 6);
            }
        }
    }

    /* 6. SAM Rocket vs Player Craft */
    for (int s = 0; s < CERES_MAX_SAM_ROCKETS; s++) {
        ceres_sam_rocket_t *sam = &pool->sam_rockets[s];
        if (!sam->active) continue;

        int rx = FIXED_TO_INT(sam->x);
        int ry = FIXED_TO_INT(sam->y);

        if (rx + SPRITE_SAM_ROCKET_W >= ship_x && rx <= ship_x + ship_w &&
            ry + SPRITE_SAM_ROCKET_H >= ship_y && ry <= ship_y + ship_h) {
            sam->active = false;
            ceres_entities_spawn_explosion(pool, sam->x + INT_TO_FIXED(6), sam->y + INT_TO_FIXED(12), 2);
            res.player_hit = true;
            break;
        }
    }

    /* 7. Weapon vs Aerial Enemies */
    for (int a_idx = 0; a_idx < CERES_MAX_AERIAL_ENEMIES; a_idx++) {
        ceres_aerial_t *a = &pool->aerials[a_idx];
        if (!a->active || a->type == CERES_AERIAL_PARACHUTE) continue;

        int ax = FIXED_TO_INT(a->x);
        int ay = FIXED_TO_INT(a->y);

        /* Laser collision check */
        for (int l = 0; l < CERES_MAX_LASERS; l++) {
            ceres_laser_t *laser = &pool->lasers[l];
            if (!laser->active) continue;

            int lx = FIXED_TO_INT(laser->x);
            int lr = lx + LASER_SPAN_PX;
            int ly = FIXED_TO_INT(laser->y);

            if (lr >= ax && lx <= ax + a->w && ly >= ay && ly <= ay + a->h) {
                laser->active = false;
                a->hp--;
                if (a->hp <= 0) {
                    a->active = false;
                    pool->total_aerials_destroyed++;
                    ceres_entities_spawn_explosion(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h / 2), 1);

                    if (a->type == CERES_AERIAL_SAUCER)        res.score_delta += 200;
                    else if (a->type == CERES_AERIAL_FIGHTER)   res.score_delta += 150;
                    else if (a->type == CERES_AERIAL_METEOR)    res.score_delta += 75;
                    else if (a->type == CERES_AERIAL_HARVESTER) {
                        res.score_delta += 400;
                        if (a->carrying_survivor) {
                            /* Liberate survivor on parachute! */
                            ceres_entities_spawn_aerial(pool, CERES_AERIAL_PARACHUTE, a->x + INT_TO_FIXED(4), a->y + INT_TO_FIXED(10));
                        }
                    }
                } else {
                    ceres_entities_spawn_sparks(pool, INT_TO_FIXED(lr), INT_TO_FIXED(ly), COLOR_STAR_WHITE, 4);
                }
                break;
            }
        }

        /* Bomb collision check (if still active) */
        if (!a->active) continue;
        for (int b = 0; b < CERES_MAX_BOMBS; b++) {
            ceres_bomb_t *bomb = &pool->bombs[b];
            if (!bomb->active) continue;

            int bx = FIXED_TO_INT(bomb->x);
            int by = FIXED_TO_INT(bomb->y);

            if (bx >= ax && bx <= ax + a->w && by >= ay && by <= ay + a->h) {
                bomb->active = false;
                a->active = false;
                pool->total_bomb_detonations++;
                pool->total_aerials_destroyed++;
                ceres_entities_spawn_explosion(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h / 2), 2);
                res.score_delta += 250;
                if (a->type == CERES_AERIAL_HARVESTER && a->carrying_survivor) {
                    ceres_entities_spawn_aerial(pool, CERES_AERIAL_PARACHUTE, a->x + INT_TO_FIXED(4), a->y + INT_TO_FIXED(10));
                }
                break;
            }
        }
    }

    /* 8. Player Craft vs Aerial Enemies */
    for (int a_idx = 0; a_idx < CERES_MAX_AERIAL_ENEMIES; a_idx++) {
        ceres_aerial_t *a = &pool->aerials[a_idx];
        if (!a->active) continue;

        int ax = FIXED_TO_INT(a->x);
        int ay = FIXED_TO_INT(a->y);

        if (ax + a->w >= ship_x && ax <= ship_x + ship_w &&
            ay + a->h >= ship_y && ay <= ship_y + ship_h) {

            if (a->type == CERES_AERIAL_PARACHUTE) {
                /* HEROIC MID-AIR RESCUE! */
                a->active = false;
                pool->total_survivors_rescued++;
                pool->total_parachutes_rescued++;
                res.survivor_rescued = true;
                res.heroic_rescue = true;
                res.fuel_delta += 300;
                res.score_delta += 1000;
                ceres_entities_spawn_sparks(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h / 2), COLOR_SURVIVOR_GREEN, 14);
                ceres_entities_spawn_sparks(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h / 2), COLOR_STAR_WHITE, 8);
            } else {
                /* Deadly collision with enemy craft or meteor */
                a->active = false;
                ceres_entities_spawn_explosion(pool, a->x + INT_TO_FIXED(a->w / 2), a->y + INT_TO_FIXED(a->h / 2), 2);
                res.player_hit = true;
                break;
            }
        }
    }

    return res;
}

/* Ground installation foundation pad renderer */
static void render_foundation_pad(const ceres_surface_t *surf, const ceres_terrain_t *terrain,
                                  int target_x, int base_y, int w) {
    if (!surf || !terrain) return;
    int x0 = target_x;
    int x1 = target_x + w;
    if (x0 < 0) x0 = 0;
    if (x1 > surf->width) x1 = surf->width;

    for (int x = x0; x < x1; x++) {
        int ground_y = surf->height - 12;
        ceres_terrain_sample(terrain, x, NULL, &ground_y);
        if (ground_y > base_y) {
            int dx = x - target_x;
            for (int fy = base_y; fy <= ground_y; fy++) {
                uint16_t pad_color;
                if (fy == base_y) {
                    pad_color = RGB565(90, 95, 105); /* Foundation top seam */
                } else if (dx == 0 || dx == w - 1) {
                    pad_color = RGB565(70, 75, 85);   /* Corner reinforcement pillar */
                } else {
                    pad_color = ((dx ^ fy) & 2) ? RGB565(48, 52, 58) : RGB565(36, 40, 46);
                }
                ceres_blit_pixel(surf, x, fy, pad_color);
            }
        }
    }
}

void ceres_entities_render(const ceres_entities_t *pool, const ceres_terrain_t *terrain, const ceres_surface_t *surf) {
    if (!pool || !surf) return;

    int camera_col = terrain ? (int)terrain->camera_col : 0;

    /* 1. Render Ground Installations with Bedrock Foundation Pads */
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        const ceres_ground_target_t *t = &pool->targets[i];
        if (!t->active) continue;

        int sx = t->world_x - camera_col;
        if (sx < -t->w || sx >= surf->width) continue;

        int draw_y = t->y - t->h;

        if (t->type == CERES_TARGET_SURVIVOR) {
            /* Survivor standing directly on terrain */
            ceres_blit_sprite(surf, sx, draw_y, &g_surf_survivor, CERES_KEY_COLOR);

            /* Distress Beacon Wave: Pulsing cyan/green rings above survivor */
            if ((t->anim_timer % 30) < 15) {
                ceres_blit_pixel(surf, sx + 8, draw_y - 3, COLOR_SURVIVOR_GREEN);
                ceres_blit_pixel(surf, sx + 7, draw_y - 2, COLOR_SURVIVOR_GREEN);
                ceres_blit_pixel(surf, sx + 9, draw_y - 2, COLOR_SURVIVOR_GREEN);
            }
        } else {
            /* Structural Foundation Anchoring into Terrain Floor */
            if (terrain) {
                render_foundation_pad(surf, terrain, sx, t->y, t->w);
            }

            /* Miniature Sprite Blit */
            const ceres_surface_t *sprite = NULL;
            switch (t->type) {
                case CERES_TARGET_FUEL_SILO:
                    sprite = &g_surf_fuel_silo;
                    break;
                case CERES_TARGET_RADAR:
                    sprite = &g_surf_radar_dish;
                    break;
                case CERES_TARGET_SAM_SILO:
                    sprite = &g_surf_sam_silo;
                    break;
                case CERES_TARGET_CRYO_BUNKER:
                    sprite = &g_surf_cryo_bunker;
                    break;
                default:
                    break;
            }

            if (sprite) {
                ceres_blit_sprite(surf, sx, draw_y, sprite, CERES_KEY_COLOR);
            }

            /* Cryo-Bunker Life Sign Beacon Pulse */
            if (t->type == CERES_TARGET_CRYO_BUNKER && ((t->anim_timer % 20) < 10)) {
                ceres_blit_pixel(surf, sx + 15, draw_y + 3, COLOR_SURVIVOR_GREEN);
                ceres_blit_pixel(surf, sx + 16, draw_y + 3, COLOR_STAR_WHITE);
            }
        }
    }

    /* 2. Render Surface-to-Air Missiles (SAM) */
    for (int s = 0; s < CERES_MAX_SAM_ROCKETS; s++) {
        const ceres_sam_rocket_t *sam = &pool->sam_rockets[s];
        if (!sam->active) continue;

        int rx = FIXED_TO_INT(sam->x);
        int ry = FIXED_TO_INT(sam->y);

        /* Blit Miniature SAM Rocket */
        ceres_blit_sprite(surf, rx, ry, &g_surf_sam_rocket, CERES_KEY_COLOR);

        /* Fiery Rocket Thruster Exhaust Plume */
        int flame_len = 6 + (int)(sam->life_frames % 5);
        int nozzle_x = rx + SPRITE_SAM_ROCKET_W / 2;
        int nozzle_y = ry + SPRITE_SAM_ROCKET_H;

        ceres_blit_line(surf, nozzle_x, nozzle_y, nozzle_x, nozzle_y + flame_len, COLOR_THRUSTER_YELLOW);
        ceres_blit_line(surf, nozzle_x - 1, nozzle_y, nozzle_x - 1, nozzle_y + flame_len - 2, COLOR_THRUSTER_ORANGE);
        ceres_blit_line(surf, nozzle_x + 1, nozzle_y, nozzle_x + 1, nozzle_y + flame_len - 2, COLOR_THRUSTER_ORANGE);
    }

    /* 3. Render Forward Lasers */
    for (int i = 0; i < CERES_MAX_LASERS; i++) {
        const ceres_laser_t *laser = &pool->lasers[i];
        if (!laser->active) continue;

        int lx = FIXED_TO_INT(laser->x);
        int ly = FIXED_TO_INT(laser->y);

        /* Outer cyan glow beam */
        ceres_blit_line(surf, lx, ly, lx + LASER_SPAN_PX, ly, COLOR_LASER_CYAN);
        /* Inner white core */
        if (LASER_SPAN_PX >= 6) {
            ceres_blit_line(surf, lx + 3, ly, lx + LASER_SPAN_PX - 2, ly, COLOR_STAR_WHITE);
        }
    }

    /* 4. Render Gravity Bombs */
    for (int i = 0; i < CERES_MAX_BOMBS; i++) {
        const ceres_bomb_t *bomb = &pool->bombs[i];
        if (!bomb->active) continue;

        int bx = FIXED_TO_INT(bomb->x);
        int by = FIXED_TO_INT(bomb->y);

        /* Classic arcade bomb: oval body, red tip, white tail fin */
        ceres_blit_line(surf, bx - 1, by - 1, bx + 1, by - 1, COLOR_BOMB_YELLOW);
        ceres_blit_line(surf, bx - 2, by,     bx + 2, by,     COLOR_BOMB_YELLOW);
        ceres_blit_line(surf, bx - 1, by + 1, bx + 1, by + 1, COLOR_BOMB_YELLOW);

        /* Red nose */
        ceres_blit_pixel(surf, bx + 2, by, COLOR_SHIP_TRIM);
        /* Top tail fin */
        ceres_blit_pixel(surf, bx - 2, by - 1, COLOR_STAR_WHITE);
        ceres_blit_pixel(surf, bx - 2, by + 1, COLOR_STAR_WHITE);
    }

    /* 5. Render Particles */
    for (int p = 0; p < CERES_MAX_PARTICLES; p++) {
        const ceres_particle_t *part = &pool->particles[p];
        if (!part->active) continue;

        int px = FIXED_TO_INT(part->x);
        int py = FIXED_TO_INT(part->y);

        if (part->life > part->max_life / 2) {
            /* 2x2 particle */
            ceres_blit_pixel(surf, px, py, part->color);
            ceres_blit_pixel(surf, px + 1, py, part->color);
            ceres_blit_pixel(surf, px, py + 1, part->color);
            ceres_blit_pixel(surf, px + 1, py + 1, part->color);
        } else {
            /* 1x1 particle */
            ceres_blit_pixel(surf, px, py, part->color);
        }
    }

    /* 6. Render Aerial Enemies */
    for (int i = 0; i < CERES_MAX_AERIAL_ENEMIES; i++) {
        const ceres_aerial_t *a = &pool->aerials[i];
        if (!a->active) continue;

        int sx = FIXED_TO_INT(a->x);
        int sy = FIXED_TO_INT(a->y);

        switch (a->type) {
            case CERES_AERIAL_SAUCER:
                ceres_blit_sprite(surf, sx, sy, &g_surf_saucer, CERES_KEY_COLOR);
                break;
            case CERES_AERIAL_FIGHTER:
                ceres_blit_sprite(surf, sx, sy, &g_surf_fighter, CERES_KEY_COLOR);
                break;
            case CERES_AERIAL_METEOR:
                ceres_blit_sprite(surf, sx, sy, &g_surf_meteor, CERES_KEY_COLOR);
                break;
            case CERES_AERIAL_HARVESTER:
                ceres_blit_sprite(surf, sx, sy, &g_surf_harvester, CERES_KEY_COLOR);
                /* Draw glowing amber tractor beam if descending */
                if (a->vy > 0 && !a->carrying_survivor) {
                    int beam_top = sy + a->h;
                    int beam_bot = beam_top + 36;
                    if (beam_bot > surf->height) beam_bot = surf->height;
                    for (int by = beam_top; by < beam_bot; by += 2) {
                        int spread = (by - beam_top) / 4;
                        ceres_blit_pixel(surf, sx + a->w / 2 - spread, by, COLOR_THRUSTER_YELLOW);
                        ceres_blit_pixel(surf, sx + a->w / 2 + spread, by, COLOR_THRUSTER_YELLOW);
                    }
                }
                /* Draw dangling captured victim if carrying */
                if (a->carrying_survivor) {
                    ceres_blit_sprite(surf, sx + a->w / 2 - SPRITE_SURVIVOR_W / 2, sy + a->h - 4, &g_surf_survivor, CERES_KEY_COLOR);
                }
                break;
            case CERES_AERIAL_PARACHUTE:
                ceres_blit_sprite(surf, sx, sy, &g_surf_parachutist, CERES_KEY_COLOR);
                break;
            default:
                break;
        }
    }
}
