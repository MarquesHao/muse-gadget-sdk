/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_mem.h"
#include "muse_settings.h"

static const char *TAG = "muse_audio";

#define CHANNELS 2
#define HPF_HZ 80.0f

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static bool s_open;
/* Opening, closing and the volume and gain setters, which other tasks call:
 * with a board-switched codec supply, none may reach a codec mid-switch. */
static SemaphoreHandle_t s_lock;
/* The chunk staging lives in PSRAM, not internal RAM: the chat
 * session spawns an 8 KB-stack task to look the VM up, and with
 * every staging buffer internal the largest free heap block at
 * that moment lands within a few hundred bytes of the task's
 * stack — on the 1.75C it fell short and voice could not connect
 * (measured 2026-11-15; see muse_voice_call.cpp). These buffers
 * are filled and emptied by the CPU around codec reads/writes,
 * never DMA targets. */
EXT_RAM_BSS_ATTR static int16_t s_in_stereo[MUSE_AUDIO_CHUNK * CHANNELS];
EXT_RAM_BSS_ATTR static int16_t s_in_mono[MUSE_AUDIO_CHUNK];
EXT_RAM_BSS_ATTR static int16_t s_out_stereo[MUSE_AUDIO_CHUNK * CHANNELS];

/* One-pole high-pass on the mixed mic signal. */
static float s_hpf_a;
static float s_mic_scale = 1.0f;
static float s_hpf_x1, s_hpf_y1;

#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
/*
 * Microphone voice DSP, this board only. The forensic round (dev record
 * 2026-11-16) cleared every other stage of the voice-note path — the
 * stream is bit-exact PCM, the codec runs stock defaults, both mics are
 * healthy — and pinned the muffled, quiet recordings on the capture
 * itself: a raw, uncompensated take of a voice 20-40 cm away and usually
 * off-axis, at about -36 dBFS where a phone's voice DSP would have
 * lifted the consonant band and ridden the level. This block is that
 * missing processing, in the one place all three consumers of
 * muse_audio_read() share (voice notes, live calls, meeting audio):
 *
 *   high-pass (the shared one above) -> two one-pole high-shelf stages
 *   (+7 dB each over a 1.4 kHz corner, +14 dB of shelf in all) -> a
 *   chunk-wise AGC (20 ms chunks, gain ramped across the chunk from the
 *   previous chunk's measurement, target -20 dBFS, +18/-6 dB range,
 *   frozen while the chunk measures under the -45 dBFS gate so pauses
 *   don't pump the room up) -> a tanh soft limiter under the int16
 *   clamp, so the button clicks and pops the AGC now reaches for are
 *   squashed, not clipped.
 *
 * Constants were tuned offline against real recordings (dev
 * mic-dsp-sim.py replicates this block coefficient for coefficient):
 * the 1-3.4 kHz share of the active speech spectrum goes from ~3% to
 * ~10% and the voice level from ~-34 to ~-21 dBFS. Retune here, in
 * this one place, if a future sample says otherwise.
 *
 * The block between the EXTRACT markers is compiled standalone by
 * tests/test_muse_audio_dsp.py: keep it free of IDF/LVGL identifiers.
 */
/* MIC_DSP_EXTRACT_BEGIN */
#define MIC_EQ_CORNER_HZ 1400.0f
#define MIC_EQ_STAGE_DB 7.0f
#define MIC_AGC_TARGET_DBFS (-20.0f)
#define MIC_AGC_MAX_GAIN_DB 18.0f
#define MIC_AGC_MIN_GAIN_DB (-6.0f)
#define MIC_AGC_GATE_DBFS (-45.0f)
#define MIC_AGC_ATTACK 0.50f    /* per chunk when the gain comes down */
#define MIC_AGC_RELEASE 0.10f   /* per chunk when the gain goes up */
#define MIC_LIMIT_KNEE 0.79f    /* soft-limiter knee, fraction of full scale */

typedef struct {
    float eq_a;         /* one-pole lowpass coefficient, both shelf stages */
    float eq_k;         /* per-stage shelf multiplier (linear gain - 1) */
    float eq_lp1, eq_lp2;
    float gain_from;    /* the gain at the end of the previous chunk */
    float gain_to;      /* the gain this chunk ramps toward */
    float sumsq;        /* the running chunk's EQ'd energy, pre-gain */
    int count;          /* samples in the running chunk */
} mic_dsp_t;

static void mic_dsp_reset(mic_dsp_t *d, float rate)
{
    float rc = 1.0f / (2.0f * (float)M_PI * MIC_EQ_CORNER_HZ);
    float dt = 1.0f / rate;
    d->eq_a = rc / (rc + dt);
    d->eq_k = powf(10.0f, MIC_EQ_STAGE_DB / 20.0f) - 1.0f;
    d->eq_lp1 = d->eq_lp2 = 0.0f;
    d->gain_from = d->gain_to = 1.0f;
    d->sumsq = 0.0f;
    d->count = 0;
}

static float mic_dsp_limit(float y)
{
    const float knee = MIC_LIMIT_KNEE * 32768.0f;
    float a = fabsf(y);
    if (a <= knee) {
        return y;
    }
    float mag = knee + (32768.0f - knee) * tanhf((a - knee) / (32768.0f - knee));
    return y < 0.0f ? -mag : mag;
}

/* One sample in (already high-passed), one out: the two shelf stages,
 * the AGC's ramped gain for this point in the chunk (i of n), and the
 * limiter. The chunk's energy is tallied pre-gain for mic_dsp_chunk_end. */
static float mic_dsp_post(mic_dsp_t *d, float y, int i, int n)
{
    d->eq_lp1 += (1.0f - d->eq_a) * (y - d->eq_lp1);
    float e = y + d->eq_k * (y - d->eq_lp1);
    d->eq_lp2 += (1.0f - d->eq_a) * (e - d->eq_lp2);
    e += d->eq_k * (e - d->eq_lp2);
    d->sumsq += e * e;
    d->count++;
    float g = d->gain_from + (d->gain_to - d->gain_from) * (float)(i + 1) / (float)n;
    return mic_dsp_limit(e * g);
}

/* End of a chunk: ride the gain toward the level the chunk measured.
 * Under the gate the gain freezes — a quiet room keeps the gain the
 * speech earned, it doesn't earn more. */
static void mic_dsp_chunk_end(mic_dsp_t *d)
{
    if (d->count > 0) {
        float rms_db = 10.0f * log10f(d->sumsq / (float)d->count / (32768.0f * 32768.0f) + 1e-12f);
        float desired = d->gain_to;
        if (rms_db >= MIC_AGC_GATE_DBFS) {
            desired = powf(10.0f, (MIC_AGC_TARGET_DBFS - rms_db) / 20.0f);
            float hi = powf(10.0f, MIC_AGC_MAX_GAIN_DB / 20.0f);
            float lo = powf(10.0f, MIC_AGC_MIN_GAIN_DB / 20.0f);
            desired = desired > hi ? hi : desired < lo ? lo : desired;
        }
        float rate = desired < d->gain_to ? MIC_AGC_ATTACK : MIC_AGC_RELEASE;
        float next = d->gain_to + (desired - d->gain_to) * rate;
        d->gain_from = d->gain_to;
        d->gain_to = next;
    }
    d->sumsq = 0.0f;
    d->count = 0;
}
/* MIC_DSP_EXTRACT_END */

static mic_dsp_t s_mic_dsp;
#endif   /* CONFIG_MUSE_BOARD_WAVESHARE_S3_175C */

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void codec_power(bool on)
{
    if (muse_board->codec_power) {
        muse_board->codec_power(on);
    }
}

/* A closed codec on a board-switched supply has no power. */
static bool codecs_reachable(void)
{
    return s_open || !muse_board->codec_power;
}

static void apply_volume(int volume)
{
    if (s_spk && codecs_reachable()) {
        esp_codec_dev_set_out_vol(s_spk, volume);
    }
}

static void apply_mic_gain(int db)
{
    if (muse_board->mic_init || (!s_mic && muse_board->mic_read)) {
        s_mic_scale = powf(10.0f, (float)db / 20.0f);   /* no codec gain: scale samples */
        return;
    }
    if (!s_mic || !codecs_reachable()) {
        return;
    }
    if (muse_board->set_mic_gain) {
        muse_board->set_mic_gain(s_mic, db);
    } else {
        esp_codec_dev_set_in_gain(s_mic, (float)db);
    }
}

static esp_err_t open_codecs(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = MUSE_AUDIO_RATE,
        .channel = CHANNELS,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    if (s_mic) {
        if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
            esp_codec_dev_close(s_spk);
            ESP_LOGE(TAG, "open mic");
            return ESP_FAIL;
        }
    }
    s_open = true;
    return ESP_OK;
}

static esp_err_t read_input(size_t frames)
{
    if (s_mic) {
        return esp_codec_dev_read(s_mic, s_in_stereo, frames * CHANNELS * sizeof(int16_t)) == ESP_CODEC_DEV_OK
            ? ESP_OK : ESP_FAIL;
    }
    /* A mic without a codec beside a speaker codec, for the self-test and
     * loopback test (muse_audio_read reads it directly): mono, its DC
     * dropped before the gain so an offset doesn't saturate the samples. */
    if (!muse_board->mic_read || muse_board->mic_read(s_in_mono, frames) != ESP_OK) {
        return ESP_FAIL;
    }
    float dc = 0;
    for (size_t i = 0; i < frames; i++) {
        dc += s_in_mono[i];
    }
    dc /= (float)frames;
    for (size_t i = 0; i < frames; i++) {
        float x = ((float)s_in_mono[i] - dc) * s_mic_scale;
        int16_t v = (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
        s_in_stereo[2 * i] = v;
        s_in_stereo[2 * i + 1] = v;
    }
    return ESP_OK;
}

esp_err_t muse_audio_init(int volume, int mic_gain_db)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "audio lock");
    esp_err_t err;
    if (muse_board->mic_init) {
        err = muse_board->mic_init();
        if (err == ESP_OK) err = muse_board->mic_power(true);
        if (err != ESP_OK) return err;
        s_open = true;
    } else {
        /* A board-switched supply goes off again on any failure: nothing will
         * open the codecs, and the setters skip them while closed. */
        codec_power(true);
        err = muse_board->audio_init(&s_spk, &s_mic);
        if (err == ESP_OK && !(s_spk && (s_mic || muse_board->mic_read))) {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "codec init failed");
        } else {
            err = open_codecs();   /* logs its own failure */
        }
        if (err != ESP_OK) {
            codec_power(false);
            return err;
        }
    }
    muse_audio_set_volume(volume);
    muse_audio_set_mic_gain(mic_gain_db);

    float rc = 1.0f / (2.0f * (float)M_PI * HPF_HZ);
    float dt = 1.0f / MUSE_AUDIO_RATE;
    s_hpf_a = rc / (rc + dt);

#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
    mic_dsp_reset(&s_mic_dsp, MUSE_AUDIO_RATE);
    ESP_LOGI(TAG, "mic DSP on: shelf 2x%.0f dB over %.0f Hz, AGC to %.0f dBFS (gate %.0f)",
             MIC_EQ_STAGE_DB, MIC_EQ_CORNER_HZ, MIC_AGC_TARGET_DBFS, MIC_AGC_GATE_DBFS);
#endif

    ESP_LOGI(TAG, "audio ready: %d Hz, %d ch, vol %d, mic gain %d dB", MUSE_AUDIO_RATE, CHANNELS, volume, mic_gain_db);
    return ESP_OK;
}

void muse_audio_power(bool on)
{
    lock();
    if (muse_board->mic_init) {
        if (on != s_open && muse_board->mic_power(on) == ESP_OK) s_open = on;
        unlock();
        return;
    }
    if (!s_spk || on == s_open) {
        unlock();
        return;
    }
    /* The shared I2S bus gets disabled twice (once per codec), which logs a
     * harmless error. */
    esp_log_level_t lvl = esp_log_level_get("i2s_common");
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    /* The codecs' supply comes up before they open and goes once both have
     * closed. The amp goes on once they're open and set, and off before they
     * close, so it never passes on their power-up or power-down. */
    if (on) {
        codec_power(true);
        if (open_codecs() == ESP_OK) {
            apply_volume(muse_settings_volume());
            apply_mic_gain(muse_settings_mic_gain());
            if (muse_board->audio_power) {
                muse_board->audio_power(true);
            }
        } else {
            /* Still closed (open_codecs closes the speaker if the mic fails)
             * and the amp still off: take the supply down again too. The
             * next wake tries again. */
            codec_power(false);
        }
    } else {
        if (muse_board->audio_power) {
            muse_board->audio_power(false);
        }
        esp_codec_dev_close(s_spk);
        if (s_mic) {
            esp_codec_dev_close(s_mic);
        }
        s_open = false;
        codec_power(false);
    }
    esp_log_level_set("i2s_common", lvl);
    unlock();
}

void muse_audio_set_volume(int volume)
{
    lock();
    apply_volume(volume);
    unlock();
}

void muse_audio_set_mic_gain(int db)
{
    lock();
    apply_mic_gain(db);
    unlock();
}

static float to_db(double mean_sq)
{
    return 10.0f * log10f((float)(mean_sq / (32768.0 * 32768.0)) + 1e-10f);
}

void muse_audio_selftest(void)
{
    if (muse_board->mic_init) {
        int16_t mono[MUSE_AUDIO_CHUNK];
        if (muse_audio_read(mono, MUSE_AUDIO_CHUNK) == ESP_OK) {
            ESP_LOGI(TAG, "PDM mic capture: %.1f dBFS", muse_audio_dbfs(mono, MUSE_AUDIO_CHUNK));
        }
        return;
    }
    enum { FLUSH = MUSE_AUDIO_RATE / 4, MEASURE = MUSE_AUDIO_RATE / 2 };

    /* Capture: drop the DMA backlog, then time a known number of frames. */
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        read_input(MUSE_AUDIO_CHUNK);
    }
    double sl = 0, sr = 0, slr = 0, ml = 0, mr = 0;
    int pl = 0, pr = 0;
    int64_t t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        read_input(MUSE_AUDIO_CHUNK);
        for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
            int l = s_in_stereo[2 * i], r = s_in_stereo[2 * i + 1];
            ml += l;
            mr += r;
            sl += (double)l * l;
            sr += (double)r * r;
            slr += (double)l * r;
            pl = abs(l) > pl ? abs(l) : pl;
            pr = abs(r) > pr ? abs(r) : pr;
        }
    }
    float cap_hz = MEASURE * 1e6f / (float)(esp_timer_get_time() - t0);
    ml /= MEASURE;
    mr /= MEASURE;
    double vl = sl / MEASURE - ml * ml, vr = sr / MEASURE - mr * mr;
    double corr = (slr / MEASURE - ml * mr) / (sqrt(vl * vr) + 1e-9);

    /* Playback: fill the DMA queue with silence, then time more silence. */
    memset(s_out_stereo, 0, sizeof(s_out_stereo));
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
    }
    t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
    }
    float play_hz = MEASURE * 1e6f / (float)(esp_timer_get_time() - t0);

    ESP_LOGI(TAG, "self-test @ %d dB: mic L %.1f dBFS (peak %d, dc %d), R %.1f dBFS (peak %d, dc %d), corr %.2f",
             muse_settings_mic_gain(), to_db(vl), pl, (int)ml, to_db(vr), pr, (int)mr, corr);
    ESP_LOGI(TAG, "self-test: capture %.0f Hz, playback %.0f Hz (expected %d)", cap_hz, play_hz, MUSE_AUDIO_RATE);
}

/* Goertzel power of `f` in an interleaved stereo buffer, as dBFS of a sine. */
static float tone_db(const int16_t *st, int ch, size_t frames, float f)
{
    float k = 2.0f * cosf(2.0f * (float)M_PI * f / MUSE_AUDIO_RATE);
    float s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; i++) {
        float s0 = st[2 * i + ch] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float p = s1 * s1 + s2 * s2 - k * s1 * s2;
    float amp = 2.0f * sqrtf(p > 0 ? p : 0) / frames;   /* sine amplitude */
    return 20.0f * log10f(amp / 32768.0f + 1e-7f);
}

/*
 * Full-duplex loopback: plays tones on the speaker while recording both mics,
 * at each PGA step. Writes and reads alternate chunk by chunk; the DMA queues
 * absorb the phase offset.
 */
void muse_audio_loopback_test(int volume)
{
    if (muse_board->mic_init) {
        ESP_LOGW(TAG, "no speaker for loopback test");
        return;
    }
    enum { TONE = MUSE_AUDIO_RATE * 3 / 10, SKIP = MUSE_AUDIO_RATE / 10, MEAS = TONE - SKIP };
    static const float freqs[] = { 200, 400, 700, 1000, 2000, 3000, 4000, 6000 };
    static const int gains[] = { 0, 6, 12, 18, 24, 30, 33, 36 };
    const float amp = 0.25f * 32767.0f;   /* -12 dBFS tone */
    int16_t *cap = heap_caps_malloc(MEAS * CHANNELS * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!cap) {
        return;
    }
    muse_audio_set_volume(volume);
    ESP_LOGI(TAG, "loopback: speaker vol %d, tone -12 dBFS; values = mic L/R tone level dBFS", volume);

    for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
        muse_audio_set_mic_gain(gains[g]);
        char line[256];
        int len = snprintf(line, sizeof(line), "gain %2d:", gains[g]);
        int peak = 0;
        /* Silence first for the noise floor, then each tone. */
        for (int t = -1; t < (int)(sizeof(freqs) / sizeof(freqs[0])); t++) {
            float f = t < 0 ? 0 : freqs[t];
            float phase = 0;
            size_t got = 0;
            for (int n = 0; n < TONE + SKIP; n += MUSE_AUDIO_CHUNK) {
                for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
                    int16_t v = f ? (int16_t)(amp * sinf(phase)) : 0;
                    phase += 2.0f * (float)M_PI * f / MUSE_AUDIO_RATE;
                    s_out_stereo[2 * i] = s_out_stereo[2 * i + 1] = v;
                }
                esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
                read_input(MUSE_AUDIO_CHUNK);
                /* Skip the first SKIP*2 frames: DMA latency plus the previous tone's tail. */
                if (n >= 2 * SKIP && got + MUSE_AUDIO_CHUNK <= MEAS) {
                    memcpy(cap + got * CHANNELS, s_in_stereo, sizeof(s_in_stereo));
                    got += MUSE_AUDIO_CHUNK;
                }
            }
            for (size_t i = 0; i < got * CHANNELS; i++) {
                peak = abs(cap[i]) > peak ? abs(cap[i]) : peak;
            }
            if (t < 0) {
                double sl = 0, sr = 0;
                for (size_t i = 0; i < got; i++) {
                    sl += (double)cap[2 * i] * cap[2 * i];
                    sr += (double)cap[2 * i + 1] * cap[2 * i + 1];
                }
                len += snprintf(line + len, sizeof(line) - len, " floor %.0f/%.0f |", to_db(sl / got), to_db(sr / got));
                peak = 0;
            } else {
                len += snprintf(line + len, sizeof(line) - len, " %.0f:%.0f/%.0f", f,
                                tone_db(cap, 0, got, f), tone_db(cap, 1, got, f));
            }
        }
        ESP_LOGI(TAG, "%s | peak %.1f dBFS", line, 20.0 * log10((peak + 1) / 32768.0));
    }
    free(cap);
    muse_audio_set_mic_gain(muse_settings_mic_gain());
    muse_audio_set_volume(muse_settings_volume());
}

esp_err_t muse_audio_read(int16_t *mono, size_t frames)
{
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        /* A mic without a codec: its gain applies in float, ahead of the
         * high-pass, so a DC offset can't saturate the samples first. */
        bool direct = muse_board->mic_init || (!s_mic && muse_board->mic_read);
        if (direct) {
            if (muse_board->mic_read(mono, n) != ESP_OK) return ESP_FAIL;
        } else if (read_input(n) != ESP_OK) {
            return ESP_FAIL;
        }
        for (size_t i = 0; i < n; i++) {
            float x = direct ? (float)mono[i] * s_mic_scale
                     : muse_board->mic_slot < 0 ? 0.5f * ((float)s_in_stereo[2 * i] + (float)s_in_stereo[2 * i + 1])
                                                : (float)s_in_stereo[2 * i + muse_board->mic_slot];
            float y = s_hpf_a * (s_hpf_y1 + x - s_hpf_x1);
            s_hpf_x1 = x;
            s_hpf_y1 = y;
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
            /* Voice DSP (see the block at the top of this file): shelf,
             * ridden gain, soft limiter, ahead of the clamp. */
            y = mic_dsp_post(&s_mic_dsp, y, (int)i, (int)n);
#endif
            mono[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C
        mic_dsp_chunk_end(&s_mic_dsp);
#endif
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

esp_err_t muse_audio_write(const int16_t *mono, size_t frames)
{
    if (muse_board->mic_init) {
        (void)mono;
        vTaskDelay(pdMS_TO_TICKS((frames * 1000 + MUSE_AUDIO_RATE - 1) / MUSE_AUDIO_RATE));
        return ESP_OK;
    }
    /* Speaker off still plays silence at the same pace, so captions timed to
     * the audio scroll as if it were heard. Text mode is silent the same way:
     * replies are shown, never played. */
    bool mute = !muse_settings_speaker_on() || muse_settings_gadget_mode() == MUSE_GADGET_MODE_TEXT;
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        for (size_t i = 0; i < n; i++) {
            s_out_stereo[2 * i] = mute ? 0 : mono[i];
            s_out_stereo[2 * i + 1] = mute ? 0 : mono[i];
        }
        if (esp_codec_dev_write(s_spk, s_out_stereo, n * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

float muse_audio_dbfs(const int16_t *mono, size_t frames)
{
    if (!frames) {
        return -100.0f;
    }
    double acc = 0;
    for (size_t i = 0; i < frames; i++) {
        acc += (double)mono[i] * mono[i];
    }
    float db = to_db(acc / frames);
    return db < -100.0f ? -100.0f : db;
}

float muse_audio_level(const int16_t *mono, size_t frames)
{
    /* Map roughly -54..-12 dBFS onto 0..1. */
    float l = (muse_audio_dbfs(mono, frames) + 54.0f) / 42.0f;
    return l < 0 ? 0 : (l > 1 ? 1 : l);
}

void muse_audio_chirp(int rising)
{
    enum { MS = 90, N = MUSE_AUDIO_RATE * MS / 1000 };
    EXT_RAM_BSS_ATTR static int16_t buf[N];   /* PSRAM: see s_in_stereo above */
    float f0 = rising ? 620.0f : 980.0f;
    float f1 = rising ? 1180.0f : 560.0f;
    float phase = 0;
    for (int i = 0; i < N; i++) {
        float x = (float)i / N;
        float f = f0 + (f1 - f0) * x;
        phase += 2.0f * (float)M_PI * f / MUSE_AUDIO_RATE;
        float env = (x < 0.1f ? x / 0.1f : 1.0f) * (1.0f - x) * (1.0f - x);
        /* A touch of the 3rd harmonic for a soft 8-bit flavour. */
        float s = sinf(phase) + 0.3f * sinf(3.0f * phase);
        buf[i] = (int16_t)(s * env * 7000.0f);
    }
    muse_audio_write(buf, N);
}
