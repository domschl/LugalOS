/* drivers/ui_sound.c -- UI sounds (drivers/include/drivers/ui_sound.h). */

#include "drivers/ui_sound.h"
#include "kernel/settings.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_ESP32P4)
#include "drivers/audio_esp32p4.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#define CLICK_SAMPLES 96u       /* 6 ms at 16 kHz */
#define CLICK_HZ      2400u
#define CLICK_IDLE_MS 4000u     /* the stream stays up this long after the last click */

/* accepted: clicks asked for and when the last was -- any task adds one, the
 * stream's render takes them; a click counted twice or late is a click. */
static volatile uint32_t g_clicks;
static volatile uint64_t g_click_ms;
static volatile int      g_click_pid = -1;      /* accepted: set once, by the first click */
static volatile bool     g_click_starting;      /* accepted: an atomic exchange picks who starts the task */
/* single task: the click task (its pumping renders the stream). */
static uint32_t g_click_pos = CLICK_SAMPLES;    /* where in the click the stream is; CLICK_SAMPLES: silence */
static uint32_t g_click_phase;

/* The stream: silence, and a click wherever one was asked for -- a tone
 * falling off over CLICK_SAMPLES. */
static void click_render(int16_t *buf, int samples, void *user) {
    (void)user;
    const uint32_t inc = (uint32_t)(((uint64_t)CLICK_HZ << 32) / AUDIO_P4_SAMPLE_RATE);
    for (int i = 0; i < samples; i++) {
        if (g_click_pos >= CLICK_SAMPLES && __atomic_load_n(&g_clicks, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_sub(&g_clicks, 1u, __ATOMIC_ACQ_REL);
            g_click_pos = 0;
            g_click_phase = 0;
        }
        int16_t v = 0;
        if (g_click_pos < CLICK_SAMPLES) {
            int32_t amp = (int32_t)(AUDIO_P4_DEFAULT_PEAK / 2) * (int32_t)(CLICK_SAMPLES - g_click_pos) / (int32_t)CLICK_SAMPLES;
            v = (int16_t)(((int32_t)audio_p4_sine((uint16_t)(g_click_phase >> 16)) * amp) >> 15);
            g_click_phase += inc;
            g_click_pos++;
        }
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
}

/* Starts the short stream at the first click (the amplifier takes ~150 ms
 * to come up, once), pumps it while clicks come, and lets it go after
 * CLICK_IDLE_MS of quiet. A program's sound is never interrupted: with one
 * playing, clicks are dropped. */
static void click_task(void *arg) {
    (void)arg;
    for (;;) {
        task_prepare_block();
        if (!__atomic_load_n(&g_clicks, __ATOMIC_ACQUIRE)) task_block();
        task_wait_done();
        if (!audio_p4_is_owner(click_render) &&
            (audio_p4_is_playing() || !audio_p4_start_lowlat(click_render, NULL))) {
            __atomic_store_n(&g_clicks, 0u, __ATOMIC_RELEASE);
            continue;
        }
        while (time_get_ms() - g_click_ms < CLICK_IDLE_MS && audio_p4_is_owner(click_render)) {
            audio_p4_pump();
            task_sleep_ms(3);
        }
        audio_p4_release(click_render);
    }
}

void ui_click(void) {
    if (settings_get("ui.click_sound") != 1) return;
    g_click_ms = time_get_ms();
    __atomic_fetch_add(&g_clicks, 1u, __ATOMIC_ACQ_REL);
    int pid = g_click_pid;
    if (pid >= 0) {
        (void)task_unblock(pid);
        return;
    }
    if (__atomic_exchange_n(&g_click_starting, true, __ATOMIC_ACQ_REL)) return;
    g_click_pid = task_create("uiclick", click_task, NULL);
}

#else

void ui_click(void) { }

#endif
