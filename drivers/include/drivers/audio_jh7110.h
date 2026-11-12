/* drivers/include/drivers/audio_jh7110.h — VisionFive 2 Audio Subsystem
 *
 * Implements audio playback for the StarFive JH7110 SoC (VisionFive 2 v1.3B)
 * targeting the onboard 3.5mm stereo headphone/line-out jack via the JH7110
 * hardware PWMDAC (Pulse-Width Modulation Digital-to-Analog Converter).
 *
 * Supports pure fixed-point tone synthesis, 16-bit PCM playback, RIFF WAV
 * decoding from /sd0/ or /nvme0/, and volume control.
 *
 * Freestanding C, zero floating-point math (pure integer/fixed-point).
 */

#ifndef AUDIO_JH7110_H
#define AUDIO_JH7110_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AUDIO_JH7110_DEFAULT_RATE    44100
#define AUDIO_JH7110_DEFAULT_VOLUME  75

/* Callback type for continuous audio stream rendering: [L0, R0, L1, R1, ...] */
typedef void (*audio_jh7110_render_fn)(int16_t *stereo_buf, int samples, void *user);

/* Initialize hardware peripherals: SYS_CRG clocks, resets, IOMUX pins, and PWMDAC */
bool audio_jh7110_init(void);

/* Power/standby control */
bool audio_jh7110_enable(bool on);
bool audio_jh7110_is_enabled(void);

/* Volume control [0..100] percent */
void audio_jh7110_set_volume(int percent);
int  audio_jh7110_get_volume(void);

/* Blocking PCM playback: samples contains interleaved stereo [L0, R0, L1, R1, ...] */
bool audio_jh7110_play_pcm(const int16_t *samples, uint32_t num_frames, uint32_t sample_rate);

/* Generate pure fixed-point sine wave tone through 3.5mm jack */
void audio_jh7110_beep(uint32_t freq_hz, uint32_t duration_ms);

/* Play a 16-bit PCM WAV file from the filesystem */
bool audio_jh7110_play_wav(const char *path);

/* Diagnostic status dump */
void audio_jh7110_status(void);

/* Shell command entry points */
void audio_cmd(const char *arg);
void play_cmd(const char *arg);
void volume_cmd(const char *arg);

#endif /* AUDIO_JH7110_H */
