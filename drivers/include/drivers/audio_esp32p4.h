/* drivers/include/drivers/audio_esp32p4.h — ESP32-P4 Hardware Audio Subsystem
 *
 * Provides bare-metal hardware audio streaming for ESP32-P4 targets (Waveshare
 * LCD-7B, M5Stack Tab5, ESP32-P4-NANO). Manages I2S0, AHB-GDMA circular DMA
 * descriptors, hardware audio codecs (ES8311, ES8388), and speaker power
 * amplifiers (NS4150B via PI4IOE1 on Tab5 or GPIO 53 on LCD-7B/NANO).
 *
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#ifndef AUDIO_ESP32P4_H
#define AUDIO_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_P4_SAMPLE_RATE     16000
#define AUDIO_P4_CHUNK_SAMPLES   512
#define AUDIO_P4_CHUNK_BYTES     (AUDIO_P4_CHUNK_SAMPLES * 2 * (int)sizeof(int16_t)) /* 2048 bytes */
#define AUDIO_P4_NUM_DESC        12
#define AUDIO_P4_BUFFER_MS       ((AUDIO_P4_CHUNK_SAMPLES * AUDIO_P4_NUM_DESC * 1000) / AUDIO_P4_SAMPLE_RATE) /* 384 ms */

/* Board-tuned amplitude headroom (pure fixed-point):
 * Waveshare LCD-7B has a high-sensitivity external speaker and NS4150B at unity gain.
 * M5Stack Tab5 has an ultra-compact internal miniature speaker requiring +4.5dB analog
 * gain, high single-note headroom for melodies, and modest chord peak to prevent multi-tone
 * transducer intermodulation in its small acoustic chamber. */
#if defined(CONFIG_BOARD_TAB5)
#define AUDIO_P4_DEFAULT_PEAK    20000
#define AUDIO_P4_CHORD_PEAK      10000
#else
#define AUDIO_P4_DEFAULT_PEAK    12000
#define AUDIO_P4_CHORD_PEAK      12000
#endif

/* Callback to render stereo 16-bit interleaved PCM samples [L0, R0, L1, R1, ...] */
typedef void (*audio_p4_render_fn)(int16_t *stereo_buf, int samples, void *user);

/* Initialize hardware peripherals: I2S0, AHB-GDMA, Codec (ES8311 or ES8388) */
bool audio_p4_init(void);

/* Power amplifier control (NS4150B): true = active, false = muted/standby */
bool audio_p4_amp_set(bool on);
int  audio_p4_amp_get(void);

/* Set / get hardware audio volume [0..100] percent */
void audio_p4_set_volume(int percent);
int  audio_p4_get_volume(void);

/* Continuous circular streaming API (for games, music, and interactive audio):
 * audio_p4_start() powers on amplifier and starts DMA circular loop.
 * audio_p4_pump() checks if in-flight descriptor is ready; refills non-blockingly.
 * audio_p4_stop() gracefully drains descriptors, mutes amplifier, and stops DMA. */
bool audio_p4_start(audio_p4_render_fn render_fn, void *user);
void audio_p4_pump(void);
void audio_p4_stop(void);
bool audio_p4_is_playing(void);

/* One-shot blocking stream playback (used by audiotest) */
void audio_p4_play_stream(void (*synth_fn)(int16_t *buf, uint32_t samples, void *user),
                          void *user, uint32_t total_samples);

/* Codec register diagnostic and debugging interface */
void        audio_p4_codec_dump(void);
bool        audio_p4_codec_write(uint8_t reg, uint8_t val);
bool        audio_p4_codec_read(uint8_t reg, uint8_t *val);
const char *audio_p4_codec_name(void);

/* Shared pure fixed-point sine table (returns -32767 .. +32767) */
int16_t audio_p4_sine(uint16_t phase);

#endif /* AUDIO_ESP32P4_H */
