/* drivers/audio_jh7110.c — StarFive VisionFive 2 Audio Subsystem
 *
 * Implements hardware audio output for the StarFive JH7110 SoC (VisionFive 2 v1.3B)
 * through the onboard 3.5mm stereo headphone/line-out jack via the JH7110
 * hardware PWMDAC (Pulse-Width Modulation Digital-to-Analog Converter).
 *
 * Hardware Overview:
 * - PWMDAC Controller: MMIO at 0x100B0000
 * - Pinmux: GPIO 33 (Left Channel, Signal 28), GPIO 34 (Right Channel, Signal 29)
 * - Clock / Reset: SYS_CRG PWMDAC APB (clk 157, 0x274) / Core (clk 158, 0x278)
 *                  and SYSCRG_RESET_ASSERT3 (rst 96, bit 0)
 *
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "drivers/audio_jh7110.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
#include "fs/vfs.h"
#include "lugalos_config.h"

#include <string.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* Hardware Base Addresses */
#define JH7110_SYS_CRG_BASE             0x13020000UL
#define JH7110_SYS_IOMUX_BASE           0x13040000UL
#define JH7110_PWMDAC_BASE              0x100B0000UL

/* SYS_CRG Clocks and Resets */
#define CRG_CLK_PWMDAC_APB              (JH7110_SYS_CRG_BASE + 0x274UL)
#define CRG_CLK_PWMDAC_CORE             (JH7110_SYS_CRG_BASE + 0x278UL)
#define CRG_RESET_ASSERT3               (JH7110_SYS_CRG_BASE + 0x304UL)
#define CRG_RESET_STATUS3               (JH7110_SYS_CRG_BASE + 0x314UL)

/* PWMDAC Registers */
#define PWMDAC_WDATA                    (JH7110_PWMDAC_BASE + 0x00UL)
#define PWMDAC_CTRL                     (JH7110_PWMDAC_BASE + 0x04UL)
#define PWMDAC_SATAE                    (JH7110_PWMDAC_BASE + 0x08UL)

/* PWMDAC Control Bits */
#define PWMDAC_CTRL_ENABLE              (1u << 0)
#define PWMDAC_CTRL_SHIFT_10BIT         (1u << 1)
#define PWMDAC_CTRL_DUTY_CENTER         (2u << 2)
#define PWMDAC_CTRL_INVERT_MSB          (1u << 14)

/* Static State */
/* guarded by: g_audio_lock */
static ylock_t g_audio_lock;
/* guarded by: g_audio_lock */
static bool g_audio_initialized = false;
/* guarded by: g_audio_lock */
static bool g_audio_enabled = false;
/* guarded by: g_audio_lock */
static int g_audio_volume = AUDIO_JH7110_DEFAULT_VOLUME;

/* 256-entry pure fixed-point sine table (-32767 .. +32767) */
static const int16_t s_sin_tab[256] = {
        0,   804,  1607,  2410,  3211,  4011,  4807,  5601,
     6392,  7179,  7961,  8739,  9511, 10278, 11038, 11792,
    12539, 13278, 14009, 14732, 15446, 16150, 16845, 17530,
    18204, 18867, 19519, 20159, 20787, 21402, 22004, 22594,
    23169, 23731, 24278, 24811, 25329, 25831, 26318, 26789,
    27244, 27683, 28105, 28510, 28897, 29268, 29621, 29955,
    30272, 30571, 30851, 31113, 31356, 31580, 31785, 31970,
    32137, 32284, 32412, 32520, 32609, 32678, 32727, 32757,
    32767, 32757, 32727, 32678, 32609, 32520, 32412, 32284,
    32137, 31970, 31785, 31580, 31356, 31113, 30851, 30571,
    30272, 29955, 29621, 29268, 28897, 28510, 28105, 27683,
    27244, 26789, 26318, 25831, 25329, 24811, 24278, 23731,
    23169, 22594, 22004, 21402, 20787, 20159, 19519, 18867,
    18204, 17530, 16845, 16150, 15446, 14732, 14009, 13278,
    12539, 11792, 11038, 10278,  9511,  8739,  7961,  7179,
     6392,  5601,  4807,  4011,  3211,  2410,  1607,   804,
        0,  -804, -1607, -2410, -3211, -4011, -4807, -5601,
    -6392, -7179, -7961, -8739, -9511,-10278,-11038,-11792,
   -12539,-13278,-14009,-14732,-15446,-16150,-16845,-17530,
   -18204,-18867,-19519,-20159,-20787,-21402,-22004,-22594,
   -23169,-23731,-24278,-24811,-25329,-25831,-26318,-26789,
   -27244,-27683,-28105,-28510,-28897,-29268,-29621,-29955,
   -30272,-30571,-30851,-31113,-31356,-31580,-31785,-31970,
   -32137,-32284,-32412,-32520,-32609,-32678,-32727,-32757,
   -32767,-32757,-32727,-32678,-32609,-32520,-32412,-32284,
   -32137,-31970,-31785,-31580,-31356,-31113,-30851,-30571,
   -30272,-29955,-29621,-29268,-28897,-28510,-28105,-27683,
   -27244,-26789,-26318,-25831,-25329,-24811,-24278,-23731,
   -23169,-22594,-22004,-21402,-20787,-20159,-19519,-18867,
   -18204,-17530,-16845,-16150,-15446,-14732,-14009,-13278,
   -12539,-11792,-11038,-10278, -9511, -8739, -7961, -7179,
    -6392, -5601, -4807, -4011, -3211, -2410, -1607,  -804
};

static inline int16_t fixed_sine(uint16_t phase) {
    return s_sin_tab[(phase >> 8) & 0xFFu];
}

/* Tri-state the pin drivers when idle to prevent PWM switching whine or DC bias */
static void hw_pin_mute(bool mute) {
    uint32_t doen2 = REG32(JH7110_SYS_IOMUX_BASE + 0x20u);
    doen2 &= ~((0x3Fu << 8) | (0x3Fu << 16));
    if (mute) {
        doen2 |= (1u << 8) | (1u << 16); /* OEN_HIGH = 1 (Hi-Z, muted) */
    }
    REG32(JH7110_SYS_IOMUX_BASE + 0x20u) = doen2;
}

bool audio_jh7110_init(void) {
    ylock_acquire(&g_audio_lock);
    if (g_audio_initialized) {
        ylock_release(&g_audio_lock);
        return true;
    }

    /* 1. Enable SYS_CRG Clocks for PWMDAC (APB and Core) */
    REG32(CRG_CLK_PWMDAC_APB) = (1u << 31);
    /* Core clock divider: 12 (nominal 12.288 MHz / 256 = 48 kHz base) */
    REG32(CRG_CLK_PWMDAC_CORE) = (1u << 31) | (12u & 0xFFFFFFu);

    /* 2. Deassert PWMDAC Reset in SYSCRG_RESET_ASSERT3 (Reset ID 96 -> bit 0) */
    REG32(CRG_RESET_ASSERT3) &= ~(1u << 0);
    time_delay_us(500);

    /* 3. Configure SYS_IOMUX Pinmux for 3.5mm Jack (GPIO 33=L, GPIO 34=R) */
    /* DOUT register 0x60 (GPIO 32..35):
     * GPIO 33 = 28 (GPO_PWMDAC0_LEFT_OUTPUT)
     * GPIO 34 = 29 (GPO_PWMDAC0_RIGHT_OUTPUT) */
    uint32_t dout2 = REG32(JH7110_SYS_IOMUX_BASE + 0x60u);
    dout2 &= ~((0x7Fu << 8) | (0x7Fu << 16));
    dout2 |= (28u << 8) | (29u << 16);
    REG32(JH7110_SYS_IOMUX_BASE + 0x60u) = dout2;

    /* Pad configuration: push-pull outputs (0x00) */
    REG32(JH7110_SYS_IOMUX_BASE + 0x120u + 33u * 4u) = 0x00u;
    REG32(JH7110_SYS_IOMUX_BASE + 0x120u + 34u * 4u) = 0x00u;

    /* Keep pin drivers muted in Hi-Z until active playback */
    hw_pin_mute(true);

    /* Controller kept disabled when idle */
    REG32(PWMDAC_CTRL) = 0;

    g_audio_initialized = true;
    g_audio_enabled = false;
    ylock_release(&g_audio_lock);

    printk("[Audio] StarFive JH7110 PWMDAC Online (3.5mm Stereo Jack, GPIO 33/34, Muted)\n");
    return true;
}

bool audio_jh7110_enable(bool on) {
    ylock_acquire(&g_audio_lock);
    if (!g_audio_initialized) {
        ylock_release(&g_audio_lock);
        return false;
    }
    if (on) {
        hw_pin_mute(false);
        uint32_t ctrl = PWMDAC_CTRL_ENABLE | PWMDAC_CTRL_SHIFT_10BIT |
                        PWMDAC_CTRL_DUTY_CENTER | PWMDAC_CTRL_INVERT_MSB;
        REG32(PWMDAC_CTRL) = ctrl;
        g_audio_enabled = true;
    } else {
        REG32(PWMDAC_CTRL) = 0;
        hw_pin_mute(true);
        g_audio_enabled = false;
    }
    ylock_release(&g_audio_lock);
    return true;
}

bool audio_jh7110_is_enabled(void) {
    ylock_acquire(&g_audio_lock);
    bool en = g_audio_enabled;
    ylock_release(&g_audio_lock);
    return en;
}

void audio_jh7110_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ylock_acquire(&g_audio_lock);
    g_audio_volume = percent;
    ylock_release(&g_audio_lock);
}

int audio_jh7110_get_volume(void) {
    ylock_acquire(&g_audio_lock);
    int vol = g_audio_volume;
    ylock_release(&g_audio_lock);
    return vol;
}

static void pwmdac_start_locked(uint32_t sample_rate) {
    /* Adjust PWMDAC Core Clock divider if necessary for sample rate */
    uint32_t div = 12u; /* default for 44.1k / 48k */
    if (sample_rate <= 16000) {
        div = 36u;
    } else if (sample_rate <= 24000) {
        div = 24u;
    }
    REG32(CRG_CLK_PWMDAC_CORE) = (1u << 31) | (div & 0xFFFFFFu);

    /* Un-mute pins and enable controller for playback */
    hw_pin_mute(false);
    uint32_t ctrl = PWMDAC_CTRL_ENABLE | PWMDAC_CTRL_SHIFT_10BIT |
                    PWMDAC_CTRL_DUTY_CENTER | PWMDAC_CTRL_INVERT_MSB;
    REG32(PWMDAC_CTRL) = ctrl;
    g_audio_enabled = true;
}

static void pwmdac_write_frames_locked(const int16_t *samples, uint32_t num_frames, int vol) {
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t raw_l = samples[2 * i];
        int16_t raw_r = samples[2 * i + 1];

        /* Fixed-point digital volume scaling (0..100) */
        int16_t scaled_l = (int16_t)(((int32_t)raw_l * vol) / 100);
        int16_t scaled_r = (int16_t)(((int32_t)raw_r * vol) / 100);

        uint32_t frame_val = ((uint32_t)(uint16_t)scaled_l) |
                             (((uint32_t)(uint16_t)scaled_r) << 16);

        /* Wait while FIFO is full (bit 1 of PWMDAC_SATAE) */
        int timeout = 50000;
        while ((REG32(PWMDAC_SATAE) & 0x02u) != 0 && --timeout > 0) {
            time_delay_us(2);
        }
        if (timeout == 0) {
            /* FIFO stall recover */
            REG32(PWMDAC_CTRL) |= PWMDAC_CTRL_ENABLE;
        }

        REG32(PWMDAC_WDATA) = frame_val;
    }
}

static void pwmdac_stop_locked(void) {
    /* Drain interpolator with silence frames */
    for (int k = 0; k < 64; k++) {
        int timeout = 50000;
        while ((REG32(PWMDAC_SATAE) & 0x02u) != 0 && --timeout > 0) {
            time_delay_us(2);
        }
        REG32(PWMDAC_WDATA) = 0;
    }
    time_delay_us(2000);

    /* Return to muted/disabled state when playback finishes */
    REG32(PWMDAC_CTRL) = 0;
    hw_pin_mute(true);
    g_audio_enabled = false;
}

bool audio_jh7110_play_pcm(const int16_t *samples, uint32_t num_frames, uint32_t sample_rate) {
    if (!samples || num_frames == 0) return false;
    if (!audio_jh7110_init()) return false;

    ylock_acquire(&g_audio_lock);
    int vol = g_audio_volume;
    pwmdac_start_locked(sample_rate);
    pwmdac_write_frames_locked(samples, num_frames, vol);
    pwmdac_stop_locked();
    ylock_release(&g_audio_lock);
    return true;
}

void audio_jh7110_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0 || duration_ms == 0) return;
    if (!audio_jh7110_init()) return;

    const uint32_t sample_rate = AUDIO_JH7110_DEFAULT_RATE;
    uint32_t total_samples = (sample_rate * duration_ms) / 1000u;
    uint32_t phase_inc = (uint32_t)(((uint64_t)freq_hz << 16) / sample_rate);
    uint32_t phase = 0;

    /* Ramp envelope (5 ms attack, 5 ms decay) to avoid speaker pop */
    uint32_t ramp_samples = (sample_rate * 5u) / 1000u;
    if (ramp_samples > total_samples / 2) ramp_samples = total_samples / 2;

    int16_t chunk[256 * 2]; /* 256 stereo frames */
    uint32_t played = 0;

    ylock_acquire(&g_audio_lock);
    int vol = g_audio_volume;
    pwmdac_start_locked(sample_rate);

    while (played < total_samples) {
        uint32_t chunk_len = total_samples - played;
        if (chunk_len > 256) chunk_len = 256;

        for (uint32_t i = 0; i < chunk_len; i++) {
            uint32_t cur = played + i;
            int32_t env = 32767;

            if (cur < ramp_samples) {
                env = (int32_t)((32767ULL * cur) / ramp_samples);
            } else if (cur > total_samples - ramp_samples) {
                uint32_t remain = total_samples - cur;
                env = (int32_t)((32767ULL * remain) / ramp_samples);
            }

            int16_t s = (int16_t)(((int32_t)fixed_sine((uint16_t)phase) * env) >> 15);
            chunk[2 * i] = s;
            chunk[2 * i + 1] = s;

            phase = (phase + phase_inc) & 0xFFFFu;
        }

        pwmdac_write_frames_locked(chunk, chunk_len, vol);
        played += chunk_len;
    }

    pwmdac_stop_locked();
    ylock_release(&g_audio_lock);
}

bool audio_jh7110_play_wav(const char *path) {
    if (!path || !*path) return false;

    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        cprintf("audio: unable to open file '%s'\n", path);
        return false;
    }

    uint8_t hdr[44];
    int nread = vfs_pread(fd, hdr, 44, 0);
    if (nread < 44) {
        cprintf("audio: '%s' is too small for a WAV header\n", path);
        vfs_close(fd);
        return false;
    }

    /* Check RIFF / WAVE signature */
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        cprintf("audio: '%s' is not a valid RIFF WAVE file\n", path);
        vfs_close(fd);
        return false;
    }

    uint16_t audio_fmt = (uint16_t)(hdr[20] | (hdr[21] << 8));
    uint16_t channels  = (uint16_t)(hdr[22] | (hdr[23] << 8));
    uint32_t srate     = (uint32_t)(hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24));
    uint16_t bits      = (uint16_t)(hdr[34] | (hdr[35] << 8));

    if (audio_fmt != 1 || bits != 16 || (channels != 1 && channels != 2)) {
        cprintf("audio: '%s' unsupported format (fmt=%u, chan=%u, bits=%u; require 16-bit PCM)\n",
                path, audio_fmt, channels, bits);
        vfs_close(fd);
        return false;
    }

    cprintf("[Audio] Playing '%s': %u Hz, %s, 16-bit PCM (Vol %d%%)\n",
            path, (unsigned)srate, channels == 2 ? "Stereo" : "Mono", audio_jh7110_get_volume());

    if (!audio_jh7110_init()) {
        vfs_close(fd);
        return false;
    }

    uint8_t raw_buf[1024];
    int16_t stereo_buf[512 * 2];
    uint64_t offset = 44;

    ylock_acquire(&g_audio_lock);
    int vol = g_audio_volume;
    pwmdac_start_locked(srate);

    while ((nread = vfs_pread(fd, raw_buf, sizeof(raw_buf), offset)) > 0) {
        int frames = 0;
        if (channels == 2) {
            frames = nread / 4;
            const int16_t *src = (const int16_t *)(const void *)raw_buf;
            pwmdac_write_frames_locked(src, (uint32_t)frames, vol);
        } else {
            frames = nread / 2;
            const int16_t *src = (const int16_t *)(const void *)raw_buf;
            for (int i = 0; i < frames; i++) {
                stereo_buf[2 * i] = src[i];
                stereo_buf[2 * i + 1] = src[i];
            }
            pwmdac_write_frames_locked(stereo_buf, (uint32_t)frames, vol);
        }
        offset += (uint64_t)nread;
    }

    pwmdac_stop_locked();
    ylock_release(&g_audio_lock);

    vfs_close(fd);
    return true;
}

void audio_jh7110_status(void) {
    audio_jh7110_init();

    ylock_acquire(&g_audio_lock);
    bool en = g_audio_enabled;
    int vol = g_audio_volume;
    uint32_t ctrl = REG32(PWMDAC_CTRL);
    uint32_t st = REG32(PWMDAC_SATAE);
    uint32_t clk_apb = REG32(CRG_CLK_PWMDAC_APB);
    uint32_t clk_core = REG32(CRG_CLK_PWMDAC_CORE);
    ylock_release(&g_audio_lock);

    cprintf("\nStarFive JH7110 Audio Subsystem (PWMDAC):\n");
    cprintf("  Hardware Engine:   PWMDAC (0x100B0000)\n");
    cprintf("  Output Jack:       3.5mm Stereo Headphone / Line-Out (GPIO 33/34)\n");
    cprintf("  State:             %s (CTRL: 0x%08x)\n", en ? "ENABLED" : "MUTED", (unsigned)ctrl);
    cprintf("  Volume:            %d%%\n", vol);
    cprintf("  FIFO Status:       %s (SATAE: 0x%08x)\n",
            (st & 0x02) ? "FULL" : "READY", (unsigned)st);
    cprintf("  Clocks:            APB=0x%08x, Core=0x%08x\n\n",
            (unsigned)clk_apb, (unsigned)clk_core);
}

/* --- Shell Commands --- */

void volume_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        cprintf("volume: %d%%\n", audio_jh7110_get_volume());
        return;
    }
    int vol = 0;
    for (; *arg >= '0' && *arg <= '9'; arg++) {
        vol = vol * 10 + (*arg - '0');
    }
    audio_jh7110_set_volume(vol);
    cprintf("volume: set to %d%%\n", audio_jh7110_get_volume());
}

void audio_cmd(const char *arg) {
    while (*arg == ' ') arg++;

    if (!*arg || strcmp(arg, "status") == 0) {
        audio_jh7110_status();
        return;
    }

    if (strncmp(arg, "beep", 4) == 0) {
        const char *p = arg + 4;
        while (*p == ' ') p++;
        uint32_t freq = 440;
        uint32_t dur = 500;
        if (*p) {
            freq = 0;
            while (*p >= '0' && *p <= '9') { freq = freq * 10 + (*p++ - '0'); }
            while (*p == ' ') p++;
            if (*p) {
                dur = 0;
                while (*p >= '0' && *p <= '9') { dur = dur * 10 + (*p++ - '0'); }
            }
        }
        if (freq == 0) freq = 440;
        if (dur == 0) dur = 500;
        cprintf("audio: beeping %u Hz for %u ms...\n", (unsigned)freq, (unsigned)dur);
        audio_jh7110_beep(freq, dur);
        return;
    }

    if (strncmp(arg, "volume", 6) == 0) {
        volume_cmd(arg + 6);
        return;
    }

    if (strcmp(arg, "mute") == 0 || strcmp(arg, "off") == 0) {
        audio_jh7110_enable(false);
        cprintf("audio: muted (PWMDAC idle, pins Hi-Z)\n");
        return;
    }

    if (strcmp(arg, "unmute") == 0 || strcmp(arg, "on") == 0) {
        audio_jh7110_enable(true);
        cprintf("audio: unmuted (PWMDAC active)\n");
        return;
    }

    if (strncmp(arg, "play ", 5) == 0) {
        audio_jh7110_play_wav(arg + 5);
        return;
    }

    cprintf("Usage: audio [status | beep [freq_hz] [ms] | volume [0..100] | mute | unmute | play <file.wav>]\n");
}

void play_cmd(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        cprintf("Usage: play <file.wav>\n");
        return;
    }
    audio_jh7110_play_wav(arg);
}
