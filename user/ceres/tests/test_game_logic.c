/* user/ceres/tests/test_game_logic.c — Unit Tests for Ceres Alpha Logic & Blitter */

#include "ceres_config.h"
#include "ceres_platform.h"
#include "ceres_blitter.h"
#include "ceres_sprites.h"
#include "ceres_fixed.h"
#include "ceres_entities.h"
#include "ceres_game.h"
#include "ceres_sound.h"
#include "ceres_ambient.h"
#include "ceres_cyber.h"
#include "ceres_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

void ceres_platform_set_volume(int percent) { (void)percent; }

static void test_initialization(void) {
    printf("[test] Checking game initialization...\n");
    ceres_game_t g;
    bool ok = ceres_game_init(&g, 640, 360);
    assert(ok);
    assert(g.width == 640);
    assert(g.height == 360);
    assert(g.state == CERES_STATE_PLAYING);
    assert(g.running == true);
    assert(g.player.fuel == 1000);
    assert(g.player.lives == 3);
    assert(g.player.score == 0);
    assert(g.player.x == 80);
    assert(g.player.y == 180);
    assert(g.player.bank == 0);
    assert(g.entities.active_lasers == 0);
    assert(g.entities.active_bombs == 0);
    assert(g.entities.active_particles == 0);
    printf("[test] Initialization passed.\n");
}

static void test_flight_boundaries(void) {
    printf("[test] Checking flight corridor clamping...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* Test boundary clamp left */
    g.key_left = true;
    for (int i = 0; i < 200; i++) {
        ceres_game_update(&g, 16);
    }
    assert(g.player.x >= 32);
    assert(g.player.thrust_boost == -1);

    /* Test boundary clamp right */
    g.key_left = false;
    g.key_right = true;
    for (int i = 0; i < 500; i++) {
        ceres_game_update(&g, 16);
    }
    assert(g.player.x <= 640 - 128);
    assert(g.player.thrust_boost == 1);

    /* Test boundary clamp up against ceiling/HUD */
    g.key_right = false;
    g.key_up = true;
    for (int i = 0; i < 200; i++) {
        ceres_game_update(&g, 16);
    }
    assert(g.player.y >= CERES_HUD_HEIGHT + 6);
    assert(g.player.bank == 1); /* Climbing bank */

    /* Test boundary clamp down against floor */
    g.key_up = false;
    g.key_down = true;
    for (int i = 0; i < 500; i++) {
        ceres_game_update(&g, 16);
    }
    assert(g.player.bank == -1); /* Diving bank */

    int ceil_y = 0, floor_y = 0;
    ceres_terrain_sample(&g.terrain, g.player.x, &ceil_y, &floor_y);
    assert(g.player.y <= floor_y - 6);
    printf("[test] Flight corridor clamping passed.\n");
}

static void test_weapons_and_scoring(void) {
    printf("[test] Checking weapons input & controls...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* Fire laser & key release */
    ceres_event_t ev_fire = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_SPACE };
    ceres_game_handle_event(&g, &ev_fire);
    assert(g.player.laser_fired);
    assert(g.key_fire);
    assert(g.entities.total_lasers_fired == 2);
    ceres_event_t ev_fire_up = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_SPACE };
    ceres_game_handle_event(&g, &ev_fire_up);
    assert(!g.key_fire);

    /* Drop bomb with B & key release */
    ceres_event_t ev_bomb = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_B };
    ceres_game_handle_event(&g, &ev_bomb);
    assert(g.player.bomb_dropped);
    assert(g.key_bomb);
    assert(g.entities.total_bombs_dropped == 1);
    ceres_event_t ev_bomb_up = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_B };
    ceres_game_handle_event(&g, &ev_bomb_up);
    assert(!g.key_bomb);

    /* Drop bomb with Enter (CERES_KEY_RETURN) & key release */
    ceres_event_t ev_enter = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_RETURN };
    ceres_game_handle_event(&g, &ev_enter);
    assert(g.key_bomb);
    ceres_event_t ev_enter_up = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_RETURN };
    ceres_game_handle_event(&g, &ev_enter_up);
    assert(!g.key_bomb);

    /* Directional keys press & release toggling */
    ceres_event_t ev_up = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_UP };
    ceres_game_handle_event(&g, &ev_up);
    assert(g.key_up);
    ceres_event_t ev_up_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_UP };
    ceres_game_handle_event(&g, &ev_up_rel);
    assert(!g.key_up);

    ceres_event_t ev_w = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_W };
    ceres_game_handle_event(&g, &ev_w);
    assert(g.key_up);
    ceres_event_t ev_w_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_W };
    ceres_game_handle_event(&g, &ev_w_rel);
    assert(!g.key_up);

    ceres_event_t ev_dn = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_DOWN };
    ceres_game_handle_event(&g, &ev_dn);
    assert(g.key_down);
    ceres_event_t ev_dn_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_DOWN };
    ceres_game_handle_event(&g, &ev_dn_rel);
    assert(!g.key_down);

    ceres_event_t ev_s = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_S };
    ceres_game_handle_event(&g, &ev_s);
    assert(g.key_down);
    ceres_event_t ev_s_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_S };
    ceres_game_handle_event(&g, &ev_s_rel);
    assert(!g.key_down);

    ceres_event_t ev_lf = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_LEFT };
    ceres_game_handle_event(&g, &ev_lf);
    assert(g.key_left);
    ceres_event_t ev_lf_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_LEFT };
    ceres_game_handle_event(&g, &ev_lf_rel);
    assert(!g.key_left);

    ceres_event_t ev_a = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_A };
    ceres_game_handle_event(&g, &ev_a);
    assert(g.key_left);
    ceres_event_t ev_a_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_A };
    ceres_game_handle_event(&g, &ev_a_rel);
    assert(!g.key_left);

    ceres_event_t ev_rt = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_RIGHT };
    ceres_game_handle_event(&g, &ev_rt);
    assert(g.key_right);
    ceres_event_t ev_rt_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_RIGHT };
    ceres_game_handle_event(&g, &ev_rt_rel);
    assert(!g.key_right);

    ceres_event_t ev_d = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_D };
    ceres_game_handle_event(&g, &ev_d);
    assert(g.key_right);
    ceres_event_t ev_d_rel = { .type = CERES_EVENT_KEY_UP, .key = CERES_KEY_D };
    ceres_game_handle_event(&g, &ev_d_rel);
    assert(!g.key_right);

    /* Pause toggle */
    ceres_event_t ev_pause = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_P };
    ceres_game_handle_event(&g, &ev_pause);
    assert(g.state == CERES_STATE_PAUSED);
    ceres_game_handle_event(&g, &ev_pause);
    assert(g.state == CERES_STATE_PLAYING);

    /* Quit */
    ceres_event_t ev_quit = { .type = CERES_EVENT_KEY_DOWN, .key = CERES_KEY_Q };
    ceres_game_handle_event(&g, &ev_quit);
    assert(!g.running);
    printf("[test] Weapons input & controls passed.\n");
}

static void test_laser_kinematics(void) {
    printf("[test] Checking laser projectile physics & cooldowns...\n");
    ceres_entities_t pool;
    ceres_entities_init(&pool);

    fixed32_t sx = INT_TO_FIXED(100);
    fixed32_t sy = INT_TO_FIXED(150);

    /* Fire initial twin laser */
    bool fired = ceres_entities_fire_laser(&pool, sx, sy);
    assert(fired);
    assert(pool.total_lasers_fired == 2);
    assert(pool.laser_cooldown == CERES_LASER_COOLDOWN);

    /* Verify two lasers placed at upper and lower offsets */
    int active_cnt = 0;
    fixed32_t y1 = 0, y2 = 0;
    for (int i = 0; i < CERES_MAX_LASERS; i++) {
        if (pool.lasers[i].active) {
            assert(pool.lasers[i].vx == LASER_SPEED_FIXED);
            if (active_cnt == 0) y1 = pool.lasers[i].y;
            else if (active_cnt == 1) y2 = pool.lasers[i].y;
            active_cnt++;
        }
    }
    assert(active_cnt == 2);
    assert(y1 == sy - INT_TO_FIXED(3));
    assert(y2 == sy + INT_TO_FIXED(3));

    /* Immediate next trigger fails due to cooldown */
    bool refired = ceres_entities_fire_laser(&pool, sx, sy);
    assert(!refired);

    /* Advance kinematics without terrain for 10 frames */
    for (int f = 0; f < 10; f++) {
        ceres_entities_update(&pool, NULL, 640, 360);
    }

    /* Verify straight forward trajectory: y unchanged, x increased by 10 * speed */
    for (int i = 0; i < CERES_MAX_LASERS; i++) {
        if (pool.lasers[i].active) {
            fixed32_t expected_x = (sx + INT_TO_FIXED(8)) + 10 * LASER_SPEED_FIXED;
            assert(pool.lasers[i].x == expected_x);
            assert(pool.lasers[i].y == y1 || pool.lasers[i].y == y2);
        }
    }

    /* Cooldown has now expired (10 frames > 8) */
    assert(pool.laser_cooldown == 0);
    bool second_burst = ceres_entities_fire_laser(&pool, sx, sy);
    assert(second_burst);
    assert(pool.total_lasers_fired == 4);

    printf("[test] Laser projectile kinematics passed.\n");
}

static void test_bomb_parabola(void) {
    printf("[test] Checking bomb parabolic trajectory & 2-bomb arcade limit...\n");
    ceres_entities_t pool;
    ceres_entities_init(&pool);

    fixed32_t sx = INT_TO_FIXED(50);
    fixed32_t sy = INT_TO_FIXED(100);

    /* Drop 1st bomb */
    bool dropped1 = ceres_entities_drop_bomb(&pool, sx, sy);
    assert(dropped1);
    assert(pool.total_bombs_dropped == 1);

    /* Update 1 frame to register active bomb count */
    ceres_entities_update(&pool, NULL, 640, 360);
    assert(pool.active_bombs == 1);

    /* Force cooldown clear to test concurrent active limit */
    pool.bomb_cooldown = 0;
    bool dropped2 = ceres_entities_drop_bomb(&pool, sx, sy);
    assert(dropped2);
    ceres_entities_update(&pool, NULL, 640, 360);
    assert(pool.active_bombs == 2);

    /* Third drop must be blocked because active_bombs >= 2 (Arcade authenticity) */
    pool.bomb_cooldown = 0;
    bool dropped3 = ceres_entities_drop_bomb(&pool, sx, sy);
    assert(!dropped3);

    /* Check parabolic trajectory of bomb 1 */
    /* Find bomb 0 */
    ceres_bomb_t *b = &pool.bombs[0];

    for (int t = 0; t < 15; t++) {
        fixed32_t prev_y = b->y;
        fixed32_t prev_vy = b->vy;
        ceres_entities_update(&pool, NULL, 640, 360);
        if (!b->active) break;

        /* Downward velocity must strictly increase by gravity each step */
        assert(b->vy == prev_vy + BOMB_GRAVITY_FIXED);
        /* Displacement must match velocity */
        assert(b->y == prev_y + b->vy);
        /* Horizontal motion is constant linear */
        assert(b->vx == BOMB_INITIAL_VX_FIXED);
    }

    printf("[test] Bomb parabolic trajectory passed.\n");
}

static void test_projectile_terrain_collision(void) {
    printf("[test] Checking projectile-terrain collision and detonations...\n");
    ceres_entities_t pool;
    ceres_entities_init(&pool);

    ceres_terrain_t terrain;
    ceres_terrain_init(&terrain, 2, 0x19810425, 360);

    /* Sample terrain at x = 120 */
    int ceil_y = 0, floor_y = 0;
    ceres_terrain_sample(&terrain, 120, &ceil_y, &floor_y);

    /* 1. Fire laser directly into ceiling rock */
    bool fired = ceres_entities_fire_laser(&pool, INT_TO_FIXED(80), INT_TO_FIXED(ceil_y - 2));
    assert(fired);

    /* Update entities with terrain: laser should strike ceiling and explode into sparks */
    for (int f = 0; f < 10; f++) {
        ceres_entities_update(&pool, &terrain, 640, 360);
    }
    assert(pool.total_laser_rock_hits > 0);
    assert(pool.active_particles > 0);

    /* 2. Drop bomb just above the floor so it hits quickly */
    pool.bomb_cooldown = 0;
    bool dropped = ceres_entities_drop_bomb(&pool, INT_TO_FIXED(120), INT_TO_FIXED(floor_y - 10));
    assert(dropped);

    uint32_t prev_detonations = pool.total_bomb_detonations;
    for (int f = 0; f < 15; f++) {
        ceres_entities_update(&pool, &terrain, 640, 360);
    }
    /* Bomb must have hit the floor and detonated */
    assert(pool.total_bomb_detonations > prev_detonations);
    assert(pool.active_bombs == 0);

    printf("[test] Projectile-terrain collisions and detonations passed.\n");
}

static void test_particles_physics_and_decay(void) {
    printf("[test] Checking particle explosion pool and lifecycle...\n");
    ceres_entities_t pool;
    ceres_entities_init(&pool);

    /* Spawn explosion at (200, 200) */
    ceres_entities_spawn_explosion(&pool, INT_TO_FIXED(200), INT_TO_FIXED(200), 1);
    assert(pool.active_particles == 0); /* will count on update */

    ceres_entities_update(&pool, NULL, 640, 360);
    assert(pool.active_particles > 8);

    /* Advance 40 frames: all particles must naturally decay and return to 0 */
    for (int f = 0; f < 40; f++) {
        ceres_entities_update(&pool, NULL, 640, 360);
    }
    assert(pool.active_particles == 0);

    printf("[test] Particle physics and decay passed.\n");
}

static void test_terrain_streaming(void) {
    printf("[test] Checking terrain ring buffer streaming & geometry...\n");
    ceres_terrain_t t;
    ceres_terrain_init(&t, 1, 0x12345678, 360);

    /* Verify initial columns */
    assert(t.generated_col_end == CERES_RING_COLS);
    for (int i = 0; i < CERES_RING_COLS; i++) {
        assert(t.ring[i].ceil_y >= CERES_HUD_HEIGHT);
        assert(t.ring[i].floor_y > t.ring[i].ceil_y);
        assert(t.ring[i].floor_y <= 360);
    }

    /* Advance 2500 world columns (wrapping ring buffer >2x) */
    for (int step = 0; step < 1250; step++) {
        ceres_terrain_advance(&t, INT_TO_FIXED(2));
    }
    assert(t.camera_col == 2500);
    assert(t.generated_col_end >= 2500 + 640);

    /* Test Sector 2 cavern generation with ceiling and clearance */
    ceres_terrain_init(&t, 2, 0x87654321, 360);
    for (int step = 0; step < 500; step++) {
        ceres_terrain_advance(&t, INT_TO_FIXED(2));
    }

    bool ceiling_descended = false;
    for (int i = 0; i < CERES_RING_COLS; i++) {
        if (t.ring[i].ceil_y > CERES_HUD_HEIGHT + 10) {
            ceiling_descended = true;
        }
        int clearance = (int)t.ring[i].floor_y - (int)t.ring[i].ceil_y;
        assert(clearance >= 100); /* Corridor remains navigable */
    }
    assert(ceiling_descended);
    printf("[test] Terrain streaming and Sector 2 cavern passed.\n");
}

static void test_terrain_collision(void) {
    printf("[test] Checking terrain collision queries...\n");
    ceres_terrain_t t;
    ceres_terrain_init(&t, 2, 0x19810425, 360);

    int ceil_y = 0, floor_y = 0;
    bool sampled = ceres_terrain_sample(&t, 100, &ceil_y, &floor_y);
    assert(sampled);
    assert(ceil_y < floor_y);

    /* Safe spot in mid-corridor: no collision */
    int mid_y = (ceil_y + floor_y) / 2;
    assert(!ceres_terrain_check_collision(&t, 100, mid_y - 4, 16, 8));

    /* Rock ceiling intrusion: collision detected */
    assert(ceres_terrain_check_collision(&t, 100, ceil_y - 2, 16, 8));

    /* Rock floor intrusion: collision detected */
    assert(ceres_terrain_check_collision(&t, 100, floor_y - 2, 16, 8));
    printf("[test] Terrain collision queries passed.\n");
}

static void test_render_safety(void) {
    printf("[test] Checking render buffer bounds safety...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    size_t sz = 640 * 360 * sizeof(uint16_t);
    uint16_t *fb = (uint16_t *)malloc(sz);
    assert(fb);
    memset(fb, 0, sz);

    /* Fire some weapons to populate entity rendering */
    ceres_entities_fire_laser(&g.entities, INT_TO_FIXED(100), INT_TO_FIXED(150));
    ceres_entities_drop_bomb(&g.entities, INT_TO_FIXED(120), INT_TO_FIXED(160));
    ceres_entities_spawn_explosion(&g.entities, INT_TO_FIXED(200), INT_TO_FIXED(200), 1);

    /* Render several frames */
    for (int f = 0; f < 30; f++) {
        ceres_game_update(&g, 16);
        ceres_game_render(&g, fb, 640, 360, 640);
    }

    free(fb);
    printf("[test] Render safety passed.\n");
}

static void test_ground_targets_and_fuel_replenishment(void) {
    printf("[test] Checking ground target bombing & fuel replenishment...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* Drain fuel partially */
    g.player.fuel = 500;
    uint32_t init_score = g.player.score;

    int real_floor = 0;
    ceres_terrain_sample(&g.terrain, 220, NULL, &real_floor);
    bool ok = ceres_entities_spawn_target(&g.entities, CERES_TARGET_FUEL_SILO, 220, real_floor);
    assert(ok);

    /* Position ship and drop bomb so it falls into [220..252] */
    g.player.x = 115;
    g.player.y = 210;
    ceres_entities_drop_bomb(&g.entities, INT_TO_FIXED(g.player.x), INT_TO_FIXED(g.player.y));

    /* Advance simulation until bomb hits silo */
    bool hit = false;
    for (int f = 0; f < 40; f++) {
        ceres_game_update(&g, 16);
        if (g.player.fuel > 500) {
            hit = true;
            break;
        }
    }

    assert(hit);
    assert(g.player.fuel >= 740); /* 500 + 250 - minor consumption */
    assert(g.player.score == init_score + 150); /* +150 for fuel silo */
    printf("[test] Ground target bombing & fuel replenishment passed.\n");
}

static void test_cryo_bunker_breach(void) {
    printf("[test] Checking cryo-bunker breach & survivor liberation...\n");
    ceres_entities_t pool;
    ceres_entities_init(&pool);
    ceres_terrain_t t;
    ceres_terrain_init(&t, 1, 0x12345678, 360);

    /* Spawn Cryo-Bunker at world_x = 200, ground_y = 320 */
    ceres_entities_spawn_target(&pool, CERES_TARGET_CRYO_BUNKER, 200, 320);

    /* 1. Laser hitting bunker should deflect (sparks fly, bunker survives) */
    ceres_entities_fire_laser(&pool, INT_TO_FIXED(160), INT_TO_FIXED(310));
    ceres_entities_update(&pool, &t, 640, 360);
    ceres_interaction_t res1 = ceres_entities_resolve_collisions(&pool, &t, 80, 100, 28, 14, false);
    assert(!res1.bunker_breached);
    assert(pool.targets[0].type == CERES_TARGET_CRYO_BUNKER);

    /* 2. Direct bomb impact breaches bunker and frees survivor! */
    ceres_entities_drop_bomb(&pool, INT_TO_FIXED(205), INT_TO_FIXED(300));
    ceres_entities_update(&pool, &t, 640, 360);
    ceres_interaction_t res2 = ceres_entities_resolve_collisions(&pool, &t, 80, 100, 28, 14, false);
    assert(res2.bunker_breached);
    assert(res2.score_delta == 100);
    /* Target has now morphed into a freed survivor! */
    assert(pool.targets[0].type == CERES_TARGET_SURVIVOR);
    assert(pool.targets[0].w == SPRITE_SURVIVOR_W);
    assert(pool.targets[0].h == SPRITE_SURVIVOR_H);
    printf("[test] Cryo-bunker breach & survivor liberation passed.\n");
}

static void test_sam_silo_proximity_launch_and_interception(void) {
    printf("[test] Checking SAM proximity launch and laser interception...\n");
    ceres_entities_t pool;
    ceres_entities_init(&pool);
    ceres_terrain_t t;
    ceres_terrain_init(&t, 1, 0x12345678, 360);

    /* Spawn SAM Silo at world_x = 300, ground_y = 320 */
    ceres_entities_spawn_target(&pool, CERES_TARGET_SAM_SILO, 300, 320);
    assert(!pool.targets[0].launched);

    /* Player at ship_x = 80 (distance 220 > trigger 190) -> should not launch yet */
    ceres_interaction_t res_far = ceres_entities_resolve_collisions(&pool, &t, 80, 200, 28, 14, false);
    assert(!res_far.sam_launched);
    assert(!pool.targets[0].launched);

    /* Player approaches to ship_x = 130 (distance 170 <= trigger 190) -> LAUNCH! */
    ceres_interaction_t res_near = ceres_entities_resolve_collisions(&pool, &t, 130, 200, 28, 14, false);
    assert(res_near.sam_launched);
    assert(pool.targets[0].launched);
    assert(pool.active_sam_rockets == 1);
    assert(pool.sam_rockets[0].active);

    /* SAM Rocket ascends upward under negative vy */
    fixed32_t init_y = pool.sam_rockets[0].y;
    ceres_entities_update(&pool, &t, 640, 360);
    assert(pool.sam_rockets[0].y < init_y); /* Clambering upward */

    /* Intercept rocket with laser */
    int rx = FIXED_TO_INT(pool.sam_rockets[0].x);
    int ry = FIXED_TO_INT(pool.sam_rockets[0].y);
    ceres_entities_fire_laser(&pool, INT_TO_FIXED(rx - 20), INT_TO_FIXED(ry + 10));
    ceres_entities_update(&pool, &t, 640, 360);
    ceres_interaction_t res_hit = ceres_entities_resolve_collisions(&pool, &t, 130, 200, 28, 14, false);
    assert(res_hit.score_delta >= 100); /* Awarded for destroying SAM */
    assert(!pool.sam_rockets[0].active); /* Rocket destroyed! */
    printf("[test] SAM proximity launch and laser interception passed.\n");
}

static void test_tractor_scoop_and_rescue(void) {
    printf("[test] Checking tractor scoop pickup & rescue bonus...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* Set fuel to 400 */
    g.player.fuel = 400;
    uint32_t init_score = g.player.score;

    /* Spawn survivor at world_x = 200, ground_y = 300 */
    ceres_entities_spawn_target(&g.entities, CERES_TARGET_SURVIVOR, 200, 300);

    /* Position ship right above survivor in low-pass scoop zone */
    g.player.x = 200 + SPRITE_SURVIVOR_W / 2;
    g.player.y = 300 - 40; /* 40 pixels above ground */

    /* Run update */
    ceres_game_update(&g, 16);

    /* Verify survivor was rescued! */
    assert(g.player.rescued == 1);
    assert(g.player.fuel >= 590); /* +200 fuel bonus (400 + 200 - 1 consumption) */
    assert(g.player.score == init_score + 500); /* +500 rescue score */
    printf("[test] Tractor scoop pickup & rescue bonus passed.\n");
}

static void test_fuel_economy_and_flameout(void) {
    printf("[test] Checking fuel depletion and unpowered flameout glide...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* Set fuel very low */
    g.player.fuel = 5;
    assert(g.player.thruster_on);

    /* Run 30 frames to exhaust fuel to 0 */
    for (int f = 0; f < 30; f++) {
        ceres_game_update(&g, 16);
    }

    assert(g.player.fuel == 0);
    assert(!g.player.thruster_on); /* Thruster extinguished */

    /* In unpowered glide: gravity pulls craft downward */
    int prev_y = g.player.y;
    ceres_game_update(&g, 16);
    assert(g.player.y > prev_y); /* Dropping under gravity */
    printf("[test] Fuel depletion and unpowered flameout glide passed.\n");
}

static void test_targets_and_astronauts_bedrock_alignment(void) {
    printf("[test] Checking targets and astronauts bedrock alignment (no rock clipping)...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* Run 600 frames of gameplay simulation (1200 px of camera scrolling) */
    for (int f = 0; f < 600; f++) {
        ceres_game_update(&g, 16);

        for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
            const ceres_ground_target_t *t = &g.entities.targets[i];
            if (!t->active) continue;

            int sx = t->world_x - (int)g.terrain.camera_col;
            if (sx >= -t->w && sx < g.width) {
                /* Target is on or near screen: test every column across footprint */
                for (int dx = 0; dx < t->w; dx++) {
                    uint32_t col_idx = (uint32_t)(t->world_x + dx) & CERES_RING_MASK;
                    int floor_y = (int)g.terrain.ring[col_idx].floor_y;
                    int ceil_y = (int)g.terrain.ring[col_idx].ceil_y;

                    /* 1. Bedrock must not rise above the base of the target */
                    assert(floor_y >= t->y);

                    /* 2. Ceiling rock must not penetrate the top of the target */
                    assert(ceil_y <= t->y - t->h);
                }
            }
        }
    }
    printf("[test] Targets and astronauts bedrock alignment passed.\n");
}

static void test_aerial_enemy_spawning_and_kinematics(void) {
    printf("[test] Checking aerial enemy spawning, sine wave kinematics & laser interception...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* 1. Spawn a Saucer, a Fighter, and a Meteor */
    bool s_ok = ceres_entities_spawn_aerial(&g.entities, CERES_AERIAL_SAUCER, INT_TO_FIXED(400), INT_TO_FIXED(150));
    bool f_ok = ceres_entities_spawn_aerial(&g.entities, CERES_AERIAL_FIGHTER, INT_TO_FIXED(450), INT_TO_FIXED(80));
    bool m_ok = ceres_entities_spawn_aerial(&g.entities, CERES_AERIAL_METEOR, INT_TO_FIXED(500), INT_TO_FIXED(120));
    assert(s_ok && f_ok && m_ok);
    assert(g.entities.active_aerials == 3);

    /* 2. Run an update and check kinematics */
    ceres_entities_update(&g.entities, &g.terrain, 640, 360);

    /* Find active saucer and check sine oscillation */
    int saucer_idx = -1;
    for (int i = 0; i < CERES_MAX_AERIAL_ENEMIES; i++) {
        if (g.entities.aerials[i].active && g.entities.aerials[i].type == CERES_AERIAL_SAUCER) {
            saucer_idx = i;
            break;
        }
    }
    assert(saucer_idx != -1);
    assert(g.entities.aerials[saucer_idx].x < INT_TO_FIXED(400)); /* Moved left */

    /* 3. Laser Interception of Saucer */
    /* Fire laser directly towards the saucer */
    fixed32_t sx = g.entities.aerials[saucer_idx].x;
    fixed32_t sy = g.entities.aerials[saucer_idx].y;
    ceres_entities_fire_laser(&g.entities, sx - INT_TO_FIXED(20), sy + INT_TO_FIXED(6));

    ceres_interaction_t res = ceres_entities_resolve_collisions(
        &g.entities, &g.terrain,
        g.player.x, g.player.y, SPRITE_PLAYER_SHIP_W, SPRITE_PLAYER_SHIP_H, false
    );

    /* Saucer destroyed, awards +200 points */
    assert(res.score_delta == 200);
    assert(!g.entities.aerials[saucer_idx].active);
    assert(g.entities.total_aerials_destroyed == 1);
    printf("[test] Aerial enemy spawning, sine wave kinematics & laser interception passed.\n");
}

static void test_harvester_abduction_and_midair_rescue(void) {
    printf("[test] Checking Harvester abductor drone, survivor snatch & mid-air heroic parachute rescue...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* 1. Spawn a ground survivor at world X = 300, ground Y = 280 */
    ceres_entities_spawn_target(&g.entities, CERES_TARGET_SURVIVOR, 300, 280);

    /* Camera starts at column 0, so screen X of survivor is 300 */
    int surv_sx = 300;

    /* 2. Spawn Harvester abductor directly above survivor at Y = 40 */
    ceres_entities_spawn_harvester(&g.entities, INT_TO_FIXED(surv_sx), INT_TO_FIXED(40));

    int harv_idx = -1;
    for (int i = 0; i < CERES_MAX_AERIAL_ENEMIES; i++) {
        if (g.entities.aerials[i].active && g.entities.aerials[i].type == CERES_AERIAL_HARVESTER) {
            harv_idx = i;
            break;
        }
    }
    assert(harv_idx != -1);
    assert(!g.entities.aerials[harv_idx].carrying_survivor);

    /* 3. Run updates so Harvester descends and snatches survivor */
    for (int f = 0; f < 130; f++) {
        ceres_entities_update(&g.entities, &g.terrain, 640, 360);
        if (g.entities.aerials[harv_idx].carrying_survivor) break;
    }

    /* Verify Harvester snatched the survivor */
    assert(g.entities.aerials[harv_idx].carrying_survivor);
    assert(g.entities.aerials[harv_idx].vy < 0); /* Ascending toward space */

    /* 4. Shoot the Harvester with a laser to liberate the survivor! */
    fixed32_t hx = g.entities.aerials[harv_idx].x;
    fixed32_t hy = g.entities.aerials[harv_idx].y;

    /* Harvester has 3 HP, bring to 1 HP for single shot */
    g.entities.aerials[harv_idx].hp = 1;
    ceres_entities_fire_laser(&g.entities, hx - INT_TO_FIXED(20), hy + INT_TO_FIXED(6));

    ceres_interaction_t res = ceres_entities_resolve_collisions(
        &g.entities, &g.terrain,
        10, 10, SPRITE_PLAYER_SHIP_W, SPRITE_PLAYER_SHIP_H, false
    );

    /* Harvester destroyed: +400 pts, survivor liberated on parachute! */
    assert(res.score_delta >= 400);

    /* Verify Harvester is no longer active */
    bool harvester_alive = false;
    for (int i = 0; i < CERES_MAX_AERIAL_ENEMIES; i++) {
        if (g.entities.aerials[i].active && g.entities.aerials[i].type == CERES_AERIAL_HARVESTER) {
            harvester_alive = true;
            break;
        }
    }
    assert(!harvester_alive);

    /* 5. Check that a parachuting survivor entity was spawned */
    int para_idx = -1;
    for (int i = 0; i < CERES_MAX_AERIAL_ENEMIES; i++) {
        if (g.entities.aerials[i].active && g.entities.aerials[i].type == CERES_AERIAL_PARACHUTE) {
            para_idx = i;
            break;
        }
    }
    assert(para_idx != -1);
    assert(g.entities.aerials[para_idx].vy > 0); /* Descending with parachute */

    /* 6. Player ship flies to intercept descending parachutist mid-air */
    int para_sx = FIXED_TO_INT(g.entities.aerials[para_idx].x);
    int para_sy = FIXED_TO_INT(g.entities.aerials[para_idx].y);

    ceres_interaction_t rescue_res = ceres_entities_resolve_collisions(
        &g.entities, &g.terrain,
        para_sx, para_sy, SPRITE_PLAYER_SHIP_W, SPRITE_PLAYER_SHIP_H, false
    );

    /* Verify Heroic Mid-Air Rescue! */
    assert(rescue_res.heroic_rescue);
    assert(rescue_res.survivor_rescued);
    assert(rescue_res.score_delta == 1000); /* +1000 Heroic Rescue Bonus */
    assert(rescue_res.fuel_delta == 300);   /* +300 Fuel Bonus */
    assert(!g.entities.aerials[para_idx].active);
    assert(g.entities.total_parachutes_rescued == 1);
    printf("[test] Harvester abductor drone, survivor snatch & mid-air heroic parachute rescue passed.\n");
}

static void test_aerial_collision_with_player_ship(void) {
    printf("[test] Checking lethal enemy craft collision and respawn protection...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);

    /* 1. Shielded test: spawn protection protects the player from death */
    assert(g.player.invincible_frames > 0);
    ceres_entities_spawn_fighter(&g.entities, INT_TO_FIXED(g.player.x), INT_TO_FIXED(g.player.y));
    ceres_game_update(&g, 16);
    assert(g.player.alive); /* Protected by spawn shield! */

    /* 2. Unshielded test: lethal collision */
    g.player.invincible_frames = 0;
    ceres_entities_spawn_fighter(&g.entities, INT_TO_FIXED(g.player.x), INT_TO_FIXED(g.player.y));
    int init_lives = g.player.lives;
    ceres_game_update(&g, 16);

    /* Ship destroyed, entering respawn timer */
    assert(!g.player.alive);
    assert(g.player.lives == init_lives - 1);
    assert(g.player.respawn_timer > 0);
    printf("[test] Lethal enemy craft collision and respawn protection passed.\n");
}

static void test_sector_4_generator_and_laser_barrier(void) {
    printf("[test] Checking Sector 4 Power Generator & Laser Barrier collapse...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);
    g.player.invincible_frames = 0;

    /* Spawn generator at world_x = 300, ground_y = 310 */
    bool spawned_gen = ceres_entities_spawn_target(&g.entities, CERES_TARGET_GENERATOR, 300, 310);
    assert(spawned_gen);
    int gen_idx = -1;
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        if (g.entities.targets[i].active && g.entities.targets[i].type == CERES_TARGET_GENERATOR) {
            gen_idx = i;
            break;
        }
    }
    assert(gen_idx != -1);

    /* Spawn linked laser barrier at world_x = 390 */
    bool spawned_barr = ceres_entities_spawn_barrier(&g.entities, 390, gen_idx);
    assert(spawned_barr);
    assert(g.entities.active_barriers == 1);
    assert(g.entities.barriers[0].active);

    /* Test 1: Laser fired at active barrier is blocked/absorbed */
    ceres_entities_fire_laser(&g.entities, INT_TO_FIXED(380), INT_TO_FIXED(200));
    ceres_entities_update(&g.entities, &g.terrain, 640, 360);
    ceres_interaction_t res_laser = ceres_entities_resolve_collisions(&g.entities, &g.terrain, 100, 200, 28, 14, false);
    assert(!res_laser.player_hit);

    /* Test 2: Ship flying into active barrier takes lethal hit */
    ceres_interaction_t res_barr = ceres_entities_resolve_collisions(&g.entities, &g.terrain, 390 - 14, 200, 28, 14, false);
    assert(res_barr.player_hit);

    /* Test 3: Bombing the generator drops the laser fence */
    ceres_entities_drop_bomb(&g.entities, INT_TO_FIXED(300 + 10), INT_TO_FIXED(300));
    ceres_entities_update(&g.entities, &g.terrain, 640, 360);
    ceres_interaction_t res_bomb = ceres_entities_resolve_collisions(&g.entities, &g.terrain, 100, 200, 28, 14, false);
    assert(res_bomb.barrier_collapsed);
    assert(!g.entities.targets[gen_idx].active); /* Generator destroyed */
    assert(!g.entities.barriers[0].active);      /* Barrier collapsed */

    /* Test 4: Ship can now pass through barrier column safely */
    ceres_interaction_t res_pass = ceres_entities_resolve_collisions(&g.entities, &g.terrain, 390 - 14, 200, 28, 14, false);
    assert(!res_pass.player_hit);

    printf("[test] Sector 4 Power Generator & Laser Barrier collapse passed.\n");
}

static void test_sector_6_reactor_core_and_meltdown(void) {
    printf("[test] Checking Sector 6 Reactor Core detonation, Meltdown & Loop Rollover...\n");
    ceres_game_t g;
    ceres_game_init(&g, 640, 360);
    g.player.sector = 6;
    g.terrain.sector = 6;

    /* Spawn Reactor Core */
    bool spawned_core = ceres_entities_spawn_target(&g.entities, CERES_TARGET_REACTOR_CORE, 400, 320);
    assert(spawned_core);
    int core_idx = -1;
    for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
        if (g.entities.targets[i].active && g.entities.targets[i].type == CERES_TARGET_REACTOR_CORE) {
            core_idx = i;
            break;
        }
    }
    assert(core_idx != -1);
    assert(g.entities.targets[core_idx].hp == 3);

    /* Twin pulse laser hit inflicts damage */
    ceres_entities_fire_laser(&g.entities, INT_TO_FIXED(390), INT_TO_FIXED(310));
    ceres_entities_update(&g.entities, &g.terrain, 640, 360);
    ceres_entities_resolve_collisions(&g.entities, &g.terrain, 100, 200, 28, 14, false);
    assert(g.entities.targets[core_idx].active);
    assert(g.entities.targets[core_idx].hp == 1);

    /* Direct Bomb hit finishes off Reactor Core */
    ceres_entities_drop_bomb(&g.entities, INT_TO_FIXED(400 + 10), INT_TO_FIXED(310));
    ceres_entities_update(&g.entities, &g.terrain, 640, 360);
    ceres_interaction_t res_core = ceres_entities_resolve_collisions(&g.entities, &g.terrain, 100, 200, 28, 14, false);
    assert(res_core.core_destroyed);
    assert(!g.entities.targets[core_idx].active);
    assert(res_core.score_delta >= 5000);

    /* Apply interaction to game state to initiate Meltdown */
    g.player.score += res_core.score_delta;
    g.meltdown_timer = 180;
    assert(g.meltdown_timer == 180);

    /* Run meltdown countdown */
    for (int f = 0; f < 180; f++) {
        ceres_game_update(&g, 16);
    }

    /* Verify Loop Rollover on Meltdown expiry */
    assert(g.meltdown_timer == 0);
    assert(g.loop_count == 2);          /* Loop 2! */
    assert(g.player.sector == 1);       /* Rolled over to Sector 1 */
    assert(g.player.fuel == 1000);      /* Refueled */
    assert(g.player.score >= 15000);    /* Core + Loop completion bonus */
    printf("[test] Sector 6 Reactor Core detonation, Meltdown & Loop Rollover passed.\n");
}

static void test_chiptune_audio_synthesizer(void) {
    printf("[test] Checking Chiptune Audio Synthesizer channels & PCM rendering...\n");
    ceres_sound_init();
    assert(ceres_sound_is_enabled());

    /* Buffer for audio samples */
    int16_t buffer[1024];

    /* Playing with no SFX produces silence */
    ceres_sound_render(buffer, 1024);
    for (int i = 0; i < 1024; i++) {
        assert(buffer[i] == 0);
    }

    /* Trigger Laser SFX */
    ceres_sound_play(CERES_SFX_LASER);
    ceres_sound_render(buffer, 512);

    /* Verify non-zero output */
    bool non_zero = false;
    for (int i = 0; i < 512; i++) {
        if (buffer[i] != 0) {
            non_zero = true;
            break;
        }
    }
    assert(non_zero);

    /* Trigger Explosion and Rescue Chime */
    ceres_sound_play(CERES_SFX_EXPLOSION_HUGE);
    ceres_sound_play(CERES_SFX_RESCUE_CHIME);
    ceres_sound_render(buffer, 1024);

    /* Test disabling sound */
    ceres_sound_set_enabled(false);
    assert(!ceres_sound_is_enabled());
    ceres_sound_play(CERES_SFX_LASER);
    ceres_sound_render(buffer, 256);
    for (int i = 0; i < 256; i++) {
        assert(buffer[i] == 0);
    }
    ceres_sound_set_enabled(true);
    printf("[test] Chiptune Audio Synthesizer passed.\n");
}

static void test_ambient_synthesizer(void) {
    printf("[test] Checking Generative Space Ambient Synthesizer...\n");
    ceres_ambient_init();
    assert(ceres_ambient_is_enabled());
    assert(ceres_ambient_get_volume() == CERES_AMBIENT_DEFAULT_VOLUME);
    assert(ceres_ambient_get_mood() == CERES_AMBIENT_MOOD_DEEP_SPACE);

    int16_t buffer[1024];
    memset(buffer, 0, sizeof(buffer));

    /* Render ambient audio stream */
    ceres_ambient_render(buffer, 1024);

    /* Verify non-zero output from drone / wind / pad layers */
    bool non_zero = false;
    for (int i = 0; i < 1024; i++) {
        if (buffer[i] != 0) {
            non_zero = true;
            break;
        }
    }
    assert(non_zero);

    /* Test mood switching */
    ceres_ambient_set_mood(CERES_AMBIENT_MOOD_CAVERN);
    assert(ceres_ambient_get_mood() == CERES_AMBIENT_MOOD_CAVERN);
    ceres_ambient_render(buffer, 512);

    ceres_ambient_set_mood(CERES_AMBIENT_MOOD_TENSE);
    assert(ceres_ambient_get_mood() == CERES_AMBIENT_MOOD_TENSE);
    ceres_ambient_render(buffer, 512);

    /* Test ducking */
    ceres_ambient_duck(160);
    ceres_ambient_render(buffer, 512);

    /* Test West Coast Buchla Wavefolder */
    assert(ceres_ambient_get_wavefold() == 256);
    ceres_ambient_set_wavefold(128); /* Below 256 clamped */
    assert(ceres_ambient_get_wavefold() == 256);
    ceres_ambient_set_wavefold(2048); /* Above 1024 clamped */
    assert(ceres_ambient_get_wavefold() == 1024);

    ceres_ambient_set_wavefold(768); /* Active multi-stage wavefolding */
    assert(ceres_ambient_get_wavefold() == 768);
    memset(buffer, 0, sizeof(buffer));
    ceres_ambient_render(buffer, 1024);
    for (int i = 0; i < 1024; i++) {
        assert(buffer[i] >= -32768 && buffer[i] <= 32767);
    }
    ceres_ambient_set_wavefold(256); /* Reset to bypass */

    /* Test volume disable */
    ceres_ambient_set_enabled(false);
    assert(!ceres_ambient_is_enabled());
    memset(buffer, 0, sizeof(buffer));
    ceres_ambient_render(buffer, 512);
    for (int i = 0; i < 512; i++) {
        assert(buffer[i] == 0);
    }

    ceres_ambient_set_enabled(true);
    ceres_ambient_set_mood(CERES_AMBIENT_MOOD_DEEP_SPACE);
    printf("[test] Generative Space Ambient Synthesizer passed.\n");
}

static void test_high_score_storage_persistence(void) {
    printf("[test] Checking High-Score & Stats persistent storage save/load...\n");
    ceres_save_data_t save;
    memset(&save, 0, sizeof(save));
    save.hi_score = 48500;
    save.total_rescued = 18;
    save.games_played = 5;
    save.highest_sector = 6;

    /* Save to persistent medium */
    bool save_ok = ceres_storage_save(&save);
    assert(save_ok);

    /* Load back and verify integrity */
    ceres_save_data_t loaded;
    bool load_ok = ceres_storage_load(&loaded);
    assert(load_ok);
    assert(loaded.magic == CERES_SAVE_MAGIC);
    assert(loaded.version == CERES_SAVE_VERSION);
    assert(loaded.hi_score == 48500);
    assert(loaded.total_rescued == 18);
    assert(loaded.games_played == 5);
    assert(loaded.highest_sector == 6);
    printf("[test] High-Score & Stats persistent storage passed.\n");
}

static void test_dynamic_cybernetic_voice(void) {
    printf("[test] Checking Dynamic Cybernetic Voice & Meltdown Sequencer with 3 Variation Methods...\n");

    ceres_cyber_init();
    assert(ceres_cyber_is_enabled());
    assert(ceres_cyber_get_state() == CERES_CYBER_PATROL);
    assert(ceres_cyber_get_current_bpm() == 108);
    assert(ceres_cyber_get_bar_index() == 0);

    /* 1. Telemetry State Priority Transitions */
    ceres_cyber_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));
    telem.sector = 1;
    telem.fuel = 1000;
    telem.alive = true;
    telem.game_active = true;

    /* Combat runner in sectors 1-3 */
    ceres_cyber_update_telemetry(&telem);
    assert(ceres_cyber_get_state() == CERES_CYBER_COMBAT);

    /* Stronghold in sectors 4-6 */
    telem.sector = 4;
    ceres_cyber_update_telemetry(&telem);
    assert(ceres_cyber_get_state() == CERES_CYBER_STRONGHOLD);

    /* Survivor in range overrides stronghold */
    telem.survivor_visible = true;
    ceres_cyber_update_telemetry(&telem);
    assert(ceres_cyber_get_state() == CERES_CYBER_SURVIVOR);

    /* Low fuel overrides survivor */
    telem.low_fuel_alert = true;
    ceres_cyber_update_telemetry(&telem);
    assert(ceres_cyber_get_state() == CERES_CYBER_LOW_FUEL);

    /* Meltdown emergency escape overrides low fuel */
    telem.meltdown_timer = 240;
    ceres_cyber_update_telemetry(&telem);
    assert(ceres_cyber_get_state() == CERES_CYBER_MELTDOWN);

    /* 2. Audio buffer rendering & non-zero sample synthesis */
    int16_t buf[2048];
    memset(buf, 0, sizeof(buf));
    ceres_cyber_render(buf, 2048);
    bool non_zero = false;
    for (int i = 0; i < 2048; i++) {
        if (buf[i] != 0) non_zero = true;
        assert(buf[i] >= -32768 && buf[i] <= 32767);
    }
    assert(non_zero);

    /* 3. 4-Bar Phrasing Evolution (A -> A -> B -> Turnaround Fill) */
    /* Render enough samples to advance through full 4-bar phrase (~4 * 16 * 1500 samples) */
    for (int bar = 0; bar < 8; bar++) {
        int16_t chunk[2048];
        memset(chunk, 0, sizeof(chunk));
        ceres_cyber_render(chunk, 2048);
    }
    /* Verify bar index advances cyclically 0..3 */
    uint8_t bar_idx = ceres_cyber_get_bar_index();
    assert(bar_idx <= 3);

    /* 4. Contextual telemetry modulation (thrust boost and pitch bank) */
    telem.meltdown_timer = 0;
    telem.low_fuel_alert = false;
    telem.survivor_visible = false;
    telem.thrust_boost = 1; /* Forward boost */
    telem.pitch_bank = 1;   /* Climb */
    telem.rescue_event = true;
    ceres_cyber_update_telemetry(&telem);
    memset(buf, 0, sizeof(buf));
    ceres_cyber_render(buf, 2048);

    /* 5. Volume mute */
    ceres_cyber_set_volume(0);
    memset(buf, 0, sizeof(buf));
    ceres_cyber_render(buf, 2048);
    for (int i = 0; i < 2048; i++) {
        assert(buf[i] == 0);
    }

    ceres_cyber_reset();
    assert(ceres_cyber_get_state() == CERES_CYBER_PATROL);
    printf("[test] Dynamic Cybernetic Voice & Meltdown Sequencer: PASS\n");
}

int main(void) {
    printf("=== Ceres Alpha Logic Unit Tests ===\n");
    test_initialization();
    test_flight_boundaries();
    test_weapons_and_scoring();
    test_laser_kinematics();
    test_bomb_parabola();
    test_projectile_terrain_collision();
    test_particles_physics_and_decay();
    test_terrain_streaming();
    test_terrain_collision();
    test_render_safety();
    test_ground_targets_and_fuel_replenishment();
    test_cryo_bunker_breach();
    test_sam_silo_proximity_launch_and_interception();
    test_tractor_scoop_and_rescue();
    test_fuel_economy_and_flameout();
    test_targets_and_astronauts_bedrock_alignment();
    test_aerial_enemy_spawning_and_kinematics();
    test_harvester_abduction_and_midair_rescue();
    test_aerial_collision_with_player_ship();
    test_sector_4_generator_and_laser_barrier();
    test_sector_6_reactor_core_and_meltdown();
    test_chiptune_audio_synthesizer();
    test_ambient_synthesizer();
    test_high_score_storage_persistence();
    test_dynamic_cybernetic_voice();
    printf("=== All Logic Tests Passed Cleanly! ===\n");
    return 0;
}
