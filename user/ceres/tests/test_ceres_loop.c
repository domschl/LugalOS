/* user/ceres/tests/test_ceres_loop.c — Full 6-Sector Campaign Loop Headless Regression Suite */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ceres_game.h"

void ceres_platform_set_volume(int percent) { (void)percent; }

int main(void) {
    printf("=== Ceres Alpha Full 6-Sector Campaign Headless Test ===\n");

    ceres_game_t g;
    bool ok = ceres_game_init(&g, 640, 360);
    assert(ok);
    assert(g.running);
    assert(g.player.alive);
    assert(g.player.sector == 1);
    assert(g.loop_count == 1);

    printf("[loop] Starting Sector 1: Mountain Approach...\n");

    /* Step through Sector 1 (Length: 1800 columns) */
    while (g.player.sector == 1) {
        /* Keep player ship alive and refueled during campaign traversal */
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        ceres_game_update(&g, 16);
    }
    assert(g.player.sector == 2);
    printf("[loop] Transitioned to Sector 2: Crystal Cavern (col %u)\n", g.terrain.camera_col);

    /* Step through Sector 2 */
    while (g.player.sector == 2) {
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        ceres_game_update(&g, 16);
    }
    assert(g.player.sector == 3);
    printf("[loop] Transitioned to Sector 3: Asteroid Trench (col %u)\n", g.terrain.camera_col);

    /* Step through Sector 3 */
    while (g.player.sector == 3) {
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        ceres_game_update(&g, 16);
    }
    assert(g.player.sector == 4);
    printf("[loop] Transitioned to Sector 4: Fortified Base (col %u)\n", g.terrain.camera_col);

    /* In Sector 4: Fly and bomb installations; verify barrier drop */
    bool barrier_down = false;
    while (g.player.sector == 4) {
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        g.player.x = 140;
        g.player.y = 200;
        g.key_bomb = true;
        g.key_fire = true;

        if (g.entities.total_barriers_down > 0) {
            barrier_down = true;
        }

        ceres_game_update(&g, 16);
    }
    assert(barrier_down);
    printf("[loop] Sector 4 completed. Barriers dropped: %u\n", g.entities.total_barriers_down);
    assert(g.player.sector == 5);
    printf("[loop] Transitioned to Sector 5: The Labyrinth (col %u)\n", g.terrain.camera_col);

    /* Step through Sector 5 */
    while (g.player.sector == 5) {
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        g.player.x = 140;
        g.player.y = 200;
        g.key_bomb = true;
        g.key_fire = true;
        ceres_game_update(&g, 16);
    }
    assert(g.player.sector == 6);
    printf("[loop] Transitioned to Sector 6: Reactor Stronghold (col %u)\n", g.terrain.camera_col);

    /* In Sector 6: Fly and bomb Reactor Core to initiate meltdown */
    int timeout = 0;
    while (g.player.sector == 6 && g.meltdown_timer == 0 && timeout < 3000) {
        timeout++;
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        g.player.x = 140;
        g.player.y = 200;
        g.key_bomb = true;
        g.key_fire = true;

        /* When Reactor Core comes into view, perform precision bombing run */
        static bool s_core_seen = false;
        for (int i = 0; i < CERES_MAX_GROUND_TARGETS; i++) {
            if (g.entities.targets[i].active && g.entities.targets[i].type == CERES_TARGET_REACTOR_CORE) {
                int sx = g.entities.targets[i].world_x - (int)g.terrain.camera_col;
                if (!s_core_seen) {
                    s_core_seen = true;
                    printf("[loop] Core spotted at target idx=%d, world_x=%d, y=%d, camera_col=%u (sx=%d)\n",
                           i, g.entities.targets[i].world_x, g.entities.targets[i].y, g.terrain.camera_col, sx);
                }
                if (sx >= 40 && sx <= g.width - 130) {
                    g.player.x = sx + 4;
                    g.player.y = g.entities.targets[i].y - 32;
                }
                break;
            }
        }

        ceres_game_update(&g, 16);
    }

    assert(g.entities.total_cores_destroyed >= 1);
    assert(g.meltdown_timer > 0);
    printf("[loop] Reactor Core detonated! Meltdown alarm triggered (timer: %u, shake: %u)\n",
           g.meltdown_timer, g.screen_shake);

    /* Run meltdown evacuation rush */
    while (g.meltdown_timer > 0) {
        g.player.fuel = 1000;
        g.player.invincible_frames = 60;
        ceres_game_update(&g, 16);
    }

    /* Verify Loop 1 Completion & Loop 2 Rollover */
    assert(g.loop_count == 2);
    assert(g.player.sector == 1);
    assert(g.player.fuel == 1000);
    assert(g.player.score >= 15000);
    printf("[loop] Meltdown escape rush complete! Rollover to Loop %u, Sector %u (Final Score: %u)\n",
           g.loop_count, g.player.sector, g.player.score);

    printf("=== Full 6-Sector Loop Playthrough Passed with 100%% Success! ===\n");
    return 0;
}
