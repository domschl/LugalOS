#include "moon_starship.h"
#include "moon_sprites.h"
#include <string.h>

/* Pure integer 16-point unit circle lookup scaled by 256 (no floats in kernel/M-mode) */
static const int16_t s_cos16[16] = {
    256, 236, 181,  97,    0,  -97, -181, -236,
   -256, -236, -181, -97,    0,   97,  181,  236
};

static const int16_t s_sin16[16] = {
      0,  97, 181, 236,  256,  236,  181,   97,
      0, -97, -181, -236, -256, -236, -181,  -97
};

void moon_starship_init(moon_starship_t *ship) {
    if (!ship) return;
    memset(ship, 0, sizeof(moon_starship_t));
    ship->state = MOON_STARSHIP_ORBIT;
    ship->alt_px = 360;
    ship->lh2_target_kg = fixed_from_int(250);   /* Standard Starship Hydrolox tank load */
    ship->lox_target_kg = fixed_from_int(2000);
    ship->turnaround_timer_ms = MOON_STARSHIP_WINDOW_MS;
    ship->manifest.colonists = 4;
    ship->manifest.regolith_blocks = 100;
    ship->manifest.reactor_power_kw = 30;
    ship->manifest.delivered = false;
    ship->window_expired = false;
    ship->commercial_mission_success = false;
}

void moon_starship_deliver_cargo(moon_starship_t *ship, moon_resources_t *res) {
    if (!ship || ship->manifest.delivered) return;
    ship->manifest.delivered = true;
    ship->turnaround_timer_ms = MOON_STARSHIP_WINDOW_MS;

    if (res) {
        /* 1. Specialist colonists arrive and move into base housing */
        if (res->population < res->max_population) {
            uint16_t room = res->max_population - res->population;
            uint16_t add = (ship->manifest.colonists < room) ? ship->manifest.colonists : room;
            res->population += add;
            if (res->population > res->peak_population) {
                res->peak_population = res->population;
            }
        }

        /* 2. Heavy machinery & prefabricated structural components */
        res->regolith_stored_kg += fixed_from_int(ship->manifest.regolith_blocks);
        if (res->regolith_stored_kg > fixed_from_int(res->regolith_capacity_kg)) {
            res->regolith_stored_kg = fixed_from_int(res->regolith_capacity_kg);
        }

        /* 3. High-yield compact modular reactor (+30 kW continuous power) */
        res->reactor_power_kw += ship->manifest.reactor_power_kw;
    }
}

bool moon_starship_find_pad(const moon_structure_mgr_t *mgr, int32_t *out_gx, int32_t *out_gy) {
    if (!mgr) return false;
    for (uint8_t i = 1; i < MOON_MAX_STRUCTURES; i++) {
        if (mgr->list[i].id == i && mgr->list[i].type == MOON_STRUCT_LANDING_PAD && mgr->list[i].active) {
            if (out_gx) *out_gx = mgr->list[i].gx;
            if (out_gy) *out_gy = mgr->list[i].gy;
            return true;
        }
    }
    return false;
}

bool moon_starship_call(moon_starship_t *ship, int32_t pad_gx, int32_t pad_gy) {
    if (!ship) return false;
    if (ship->state != MOON_STARSHIP_ORBIT && ship->state != MOON_STARSHIP_DEPARTED) {
        return false; /* Flight already in progress */
    }

    ship->pad_gx = pad_gx;
    ship->pad_gy = pad_gy;
    ship->has_pad = true;
    ship->state = MOON_STARSHIP_APPROACH;
    ship->state_timer_ms = 0;
    ship->alt_px = 360;
    ship->lh2_loaded_kg = 0;
    ship->lox_loaded_kg = 0;
    ship->umbilical_pct = 0;
    ship->dust_count = 0;
    ship->turnaround_timer_ms = MOON_STARSHIP_WINDOW_MS;
    ship->window_expired = false;
    ship->commercial_mission_success = false;

    /* Fresh cargo manifest arriving from Earth */
    ship->manifest.colonists = 4;
    ship->manifest.regolith_blocks = 100;
    ship->manifest.reactor_power_kw = 30;
    ship->manifest.delivered = false;
    return true;
}

bool moon_starship_launch(moon_starship_t *ship) {
    if (!ship) return false;
    if (ship->state != MOON_STARSHIP_SERVICING && ship->state != MOON_STARSHIP_PRELAUNCH) {
        return false;
    }

    if (ship->state == MOON_STARSHIP_SERVICING) {
        if (ship->lh2_loaded_kg < ship->lh2_target_kg ||
            ship->lox_loaded_kg < ship->lox_target_kg) {
            return false; /* Refueling not complete */
        }
    }

    ship->commercial_mission_success = true;
    ship->state = MOON_STARSHIP_PRELAUNCH;
    ship->state_timer_ms = 0;
    return true;
}

static void spawn_dust_skirt(moon_starship_t *ship, int speed_scale) {
    ship->dust_count = MOON_STARSHIP_MAX_DUST;
    for (int i = 0; i < MOON_STARSHIP_MAX_DUST; i++) {
        int dir = i % 16;
        ship->dust[i].x = 0;
        ship->dust[i].y = 0;
        /* Radial speed in 2:1 dimetric isometric space: vy = vx / 2 */
        ship->dust[i].vx = (s_cos16[dir] * speed_scale) / 256;
        ship->dust[i].vy = (s_sin16[dir] * speed_scale) / 512;
        ship->dust[i].life = 20;
        ship->dust[i].max_life = 20;
    }
}

static void update_dust(moon_starship_t *ship) {
    for (int i = 0; i < ship->dust_count; i++) {
        if (ship->dust[i].life > 0) {
            ship->dust[i].x += ship->dust[i].vx;
            ship->dust[i].y += ship->dust[i].vy;
            ship->dust[i].life--;
        }
    }
}

void moon_starship_update(moon_starship_t *ship,
                          const moon_structure_mgr_t *mgr,
                          moon_resources_t *res,
                          uint32_t delta_ms) {
    if (!ship) return;
    ship->anim_frame++;
    update_dust(ship);

    if (ship->state == MOON_STARSHIP_ORBIT) {
        /* If a pad exists and no mission in progress, check if auto-called or stay in orbit */
        return;
    }

    /* Verify target pad still exists */
    if (mgr && ship->has_pad) {
        bool pad_alive = false;
        for (uint8_t i = 1; i < MOON_MAX_STRUCTURES; i++) {
            if (mgr->list[i].id == i &&
                mgr->list[i].type == MOON_STRUCT_LANDING_PAD &&
                mgr->list[i].gx == ship->pad_gx &&
                mgr->list[i].gy == ship->pad_gy &&
                mgr->list[i].active) {
                pad_alive = true;
                break;
            }
        }
        if (!pad_alive && ship->state != MOON_STARSHIP_DEPARTED) {
            /* Emergency pad abort: return straight to orbit */
            ship->state = MOON_STARSHIP_DEPARTED;
            ship->state_timer_ms = 0;
            return;
        }
    }

    ship->state_timer_ms += delta_ms;

    switch (ship->state) {
        case MOON_STARSHIP_ORBIT:
            break;

        case MOON_STARSHIP_APPROACH: {
            /* 1.5 s descent from 360 px to 240 px */
            const uint32_t dur = 1500;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_RETRO;
                ship->state_timer_ms = 0;
                ship->alt_px = 240;
            } else {
                int64_t t = ship->state_timer_ms;
                ship->alt_px = 360 - (int32_t)((120 * t) / dur);
            }
            break;
        }

        case MOON_STARSHIP_RETRO: {
            /* 3.0 s ease-out retro deceleration from 240 px down to 40 px:
             * alt = 40 + 200 * ((dur - t) / dur)^2 */
            const uint32_t dur = 3000;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_GEAR_EXTEND;
                ship->state_timer_ms = 0;
                ship->alt_px = 40;
            } else {
                int64_t rem = (int64_t)(dur - ship->state_timer_ms);
                ship->alt_px = 40 + (int32_t)((200 * rem * rem) / ((int64_t)dur * dur));
            }
            break;
        }

        case MOON_STARSHIP_GEAR_EXTEND: {
            /* 1.2 s final landing flare from 40 px down to 0 px with ground effect */
            const uint32_t dur = 1200;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_TOUCHDOWN;
                ship->state_timer_ms = 0;
                ship->alt_px = 0;
                spawn_dust_skirt(ship, 3);
            } else {
                int64_t rem = (int64_t)(dur - ship->state_timer_ms);
                ship->alt_px = (int32_t)((40 * rem * rem) / ((int64_t)dur * dur));
            }
            break;
        }

        case MOON_STARSHIP_TOUCHDOWN: {
            /* 1.0 s engine cutoff, dust skirts spreading across apron */
            ship->alt_px = 0;
            const uint32_t dur = 1000;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_SERVICING;
                ship->state_timer_ms = 0;
                ship->umbilical_pct = 0;
                moon_starship_deliver_cargo(ship, res);
            }
            break;
        }

        case MOON_STARSHIP_SERVICING: {
            ship->alt_px = 0;
            if (!ship->manifest.delivered) {
                moon_starship_deliver_cargo(ship, res);
            }

            /* Umbilical arm extends over 1.0 second */
            if (ship->umbilical_pct < 100) {
                uint32_t step = (delta_ms * 100) / 1000;
                if (step == 0) step = 1;
                ship->umbilical_pct = (uint8_t)((ship->umbilical_pct + step > 100) ? 100 : ship->umbilical_pct + step);
            }

            /* Turnaround timer counts down towards launch window close */
            if (ship->turnaround_timer_ms > delta_ms) {
                ship->turnaround_timer_ms -= delta_ms;
            } else {
                ship->turnaround_timer_ms = 0;
            }

            /* Propellant transfer once umbilical is locked in */
            if (ship->umbilical_pct >= 100 && res) {
                /* Target rates: 100 kg/s LOX, 12.5 kg/s LH2 */
                fixed32_t rate_lox = fixed_from_int(100);
                fixed32_t rate_lh2 = fixed_from_int(12) + (FIXED_ONE / 2); /* 12.5 */

                fixed32_t d_lox = fixed_mul(rate_lox, fixed_from_int(delta_ms)) / 1000;
                fixed32_t d_lh2 = fixed_mul(rate_lh2, fixed_from_int(delta_ms)) / 1000;

                /* Clamp to remaining requirement */
                if (ship->lox_loaded_kg + d_lox > ship->lox_target_kg) {
                    d_lox = ship->lox_target_kg - ship->lox_loaded_kg;
                }
                if (ship->lh2_loaded_kg + d_lh2 > ship->lh2_target_kg) {
                    d_lh2 = ship->lh2_target_kg - ship->lh2_loaded_kg;
                }

                /* Clamp to available base reserves */
                if (d_lox > res->lox_stored_kg) d_lox = res->lox_stored_kg;
                if (d_lh2 > res->lh2_stored_kg) d_lh2 = res->lh2_stored_kg;

                /* Transfer */
                if (d_lox > 0) {
                    res->lox_stored_kg -= d_lox;
                    ship->lox_loaded_kg += d_lox;
                }
                if (d_lh2 > 0) {
                    res->lh2_stored_kg -= d_lh2;
                    ship->lh2_loaded_kg += d_lh2;
                }

                /* Automatically advance to prelaunch when fully refueled */
                if (ship->lh2_loaded_kg >= ship->lh2_target_kg &&
                    ship->lox_loaded_kg >= ship->lox_target_kg) {
                    ship->state = MOON_STARSHIP_PRELAUNCH;
                    ship->state_timer_ms = 0;
                }
            }

            /* Launch window expiry: abort to orbit if turnaround window closed before refueling */
            if (ship->turnaround_timer_ms == 0 && ship->state == MOON_STARSHIP_SERVICING) {
                ship->window_expired = true;
                ship->state = MOON_STARSHIP_PRELAUNCH;
                ship->state_timer_ms = 0;
                ship->lh2_loaded_kg = 0;
                ship->lox_loaded_kg = 0;
            }
            break;
        }

        case MOON_STARSHIP_PRELAUNCH: {
            ship->alt_px = 0;
            /* Umbilical arm retracts back to gantry tower */
            if (ship->umbilical_pct > 0) {
                uint32_t step = (delta_ms * 100) / 1000;
                if (step == 0) step = 1;
                ship->umbilical_pct = (uint8_t)((ship->umbilical_pct < step) ? 0 : ship->umbilical_pct - step);
            }

            /* 1.5 s pre-ignition sequence and hazard beacons */
            const uint32_t dur = 1500;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_ASCENT;
                ship->state_timer_ms = 0;
                spawn_dust_skirt(ship, 4);
            }
            break;
        }

        case MOON_STARSHIP_ASCENT: {
            /* 3.2 s ease-in vertical ascent acceleration:
             * alt = 450 * (t / dur)^2 */
            const uint32_t dur = 3200;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_DEPARTED;
                ship->state_timer_ms = 0;
                ship->alt_px = 450;
                if (!ship->window_expired &&
                    ship->lh2_loaded_kg >= ship->lh2_target_kg &&
                    ship->lox_loaded_kg >= ship->lox_target_kg) {
                    ship->commercial_mission_success = true;
                }
            } else {
                int64_t t = (int64_t)ship->state_timer_ms;
                ship->alt_px = (int32_t)((450 * t * t) / ((int64_t)dur * dur));
            }
            break;
        }

        case MOON_STARSHIP_DEPARTED: {
            /* 1.0 s transit confirmation before resetting to orbit */
            const uint32_t dur = 1000;
            if (ship->state_timer_ms >= dur) {
                ship->state = MOON_STARSHIP_ORBIT;
                ship->state_timer_ms = 0;
                ship->alt_px = 360;
                ship->lh2_loaded_kg = 0;
                ship->lox_loaded_kg = 0;
                ship->has_pad = false;
            }
            break;
        }
    }
}

/* Render pale-blue / cyan hydrolox rocket exhaust plume with shock diamonds */
static void render_hydrolox_plume(const moon_surface_t *surf,
                                  int32_t cx, int32_t cy,
                                  int32_t alt_px, uint32_t frame) {
    /* Rocket nozzle base is at (cx, cy). Plume shoots vertically down. */
    int nominal_len = 54;
    int plume_len = nominal_len;
    bool pad_impinge = false;

    if (alt_px < nominal_len) {
        plume_len = alt_px;
        pad_impinge = true;
    }

    /* Core colors for clean Hydrolox (LH2 + LOX) combustion */
    uint16_t col_core   = RGB565(240, 255, 255); /* White-hot core */
    uint16_t col_cyan   = COLOR_LASER_CYAN;      /* Hydrolox cyan 0x07FF */
    uint16_t col_sheath = RGB565(40, 140, 230);  /* Outer blue glow */

    /* Plume envelope: tapered triangular cone */
    for (int dy = 0; dy < plume_len; dy++) {
        int half_w;
        if (dy < 12) {
            half_w = 7 + (dy * 2) / 12;
        } else {
            half_w = 9 - ((dy - 12) * 7) / (nominal_len - 12);
            if (half_w < 2) half_w = 2;
        }

        /* Flicker width */
        if ((frame + dy) & 1) half_w += 1;

        int py = cy + dy;
        moon_blit_line(surf, cx - half_w, py, cx + half_w, py, col_sheath);
        if (half_w >= 2) {
            moon_blit_line(surf, cx - half_w + 1, py, cx + half_w - 1, py, col_cyan);
        }
        if (half_w >= 4 && dy < 36) {
            moon_blit_line(surf, cx - 1, py, cx + 1, py, col_core);
        }
    }

    /* Supersonic Shock Diamonds (Mach diamond nodes along centerline) */
    static const int shock_nodes[4] = { 10, 22, 34, 46 };
    for (int i = 0; i < 4; i++) {
        int ny = cy + shock_nodes[i];
        if (shock_nodes[i] >= plume_len - 4) break;

        int dw = 3 + ((frame + i) & 1);
        int dh = 4;

        /* Diamond node: top, bottom, left, right vertices */
        moon_blit_line(surf, cx - dw, ny, cx, ny - dh, col_core);
        moon_blit_line(surf, cx, ny - dh, cx + dw, ny, col_core);
        moon_blit_line(surf, cx + dw, ny, cx, ny + dh, col_core);
        moon_blit_line(surf, cx, ny + dh, cx - dw, ny, col_core);
        moon_blit_fill_rect(surf, cx - 1, ny - 1, 3, 3, RGB565(255, 255, 255));
    }

    /* Ground-effect flame deflection along the 4x4 blast berms */
    if (pad_impinge) {
        int ground_y = cy + alt_px;
        int spread = (nominal_len - alt_px) * 2;
        if (spread > 38) spread = 38;

        /* Twin deflection flame tongues */
        moon_blit_line(surf, cx, ground_y, cx - spread, ground_y + (spread / 3), col_cyan);
        moon_blit_line(surf, cx, ground_y, cx + spread, ground_y + (spread / 3), col_cyan);
        moon_blit_line(surf, cx - 2, ground_y - 1, cx + 2, ground_y - 1, col_core);
    }
}

/* Render articulated cryogenic gantry umbilical arm from pad tower to ship */
static void render_umbilical_arm(const moon_surface_t *surf,
                                 int32_t pad_cx, int32_t pad_cy,
                                 uint8_t pct, uint32_t frame) {
    if (pct == 0) return;

    /* Tower pivot joint (on gantry tower to the right/north-east of pad center) */
    int32_t tx = pad_cx + 34;
    int32_t ty = pad_cy - 74;

    /* Ship cryogenic service port (starboard/leeward flank of Starship) */
    int32_t sx = pad_cx + 12;
    int32_t sy = pad_cy - 24;

    /* Interpolate current arm tip position based on extension percentage */
    int32_t cur_tip_x = tx + ((sx - tx) * (int32_t)pct) / 100;
    int32_t cur_tip_y = ty + ((sy - ty) * (int32_t)pct) / 100;

    /* Articulated elbow joint */
    int32_t ex = (tx + cur_tip_x) / 2 + 8;
    int32_t ey = (ty + cur_tip_y) / 2 - 4;

    uint16_t truss_col = RGB565(70, 75, 85);
    uint16_t cryo_col  = COLOR_LASER_CYAN;
    uint16_t frost_col = COLOR_LUNAR_WHITE;

    /* Segment 1: Tower to Elbow */
    moon_blit_line(surf, tx, ty, ex, ey, truss_col);
    moon_blit_line(surf, tx, ty - 1, ex, ey - 1, cryo_col);

    /* Segment 2: Elbow to Ship Tip */
    moon_blit_line(surf, ex, ey, cur_tip_x, cur_tip_y, truss_col);
    moon_blit_line(surf, ex, ey - 1, cur_tip_x, cur_tip_y - 1, cryo_col);

    /* Cryogenic coupling collar */
    moon_blit_fill_rect(surf, cur_tip_x - 2, cur_tip_y - 2, 5, 5, frost_col);

    /* Tiny boil-off vapor venting puff when locked in */
    if (pct == 100) {
        int v_off = (frame >> 2) & 3;
        moon_blit_pixel(surf, cur_tip_x + 3 + v_off, cur_tip_y - 2 - v_off, RGB565(180, 240, 255));
        moon_blit_pixel(surf, cur_tip_x + 4 + v_off, cur_tip_y - 3 - v_off, RGB565(240, 255, 255));
    }
}

/* Render expanding particulate dust skirts across the 4x4 sintered foundation */
static void render_dust_skirts(const moon_surface_t *surf,
                               int32_t pad_cx, int32_t pad_cy,
                               const moon_dust_particle_t *dust, uint8_t count) {
    for (int i = 0; i < count; i++) {
        if (dust[i].life == 0) continue;

        int32_t px = pad_cx + dust[i].x;
        int32_t py = pad_cy + dust[i].y;

        uint16_t col;
        if (dust[i].life > 14) {
            col = COLOR_REGOLITH_LIGHT;
        } else if (dust[i].life > 7) {
            col = RGB565(130, 135, 140);
        } else {
            col = RGB565(75, 78, 85);
        }

        moon_blit_fill_rect(surf, px - 1, py - 1, 3, 2, col);
    }
}

/* Render floating 10-pip Cryogenic Loading Meter above Starship nosecone */
static void render_cryo_meter(const moon_surface_t *surf,
                              int32_t ship_cx, int32_t ship_top_y,
                              fixed32_t lh2, fixed32_t lh2_max,
                              fixed32_t lox, fixed32_t lox_max,
                              uint32_t turnaround_timer_ms,
                              uint32_t frame) {
    int max_pips = 10;
    int total_max = fixed_to_int(lh2_max) + fixed_to_int(lox_max);
    int total_cur = fixed_to_int(lh2) + fixed_to_int(lox);
    int pips = (total_max > 0) ? (total_cur * max_pips + (total_max / 20)) / total_max : 0;
    if (pips > max_pips) pips = max_pips;

    int bar_w = max_pips * 4;
    int bar_x = ship_cx - (bar_w / 2);
    int bar_y = ship_top_y - 12;
    int bar_h = 4;

    /* Bezel with launch window warning */
    uint16_t border_col = RGB565(50, 60, 75);
    if (turnaround_timer_ms < 30000 && pips < max_pips) {
        border_col = ((frame >> 3) & 1) ? COLOR_ALERT_RED : COLOR_ENERGY_YELLOW;
    }
    moon_blit_fill_rect(surf, bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, RGB565(15, 18, 22));
    moon_blit_rect_outline(surf, bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, border_col);

    uint16_t fill_col = (pips == max_pips && ((frame >> 2) & 1)) ? COLOR_STATUS_GREEN : COLOR_LASER_CYAN;

    for (int p = 0; p < max_pips; p++) {
        uint16_t col = (p < pips) ? fill_col : RGB565(30, 34, 42);
        moon_blit_fill_rect(surf, bar_x + (p * 4), bar_y, 3, bar_h, col);
    }
}

void moon_starship_render(const moon_starship_t *ship,
                          const moon_grid_t *grid,
                          const moon_surface_t *surf,
                          int32_t cam_x, int32_t cam_y) {
    if (!ship || !grid || !surf || !ship->has_pad) return;
    if (ship->state == MOON_STARSHIP_ORBIT) return;

    /* Pad diamond center on screen */
    int32_t pad_sx, pad_sy;
    moon_grid_to_screen(ship->pad_gx, ship->pad_gy, 0,
                        cam_x, cam_y, surf->width, surf->height,
                        &pad_sx, &pad_sy);

    int32_t pad_cx = pad_sx;
    int32_t pad_cy = pad_sy + 48;

    /* 1. Dust Skirts (drawn under the rocket on the apron) */
    render_dust_skirts(surf, pad_cx, pad_cy, ship->dust, ship->dust_count);

    /* 2. Rocket base position */
    int32_t ship_cx = pad_cx;
    int32_t ship_base_y = pad_cy - ship->alt_px;

    /* 3. Hydrolox Rocket Plume & Shock Diamonds (drawn below rocket nozzle) */
    if (ship->state == MOON_STARSHIP_RETRO ||
        ship->state == MOON_STARSHIP_GEAR_EXTEND ||
        ship->state == MOON_STARSHIP_ASCENT) {
        render_hydrolox_plume(surf, ship_cx, ship_base_y, ship->alt_px, ship->anim_frame);
    }

    /* 4. Starship Vessel Sprite (48x128 px) */
    int32_t sprite_x = ship_cx - (SPRITE_STARSHIP_W / 2);
    int32_t sprite_y = ship_base_y - SPRITE_STARSHIP_H;

    if (sprite_x + SPRITE_STARSHIP_W > 0 && sprite_x < surf->width &&
        sprite_y + SPRITE_STARSHIP_H > 0 && sprite_y < surf->height) {
        moon_blit_sprite(surf, sprite_x, sprite_y, &g_surf_starship, MOON_KEY_COLOR);

        /* Cold-gas RCS puffs during approach */
        if (ship->state == MOON_STARSHIP_APPROACH && ((ship->anim_frame >> 2) & 1)) {
            /* Nose canard puff */
            moon_blit_pixel(surf, ship_cx - 14, sprite_y + 18, COLOR_LASER_CYAN);
            moon_blit_pixel(surf, ship_cx - 15, sprite_y + 18, COLOR_LUNAR_WHITE);
            /* Aft flap puff */
            moon_blit_pixel(surf, ship_cx + 18, sprite_y + 98, COLOR_LASER_CYAN);
            moon_blit_pixel(surf, ship_cx + 19, sprite_y + 98, COLOR_LUNAR_WHITE);
        }
    }

    /* 5. Cryogenic Umbilical Arm */
    if (ship->state == MOON_STARSHIP_SERVICING || ship->state == MOON_STARSHIP_PRELAUNCH) {
        render_umbilical_arm(surf, pad_cx, pad_cy, ship->umbilical_pct, ship->anim_frame);
    }

    /* 6. Cryogenic Loading Meter */
    if (ship->state == MOON_STARSHIP_SERVICING) {
        render_cryo_meter(surf, ship_cx, sprite_y,
                          ship->lh2_loaded_kg, ship->lh2_target_kg,
                          ship->lox_loaded_kg, ship->lox_target_kg,
                          ship->turnaround_timer_ms,
                          ship->anim_frame);
    }
}
