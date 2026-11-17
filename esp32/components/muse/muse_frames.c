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

#include "muse_frames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_boot_still.h"
#include "muse_meeting.h"
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"

/*
 * The action library (project P8) lives on the meeting storage
 * partition, mounted at /meet by muse_meeting: frames/<state>/fNNN.jpg
 * sequences and poses/<state>.jpg stills, all 466x466. The four
 * part-composited faces (listening/thinking/sleepy/dizzy) were kits
 * once — a blank plate plus PNG parts blended at runtime, ~120-250 ms
 * per entry — and are baked offline into single poses stills since
 * the smooth round (the parts and the compositor stay in the dev
 * tree; the baked pixels are the device's own composite, verified
 * byte-for-byte against a runtime build before encoding). This
 * player is the only reader besides the meeting console's dump.
 *
 * RAM discipline (the 1.75C's internal RAM is the scarce resource, see
 * muse_meeting.c's prewarm comment): both frame buffers, the file
 * scratch and this task's stack are in PSRAM, and storage is only
 * mounted seconds after boot — after the voice session's first VM
 * lookup has had its pick of internal RAM. The mount then stays, like
 * it does after any meeting use.
 *
 * The fallback face, for the seconds before the mount and for any
 * failure after it, is the embedded boot still (muse_boot_still.h)
 * when this build links one: muse_frames_attach() decodes it before
 * the player task starts and it stays resident in PSRAM all boot.
 * Only a build with no still falls back to the pixel face — the image
 * widget then simply never unhides.
 */

static const char *TAG = "muse_frames";

#define FR_W 466
#define FR_H 466
/* The minimal face rests lower on the screen (Hugh's hands-on
 * feedback, 2026-11-16): the whole image — sequences, kit composites
 * and stills all show through the one widget — shifts down by this
 * much. Matches FACE_DY in muse_ui.c, which shifts the pixel face. */
#define FR_FACE_DY 20
#define FR_BYTES (FR_W * FR_H * 2)   /* RGB565 */
#define FR_PERIOD_US 125000          /* 8 fps */
#define FR_FILE_MAX (96 * 1024)      /* largest asset is a ~35 KB frame */
#define FR_BOOT_DELAY_US (6 * 1000000LL)   /* mount only once boot settled */
#define FR_BOOT_ANYWAY_US (30 * 1000000LL) /* ... but not never */
#define FR_SLEEPY_IDLE_S 180         /* idle this long: the sleepy face */
#define FR_MAKING_AFTER_S 20.0f      /* thinking this long: a long task */

/* The waking sequence's framing does not match idle's (measured
 * on the shipped frames): both are bottom-anchored figures cropped
 * at the bottom edge, but waking's artwork sits higher — its
 * handoff frame's top edge is at 24 against idle's 38, and its
 * brightness centroid ~16 px higher — so at the shared placement
 * the waking->idle handoff jumps up. An earlier fix zoomed waking
 * 1.18x about a pivot to land its centroid exactly on idle's; the
 * centroid matched, but the figure grew ~22% and its head left the
 * top of the screen (Hugh, hands-on: still too high), so the zoom
 * is gone. Waking now shows 1:1 like every other visual, shifted
 * down by FR_WAKING_DY instead: simulated on the handoff frame,
 * +15 leaves the top edge and both centroids within ~1.3 px of
 * idle's. Only waking moves; idle itself, the poses stills and the
 * kits keep their existing placement, and waking's own
 * mid-animation motion remains, playing around idle's anchor. The
 * embedded boot still shares the shift: it is generated from
 * waking's first frame (tools/gen_boot_still.py), so the boot
 * sequence still -> waking -> idle reads as one continuous picture
 * with no step at the first swap. */
#define FR_WAKING_DY 15

/* Waking -> idle seam experiment (2026-11-17): instead of dipping
 * through black, the last FR_CONV_FRAMES frames of waking zoom the
 * face — through the image widget's own scale, the B-round-proven
 * path — until its geometry matches idle's first frame, then hard-
 * cut. Measured on the shipped frames with the dark-feature method
 * (threshold <80, calibrated against idle f000): idle f000 eye span
 * 146, eye line y 183.1, face top 38; waking f055 (same pose) eye
 * span 112, eye line y 129.3, top 24, eye midpoint (235, 129).
 * Scaling waking by 146/112 = 1.304 (LVGL 334) about that midpoint
 * lands its eye line on its own y=129 in image space; the widget
 * then needs dy 203 - 129 = 74 on screen to sit on idle's eye line
 * (183.1 + FR_FACE_DY 20), i.e. FR_CONV_DY_END 39 on top of the
 * usual 20 + FR_WAKING_DY 15. Cross-checks under this transform:
 * the mouth lands within 0.6 px of idle's, the eyes within a pixel
 * of idle's size; the head top lands ~8 px low, accepted. Set
 * FR_WAKING_CONVERGE to 0 and the Hugh-approved dip path runs
 * again, untouched — Hugh's eyes decide which one ships. */
#define FR_WAKING_CONVERGE 1
#define FR_CONV_FRAMES 4          /* last 4 frames at 8 fps = 0.5 s */
#define FR_CONV_SCALE_END 334     /* 146/112 in LVGL scale (256=1x) */
#define FR_CONV_PIVOT_X 235       /* waking f055 eye midpoint */
#define FR_CONV_PIVOT_Y 129
#define FR_CONV_DY_END 39         /* extra dy on top of 20 + 15 */

/* Core the player task is pinned to. Decoding a 466x466 frame takes
 * ~100 ms of wall time (P8 acceptance log), and the measured time is
 * wall time, so whatever shares the core inflates it directly: on
 * the UI core the player sat under the LVGL task (priority 5) plus
 * the voice and audio work, and every frame flush preempted the
 * decode mid-frame. Core 0 is the network core — its tasks are
 * mostly blocked while a sequence plays, and the ones that do wake
 * (Wi-Fi, lwIP) run above the player's priority 3, so they preempt
 * the decode instead of being delayed by it. This mirrors the video
 * pipeline, whose decoder also runs on core 0 for the same reason
 * (muse_video.c's DECODE_CORE). On a unicore build core 0 is the
 * only core, so nothing changes there. The player's stack stays in
 * PSRAM (MUSE_BIG_CAPS): internal RAM is the scarce resource the
 * VM lookup already starved on once, and a permanent internal stack
 * would shrink its largest block for the whole boot — the video
 * decoder can afford an internal stack only because it is transient.
 * Whether the move actually pays is a device question: the decode
 * stats line (dec_stats_note) is the A/B instrument, and the player
 * logs its core at start so a capture shows which side of the A/B
 * it came from. */
#define FR_PLAYER_CORE 0

/* Interactive state switches (idle <-> listening/thinking, a turn's
 * edges, pet reactions) hard-cut on purpose: the old frame stays at
 * full brightness for the ~0.2 s the new one takes to build, then a
 * single opa-255 direct present replaces it whole. A shallow dip
 * (old frame sunk to opa 160 during the build, swap at the bottom,
 * fade back up) was tried and removed the same day the direct
 * present landed: on device it read as "darkens, then comes back" —
 * 160/255 is ~63% of code value but only ~36% of perceived
 * brightness, and the undimmed border made the centre look darker
 * still (Codex consultation, dev/咨询记录-切换呈现-codex.md).
 * A true crossfade would need a third 434 KB frame buffer, which
 * the PSRAM budget does not have. The waking handoff keeps its own
 * full dip-to-black (Hugh-approved, dip_fade_out() below); nothing
 * here applies to it. */

/* The idle rotation's variant-to-variant change was the one
 * exception for a day (Hugh, 2026-11-17): a true crossfade mixed
 * per pixel in the present path's fill callback, no third buffer
 * needed. On device it read as flicker — six steps over ~370 ms is
 * only ~16 updates a second, each step is itself a ~48 ms top-down
 * progressive write, and the code-value blend dips brightness
 * mid-fade on bright-to-dark pairs (research memo,
 * dev/调研-切换闪烁.md; Codex consulted and agreed). Hugh called
 * it the same evening: rotation hard-cuts like everything else.
 * The crossfade machinery is gone; a rotation enter is an ordinary
 * swap_buffers() through show_front(). The waking dip below is
 * not part of this and stays exactly as approved. */

/* The squash's low point (image scale, 256 = 1.0x). */
#define FR_SQUASH_MIN 246        /* ~96% */

/* First-frame preload: only the three big first frames behind the
 * hottest switches — the listening and thinking stills (baked kit
 * composites since the smooth round) and the level-up celebration's
 * first frame — read into PSRAM once the player is idling, so
 * entering those states skips the slowest FAT reads. ~86 KB in
 * total at the shipped sizes; the margin lesson stands from the
 * first device run of the preload round, which warmed fifteen
 * assets (~148 KB) and left free PSRAM thin enough to hurt
 * neighbours. Every entry is optional and a failed allocation just
 * leaves that asset on the file path. Listening first (every voice
 * turn starts there), then level-up, then thinking. */
static const char *const FR_PRELOAD_PATHS[] = {
    "/meet/poses/listening.jpg",
    "/meet/frames/levelup/f000.jpg",
    "/meet/poses/thinking.jpg",
};
#define FR_PRELOAD_COUNT (sizeof(FR_PRELOAD_PATHS) / sizeof(FR_PRELOAD_PATHS[0]))

typedef struct {
    const uint8_t *data;   /* PSRAM, owned by the table */
    size_t len;
} preload_ent_t;

/* PSRAM, like every other big static in this project: the table is
 * metadata for PSRAM payloads and internal RAM is the scarce one. */
EXT_RAM_BSS_ATTR static preload_ent_t s_pre[FR_PRELOAD_COUNT];
EXT_RAM_BSS_ATTR static int s_pre_next;   /* next FR_PRELOAD_PATHS index */

/* Visuals the resolver can ask for. */
typedef enum {
    V_NONE = 0,   /* pixel face (widget hidden) */
    V_PAUSED,     /* keep whatever is up, decode nothing */
    V_WAKING,
    V_IDLE,
    V_IDLE4,     /* idle rotation pool (see ROT_POOL) */
    V_IDLE5,
    V_DANCE1,
    V_DANCE2,
    V_DANCE3,
    V_DANCE4,
    V_DANCE5,
    V_WORKING,
    V_MAKING,
    V_LEVELUP,
    V_SPEAKING,
    V_LISTENING,
    V_THINKING,
    V_SLEEPY,
    V_DIZZY,
    V_REST,
    V_HAPPY,
    V_MUSIC,
    V_STILL,     /* the embedded boot still is up (fallback only) */
    V_COUNT
} visual_t;

typedef struct {
    const char *dir;   /* under /meet/frames */
    int frames;        /* stored frames (pingpong: one direction) */
    bool oneshot;
    bool pingpong;     /* play forward then back: cycle is 2n-2 */
} seq_def_t;

static const seq_def_t SEQS[V_COUNT] = {
    [V_WAKING] = { "waking", 56, true },
    [V_IDLE] = { "idle", 40, false },
    [V_IDLE4] = { "idle4", 42, false },
    [V_IDLE5] = { "idle5", 83, false },
    [V_DANCE1] = { "dance1", 35, false },
    [V_DANCE2] = { "dance2", 29, false },
    [V_DANCE3] = { "dance3", 39, false },
    [V_DANCE4] = { "dance4", 34, false },
    [V_DANCE5] = { "dance5", 36, false },
    [V_WORKING] = { "working", 40, false },
    [V_MAKING] = { "making", 40, false },
    [V_LEVELUP] = { "levelup", 40, true },
};

static const char *const VIS_NAMES[V_COUNT] = {
    [V_NONE] = "none",       [V_PAUSED] = "paused",   [V_WAKING] = "waking",
    [V_IDLE] = "idle",       [V_WORKING] = "working", [V_MAKING] = "making",
    [V_LEVELUP] = "levelup", [V_SPEAKING] = "speaking",
    [V_LISTENING] = "listening", [V_THINKING] = "thinking",
    [V_SLEEPY] = "sleepy",   [V_DIZZY] = "dizzy",     [V_REST] = "rest",
    [V_HAPPY] = "happy",     [V_MUSIC] = "music",     [V_STILL] = "still",
    [V_IDLE4] = "idle4",     [V_IDLE5] = "idle5",
    [V_DANCE1] = "dance1",   [V_DANCE2] = "dance2",   [V_DANCE3] = "dance3",
    [V_DANCE4] = "dance4",   [V_DANCE5] = "dance5",
};

/* Sequence positions are cycle positions: for a ping-pong sequence
 * the cycle runs 0..n-1..1 (2n-2 steps) over the n stored frames. */
static int seq_cycle(visual_t v)
{
    return SEQS[v].pingpong ? 2 * SEQS[v].frames - 2 : SEQS[v].frames;
}

static int seq_file_idx(visual_t v, int pos)
{
    int n = SEQS[v].frames;
    return (SEQS[v].pingpong && pos >= n) ? 2 * n - 2 - pos : pos;
}

/* Still states are a single poses/<name>.jpg (the four ex-kit faces
 * included, baked offline). Speaking is three frames picked by level. */
static bool vis_is_still(visual_t v)
{
    return v == V_REST || v == V_HAPPY || v == V_MUSIC;
}

static lv_obj_t *s_img;
static lv_image_dsc_t s_dsc;
static uint16_t *s_still;          /* PSRAM: the decoded boot still */
static lv_image_dsc_t s_still_dsc;
static bool s_still_up;            /* still decoded: it is the fallback */
static uint16_t *s_buf[2];   /* PSRAM frame buffers; s_front is on screen */
static int s_front;
static uint8_t *s_file;      /* PSRAM scratch: one asset file, whole */
static jpeg_dec_handle_t s_jdec;
static bool s_broken[V_COUNT];   /* asset failed once: pixel face for it */

/* Speculative thinking pre-build (smooth round). While the listening
 * face sits static, the player decodes the thinking still — the
 * common next visual in a voice turn — into the back buffer, so the
 * switch costs a swap instead of a decode. Ownership is trivially
 * safe: the player is the only task that ever decodes, and it never
 * holds the display lock while doing so. s_epoch invalidates the
 * pre-build: it bumps on every visual enter and every broken-target
 * diversion, and the enter path only consumes a pre-build whose
 * recorded epoch is still current (any other enter logs it wasted).
 * A demand build always preempts: the worst case is waiting out the
 * one speculative decode already in flight (~a still's decode time),
 * the same order as the statics poll cadence itself. */
static bool s_spec_ready;        /* back buffer holds the thinking still */
static uint32_t s_spec_epoch;    /* s_epoch when the pre-build finished */
static uint32_t s_epoch;         /* bumped on every enter / broken diversion */

/* muse_frames_hold(): the requester sets s_hold_req; the player
 * acknowledges in s_hold_ack once it has closed the decoder and
 * stopped decoding. s_player_alive/done let a hold on a build whose
 * player never started (or gave up on the library) succeed at once —
 * nothing is decoding then, which is all the requester needs. */
static volatile bool s_hold_req;
static volatile bool s_hold_ack;
static volatile bool s_player_alive;
static volatile bool s_player_done;

/* Decode timing, for the acceptance log line. */
static uint32_t s_dec_n, s_dec_total_ms, s_dec_max_ms, s_dec_skipped;
static const char *s_dec_what = "-";

static void dec_stats_note(uint32_t ms)
{
    s_dec_n++;
    s_dec_total_ms += ms;
    if (ms > s_dec_max_ms) {
        s_dec_max_ms = ms;
    }
    if (s_dec_n >= 80) {
        ESP_LOGI(TAG, "decode %s: avg %u ms, max %u ms (n=%u, skipped %u)",
                 s_dec_what, (unsigned)(s_dec_total_ms / s_dec_n),
                 (unsigned)s_dec_max_ms, (unsigned)s_dec_n, (unsigned)s_dec_skipped);
        s_dec_n = 0;
        s_dec_total_ms = 0;
        s_dec_max_ms = 0;
        s_dec_skipped = 0;
    }
}

/* ---- file + JPEG plumbing (player task only) ---- */

static bool read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(s_file, 1, FR_FILE_MAX, f);
    bool ok = n > 0 && !ferror(f) && feof(f);
    fclose(f);
    if (!ok) {
        return false;
    }
    *len_out = n;
    return true;
}

/* The preloaded bytes for path, or NULL when that asset is not (yet)
 * resident — callers then take the file path they always took. */
static const uint8_t *preload_get(const char *path, size_t *len_out)
{
    for (size_t i = 0; i < FR_PRELOAD_COUNT; i++) {
        if (s_pre[i].data && !strcmp(FR_PRELOAD_PATHS[i], path)) {
            *len_out = s_pre[i].len;
            return s_pre[i].data;
        }
    }
    return NULL;
}

/* Load one more preload entry from storage into PSRAM. Called from
 * the player's idle passes, one file at a time, so the reads spread
 * over frames that already have their next frame decoded. A failure
 * is permanent for that entry this boot (the index still advances)
 * and only costs the speedup, never the state. */
static void preload_step(void)
{
    if (s_pre_next >= (int)FR_PRELOAD_COUNT) {
        return;
    }
    const char *path = FR_PRELOAD_PATHS[s_pre_next++];
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    if (size <= 0 || size > FR_FILE_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    uint8_t *buf = heap_caps_malloc((size_t)size, MUSE_BIG_CAPS);
    if (!buf) {
        fclose(f);
        ESP_LOGW(TAG, "preload %s: no PSRAM, staying on the file path", path);
        return;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        heap_caps_free(buf);
        return;
    }
    s_pre[s_pre_next - 1].data = buf;
    s_pre[s_pre_next - 1].len = (size_t)size;
    ESP_LOGI(TAG, "preload %s: %u bytes (free psram %u)", path, (unsigned)size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* Decode one 466x466 JPEG with an already-open handle into out
 * (FR_BYTES of RGB565). No stats, no handle management: the caller
 * owns the handle. */
static bool jpeg_decode(jpeg_dec_handle_t dec, const uint8_t *jpg, size_t len, uint16_t *out)
{
    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)jpg,
        .inbuf_len = (int)len,
        .outbuf = (uint8_t *)out,
        .out_size = FR_BYTES,
    };
    jpeg_dec_header_info_t info;
    bool ok = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK
              && info.width == FR_W && info.height == FR_H;
    int need = 0;
    if (ok) {
        ok = jpeg_dec_get_outbuf_len(dec, &need) == JPEG_ERR_OK && need == FR_BYTES;
    }
    if (ok) {
        ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
    }
    return ok;
}

/* Decode the JPEG in s_file[0..len) into out (FR_BYTES of RGB565).
 * False on any failure; a decoder failure also drops the handle so the
 * next call reopens it. */
static bool decode_jpeg_mem(const uint8_t *jpg, size_t len, uint16_t *out)
{
    if (!s_jdec) {
        jpeg_dec_config_t cfg = {
            .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
            .rotate = JPEG_ROTATE_0D,
        };
        if (jpeg_dec_open(&cfg, &s_jdec) != JPEG_ERR_OK || !s_jdec) {
            s_jdec = NULL;
            return false;
        }
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = jpeg_decode(s_jdec, jpg, len, out);
    if (!ok) {
        jpeg_dec_close(s_jdec);
        s_jdec = NULL;
        return false;
    }
    dec_stats_note((uint32_t)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static bool decode_jpeg_file(const char *path, uint16_t *out)
{
    size_t len;
    if (!read_file(path, &len)) {
        return false;
    }
    return decode_jpeg_mem(s_file, len, out);
}

/* ---- stills ---- */

/* Decode visual v's still (poses/<name>.jpg) into out, through the
 * preload when the asset is resident. Every still state goes through
 * here: the plain poses and, since the smooth round, the four faces
 * that used to be composited from sprite kits at runtime. */
static bool decode_still(visual_t v, uint16_t *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/meet/poses/%s.jpg", VIS_NAMES[v]);
    s_dec_what = VIS_NAMES[v];
    size_t clen;
    const uint8_t *cached = preload_get(path, &clen);
    return cached ? decode_jpeg_mem(cached, clen, out)
                  : decode_jpeg_file(path, out);
}

/* ---- display (always under the board's display lock) ---- */

/* The visual the next swap_buffers() puts up; waking — and the boot
 * still, which is waking's first frame — are shown shifted down
 * (see FR_WAKING_DY). Set by every path that swaps. */
static visual_t s_swap_vis = V_NONE;

/* The next swap_buffers() is a visual-change enter, not a same-visual
 * frame advance: show_front() may present it straight to the panel
 * (present_now). Set in the player's enter branch; consumed — read
 * and cleared — by show_front(), and cleared again if the enter
 * fails before any swap. */
static bool s_enter_swap;

/* Mid-dip: waking has faded to black and the next visual swapped in
 * must start at opa 0 so the player can fade it in (dip_fade_in()).
 * Set by dip_fade_out(), consumed by the player's enter path. */
static bool s_dip;

/* Display lock held by the caller (the show_* paths take it). */
/* The live squash factor (256 = 1.0x), written by the squash
 * animation in LVGL context and read here so a frame swap mid-squash
 * re-applies the in-flight factor instead of resetting it. */
static volatile int32_t s_squash_factor = 256;

/* Waking convergence state (FR_WAKING_CONVERGE): while active, the
 * waking widget zooms about the measured pivot instead of resting
 * at the squash factor. Written by the player (conv_update) ahead
 * of the swap that shows each converged frame, read here under the
 * display lock like s_squash_factor. Reset by any non-waking enter. */
static bool s_conv_active;
static int32_t s_conv_scale = 256;
static int32_t s_conv_dy;

static void apply_framing(visual_t v)
{
    lv_image_set_scale(s_img, s_squash_factor);
    if (v == V_WAKING && s_conv_active) {
        lv_image_set_pivot(s_img, FR_CONV_PIVOT_X, FR_CONV_PIVOT_Y);
        lv_image_set_scale(s_img, s_conv_scale);
    }
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0,
                 (muse_board->minimal_face ? FR_FACE_DY : 0) +
                 ((v == V_WAKING || v == V_STILL) ? FR_WAKING_DY : 0) +
                 ((v == V_WAKING && s_conv_active) ? s_conv_dy : 0));
}

/* Convergence for the waking frame the next swap puts up: the last
 * FR_CONV_FRAMES frames ramp scale 256 -> FR_CONV_SCALE_END and the
 * extra dy 0 -> FR_CONV_DY_END in equal steps. */
static void conv_update(int pos)
{
    if (!FR_WAKING_CONVERGE) {
        return;
    }
    int start = SEQS[V_WAKING].frames - FR_CONV_FRAMES;
    if (pos < start) {
        return;
    }
    float p = (float)(pos - start + 1) / (float)FR_CONV_FRAMES;
    s_conv_scale = 256 + (int32_t)((FR_CONV_SCALE_END - 256) * p);
    s_conv_dy = (int32_t)(FR_CONV_DY_END * p);
    if (!s_conv_active || pos >= SEQS[V_WAKING].frames - 1) {
        ESP_LOGI(TAG, "waking converge: frame %d scale %d dy extra %d",
                 pos, (int)s_conv_scale, (int)s_conv_dy);
    }
    s_conv_active = true;
}

static void squash_exec(void *obj, int32_t v);   /* defined with the squash */
static void breath_stop_locked(void);            /* defined with the breath */

/* ---- direct present: a whole face in one panel write ----
 *
 * An LVGL refresh paints a full-screen face in four 118-row bands,
 * drawn and sent one after another: the sweep takes ~65-75 ms, so
 * for several panel refreshes the top of the screen shows the new
 * face while the bottom still shows the old one — the horizontal
 * split a state switch used to show. The decoded frame is already
 * whole in PSRAM, so on boards whose driver can write a rectangle
 * straight to the panel (draw_frame, timed against the panel's scan
 * like the video path), an enter presents the face's centre in one
 * such write: the scan runs faster than the writer, so started just
 * after a scan it reads the old picture everywhere, and the next
 * scan reads the new one everywhere. No mixed refresh.
 *
 * Only the centre goes direct. The fixed chrome — progress ring,
 * battery arc, status row — and the dialog box are LVGL objects over
 * the face; the rows and columns they live in stay LVGL's, repaired
 * by the invalidations in present_border() right after the write
 * (thin strips, one or two bands each, mostly edge and background
 * pixels). The widget's source and opa are still updated under the
 * display lock first, so any later LVGL redraw composites the same
 * picture. Falls back to the plain LVGL swap whenever anything is
 * off: no draw_frame, not the minimal face, the face tile not front,
 * the waking black dip, or the boot/waking stills. A squash still
 * running is cancelled, not waited out (present_squash_cancel). */
#define FR_PRES_TOP 52            /* first direct row: below the status row */
#define FR_PRES_X0 16             /* side columns stay LVGL's: the ring's */
#define FR_PRES_X1 (FR_W - 16)    /* edge pixels live in them */
#define FR_PRES_DIALOG_TOP 342    /* dialog up: rows from here stay LVGL's */

typedef struct {
    const uint16_t *base;   /* the rect's first source pixel */
    int opa;                /* 255: plain copy; below: scale channels */
} pres_arg_t;

/* fill() for draw_frame: the rect's rows from the front buffer, in
 * the panel's byte order (the decoded frames are plain RGB565, the
 * panel wants them swapped), at the given opa. */
static void pres_fill(uint8_t *dst, int stride, int row, int rows, void *arg)
{
    const pres_arg_t *p = arg;
    const int w = FR_PRES_X1 - FR_PRES_X0;
    const uint16_t *src = p->base + (size_t)row * FR_W;
    for (int r = 0; r < rows; r++) {
        uint16_t *d = (uint16_t *)dst;
        const uint16_t *s = src + (size_t)r * FR_W;
        if (p->opa >= LV_OPA_COVER) {
            for (int x = 0; x < w; x++) {
                uint16_t v = s[x];
                d[x] = (uint16_t)((v << 8) | (v >> 8));
            }
        } else {
            const int o = p->opa;
            for (int x = 0; x < w; x++) {
                uint16_t v = s[x];
                uint16_t nv = (uint16_t)((((v >> 11) & 31) * o / 255 << 11) |
                                         (((v >> 5) & 63) * o / 255 << 5) |
                                         ((v & 31) * o / 255));
                d[x] = (uint16_t)((nv << 8) | (nv >> 8));
            }
        }
        dst += stride;
    }
}

static bool present_ok(void)
{
    return muse_board->draw_frame && muse_board->minimal_face &&
           muse_ui_face_active();
}

/* End a squash that is still running, snapping the widget back to
 * full scale. The caller holds the display lock. A direct present
 * writes the face unscaled, so a squash left running would fight it
 * — and gating the present on the squash instead would defeat the
 * whole point: the presses that start a squash (a face tap, the talk
 * key's edge into LISTENING) are the very presses that switch the
 * face, and their builds routinely outlast the animation's first
 * steps, so the swap would land mid-squash and fall back to the
 * banded sweep. The press already had its feedback while the new
 * face built; the switch itself is the rest of the answer. */
static void present_squash_cancel(void)
{
    lv_anim_delete(s_img, squash_exec);
    if (s_squash_factor != 256) {
        s_squash_factor = 256;
        lv_image_set_scale(s_img, 256);
    }
}

/* The last direct row's other side: with the dialog up, its rows
 * (and the chin under it) stay LVGL's. Read under the display lock —
 * the dialog's visibility only changes in LVGL context — and the
 * snapshot is passed down, so the write and the border repair agree
 * on where the face's territory ends. */
static int present_bottom(void)
{
    return muse_ui_dialog_up() ? FR_PRES_DIALOG_TOP : FR_H;
}

/* Write the front buffer's centre straight to the panel at opa,
 * rows [FR_PRES_TOP, y1). Blocks for the write (~26 ms of wire
 * time). The caller holds the display lock across the write: an
 * LVGL refresh pass runs under the same lock, so no band of a pass
 * can interleave with the frame on the panel bus. */
static void present_now(int opa, int y1)
{
    int dy = (muse_board->minimal_face ? FR_FACE_DY : 0) +
             ((s_swap_vis == V_WAKING || s_swap_vis == V_STILL) ? FR_WAKING_DY : 0);
    int y0 = FR_PRES_TOP > dy ? FR_PRES_TOP : dy;
    pres_arg_t a = { s_buf[s_front] + (size_t)(y0 - dy) * FR_W + FR_PRES_X0, opa };
    int64_t t0 = esp_timer_get_time();
    muse_board->draw_frame(FR_PRES_X0, y0, FR_PRES_X1, y1, 8, pres_fill, &a);
    ESP_LOGI(TAG, "present: %lld ms (opa %d)", (long long)(esp_timer_get_time() - t0) / 1000, opa);
}

/* After a direct write, hand the border back to LVGL: the strips and
 * columns the write left out (or painted over), redrawn from the
 * widget's new source at its new opa, chrome included. y1 is the
 * same snapshot present_now() wrote to. */
static void present_border(int y1)
{
    const lv_area_t areas[] = {
        { 0, 0, FR_W - 1, FR_PRES_TOP - 1 },              /* status row, arc top */
        { 64, 40, 151, 99 },                              /* battery arc, left end */
        { 314, 40, 401, 99 },                             /* battery arc, right end */
        { 0, FR_PRES_TOP, FR_PRES_X0 - 1, y1 - 1 },       /* ring's left edge */
        { FR_PRES_X1, FR_PRES_TOP, FR_W - 1, y1 - 1 },    /* ring's right edge */
        { 216, 448, 249, FR_H - 1 },                      /* ring's bottom point */
        { 0, FR_PRES_DIALOG_TOP, FR_W - 1, FR_H - 1 },    /* dialog rows, if any */
    };
    int n = (int)(sizeof(areas) / sizeof(areas[0])) - (y1 < FR_H ? 0 : 1);
    muse_board->display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate_area(scr, &areas[i]);
    }
    muse_board->display_unlock();
}

static void show_front(void)
{
    int opa = s_dip ? 0 : LV_OPA_COVER;
    muse_board->display_lock(-1);
    /* Every swap ends the listening breath (a no-op unless the
     * breath is running): the face this swap puts up is not the
     * listening still, and apply_framing below re-applies the
     * resting scale. */
    breath_stop_locked();
    /* Eligibility is decided under the lock: the face tile can be
     * swiped away while this swap was building, and a direct write
     * decided on the stale answer would paint the face over the
     * tile that just came forward. */
    bool direct = s_enter_swap && !s_dip && present_ok() &&
                  s_swap_vis != V_WAKING && s_swap_vis != V_STILL;
    s_enter_swap = false;
    if (direct) {
        present_squash_cancel();   /* before apply_framing re-applies it */
    }
    s_dsc.data = (const uint8_t *)s_buf[s_front];
    lv_image_set_src(s_img, &s_dsc);
    apply_framing(s_swap_vis);
    /* Mid-waking-dip the incoming visual starts black; the player
     * fades it in right after the swap (see dip_fade_out()). Every
     * other swap starts at full opa. */
    lv_obj_set_style_opa(s_img, (lv_opa_t)opa, 0);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    if (!direct) {
        lv_obj_invalidate(s_img);
    }
    int y1 = direct ? present_bottom() : 0;
    if (direct) {
        present_now(opa, y1);   /* under the lock: serialized with LVGL */
    }
    muse_board->display_unlock();
    if (direct) {
        present_border(y1);
    }
}

static void hide_face(void)
{
    muse_board->display_lock(-1);
    breath_stop_locked();
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    muse_board->display_unlock();
}

static void show_still(void)
{
    muse_board->display_lock(-1);
    breath_stop_locked();
    lv_image_set_src(s_img, &s_still_dsc);
    apply_framing(V_STILL);
    lv_obj_set_style_opa(s_img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_img);
    muse_board->display_unlock();
}

/* The face to fall back on when a visual cannot be shown: the
 * embedded boot still when this build has one (it is already decoded
 * and resident), the pixel face — widget hidden — otherwise. Returns
 * the visual to record as current. */
static visual_t fallback_face(visual_t cur)
{
    if (s_still_up) {
        if (cur != V_STILL) {
            show_still();
        }
        return V_STILL;
    }
    if (cur != V_NONE) {
        hide_face();
    }
    return V_NONE;
}

static void swap_buffers(void)
{
    s_front ^= 1;
    show_front();
}

static uint16_t *back_buffer(void)
{
    return s_buf[s_front ^ 1];
}

/* levelup's exit used to fade the last frame out here (LVGL opa
 * steps 200 -> 0, ~45 ms apart) and hide the widget, leaving a
 * dark gap while idle built. It was the same brightness excursion
 * as the interactive soft dip and went with it: the caller now
 * leaves the last celebration frame up, and the next visual's own
 * swap replaces it whole at full opa — a hard cut, no dark phase. */

/* Waking's exit dips through black in fine steps (~240 ms each
 * way), leaving the widget up at opa 0: the waking and idle
 * artwork was shot at different distances — the face grows ~19%
 * at the cut — and the dip turns that size step into a blink. The
 * ramp runs the full range from LV_OPA_COVER in 32-opa steps:
 * starting mid-way (or stepping coarsely) is itself a visible
 * jump at the cut. Only the waking handoff uses this (Hugh
 * approved it as-is; the interactive soft dip that used to sit
 * beside it is gone, this one stays). The fade-out runs in the
 * player task when the one-shot plays out; s_dip then makes the
 * next swap start black (show_front), and the player fades the
 * new visual in right after entering it. The display lock is
 * taken per step, so the UI task is never blocked by the delays. */
static void dip_fade_out(void)
{
    s_dip = true;
    for (int opa = LV_OPA_COVER; opa > 0; opa -= 32) {
        muse_board->display_lock(-1);
        lv_obj_set_style_opa(s_img, (lv_opa_t)opa, 0);
        lv_obj_invalidate(s_img);
        muse_board->display_unlock();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    muse_board->display_lock(-1);
    lv_obj_set_style_opa(s_img, LV_OPA_TRANSP, 0);
    lv_obj_invalidate(s_img);
    muse_board->display_unlock();
}

static void dip_fade_in(void)
{
    for (int opa = 0; opa < LV_OPA_COVER; opa += 32) {
        muse_board->display_lock(-1);
        lv_obj_set_style_opa(s_img, (lv_opa_t)opa, 0);
        lv_obj_invalidate(s_img);
        muse_board->display_unlock();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    muse_board->display_lock(-1);
    lv_obj_set_style_opa(s_img, LV_OPA_COVER, 0);
    lv_obj_invalidate(s_img);
    muse_board->display_unlock();
}

/* ---- idle rotation ----
 *
 * The resolver's plain "idle" is a pool, not a single sequence
 * (Hugh, 2026-11-17: every animation that can pass as idle joins).
 * A shuffle bag deals the visits:
 * normal variants hold three slots each, low-weight ones (the five
 * official dances) one — so a low variant surfaces occasionally,
 * never twice in a row, and no variant repeats back-to-back. Each
 * visit plays its loops, then the player rotates: the next
 * variant is picked here, and its enter hard-cuts through the
 * ordinary swap, framing step and all (the crossfade that was
 * meant to absorb the framing differences read as flicker and
 * was removed the same evening — see the note at the top of the
 * file). State faces
 * (working/making/taking-notes semantics, the headphones rule) are
 * NOT pool members: idle showing them would fake the status the
 * face exists to report.
 *
 * Slimmed the same evening on Hugh's call after he reviewed every
 * segment: the two ping-pong close-ups (idle2/idle3), the flip gag,
 * the Halloween dance and the waking-as-idle stretch are out — the
 * stretch duplicated the boot animation, which idle must not
 * replay. The waking artwork itself is untouched (it is the boot
 * sequence); it simply no longer joins the rotation. */
typedef struct {
    visual_t v;
    int loops_min, loops_max;
    bool low;   /* low weight: one bag slot, never after another low */
} rot_ent_t;

static const rot_ent_t ROT_POOL[] = {
    { V_IDLE,    2, 3, false },
    { V_IDLE4,   2, 3, false },
    { V_IDLE5,   2, 3, false },
    { V_DANCE1,  2, 2, true },
    { V_DANCE2,  2, 2, true },
    { V_DANCE3,  2, 2, true },
    { V_DANCE4,  2, 2, true },
    { V_DANCE5,  2, 2, true },
};
#define ROT_N (sizeof(ROT_POOL) / sizeof(ROT_POOL[0]))
#define ROT_BAG_MAX 32

static visual_t s_rot_cur = V_IDLE;   /* variant the pool is visiting */
static int s_rot_loops;               /* loops left in this visit */
static bool s_rot_pending;            /* a rotation enter is pending */
static int s_bag[ROT_BAG_MAX];
static int s_bag_n;
static int s_bag_pos;                 /* == s_bag_n: bag empty */

static const rot_ent_t *rot_find(visual_t v)
{
    for (size_t i = 0; i < ROT_N; i++) {
        if (ROT_POOL[i].v == v) {
            return &ROT_POOL[i];
        }
    }
    return NULL;
}

static bool is_idle_variant(visual_t v)
{
    return rot_find(v) != NULL;
}

static int visit_loops(visual_t v)
{
    const rot_ent_t *e = rot_find(v);
    if (!e) {
        return 2;
    }
    if (e->loops_max <= e->loops_min) {
        return e->loops_min;
    }
    return e->loops_min
           + (int)(esp_random() % (uint32_t)(e->loops_max - e->loops_min + 1));
}

static void rot_refill(void)
{
    int n = 0;
    for (size_t i = 0; i < ROT_N && n < ROT_BAG_MAX; i++) {
        int reps = ROT_POOL[i].low ? 1 : 3;
        for (int r = 0; r < reps && n < ROT_BAG_MAX; r++) {
            s_bag[n++] = (int)i;
        }
    }
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int tmp = s_bag[i];
        s_bag[i] = s_bag[j];
        s_bag[j] = tmp;
    }
    s_bag_n = n;
    s_bag_pos = 0;
}

/* The next variant to visit after cur: never cur itself, never a
 * broken one, never a low-weight one right after a low-weight one.
 * Skipped slots are spent (the multiplicities make that cheap);
 * an exhausted bag is refilled once before giving up to V_IDLE. */
static visual_t rot_pick_next(visual_t cur)
{
    const rot_ent_t *ce = rot_find(cur);
    bool cur_low = ce && ce->low;
    for (int attempt = 0; attempt < 2; attempt++) {
        while (s_bag_pos < s_bag_n) {
            const rot_ent_t *e = &ROT_POOL[s_bag[s_bag_pos++]];
            if (e->v == cur || s_broken[e->v] || (cur_low && e->low)) {
                continue;
            }
            return e->v;
        }
        rot_refill();
    }
    return V_IDLE;
}

/* End the current visit: pick the next variant; its enter is an
 * ordinary hard-cut swap. A no-op when a rotation is already
 * pending (a frame skip in the same pass must not spend two
 * picks), and re-arms the visit when the bag hands back the
 * current variant itself. */
static void rot_advance(visual_t cur)
{
    if (s_rot_pending) {
        return;
    }
    visual_t nxt = rot_pick_next(cur);
    if (nxt != cur) {
        ESP_LOGI(TAG, "idle rotate: %s -> %s", VIS_NAMES[cur], VIS_NAMES[nxt]);
        s_rot_cur = nxt;
        s_rot_pending = true;
    } else {
        s_rot_loops = visit_loops(cur);
    }
}

/* ---- which visual should be up ---- */

static visual_t s_last_active = V_NONE;   /* last SPEAKING/THINKING seen */
static TickType_t s_last_active_tick;     /* when it was last seen */
static bool s_levelup_pending;
static float s_prev_happy;
static bool s_waking_done;

/* Pet taps (muse_frames_pet_tap): a pending request plus which of
 * the two reactions is next. Written by the UI task, consumed only
 * by the player's resolver in IDLE, so a tap lands between turns,
 * never in the middle of one. (The headphones overlay that used to
 * answer taps on sprite builds was removed whole on Hugh's call,
 * 2026-11-17: he saw it on the device and did not like it.) */
static volatile bool s_pet_pending;
static bool s_pet_levelup_turn = true;

void muse_frames_pet_tap(void)
{
    s_pet_pending = true;
}

/* ---- instant touch feedback (LVGL context only) ----
 * The squash rides the image widget's own scale (lv_image_set_scale),
 * NOT the object transform: a style transform makes LVGL render the
 * widget into a layer buffer, and on this board that allocation
 * (106-137 KB) cannot be served once the action library is up — the
 * first device run of this round (2026-11-17) logged dozens of
 * lv_draw_layer_alloc_buf failures around the press and the switch
 * it was meant to decorate took 1.6 s. The image's own scale is
 * applied by the image drawer while it samples the source, with no
 * layer. apply_framing() re-applies s_squash_factor at every swap,
 * so the animation also survives frame swaps instead of being reset
 * by them. The press half is fast (70 ms down to FR_SQUASH_MIN);
 * the recovery overshoots a touch past 1.0x on its way back, which
 * is what makes it read as a spring rather than a zoom. A
 * re-trigger deletes the running half and starts from wherever the
 * scale currently is. */
static void squash_exec(void *obj, int32_t v)
{
    s_squash_factor = v;
    lv_image_set_scale((lv_obj_t *)obj, v);
}

static void squash_recover(lv_anim_t *a)
{
    lv_obj_t *img = (lv_obj_t *)a->var;
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, img);
    lv_anim_set_exec_cb(&b, squash_exec);
    lv_anim_set_values(&b, s_squash_factor, 256);
    lv_anim_set_duration(&b, 190);
    lv_anim_set_path_cb(&b, lv_anim_path_overshoot);
    lv_anim_start(&b);
}

void muse_frames_squash(void)
{
    if (!s_img || lv_obj_has_flag(s_img, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_image_set_pivot(s_img, FR_W / 2, FR_H / 2);
    lv_anim_delete(s_img, squash_exec);
    int32_t cur = s_squash_factor;
    if (cur <= 0 || cur > 512) {   /* the recovery overshoots past 256
                                    * legitimately: only nonsense resets */
        cur = 256;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_img);
    lv_anim_set_exec_cb(&a, squash_exec);
    lv_anim_set_values(&a, cur, FR_SQUASH_MIN);
    lv_anim_set_duration(&a, 70);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, squash_recover);
    lv_anim_start(&a);
}

/* ---- the listening breath ----
 * Holding the talk key leaves the listening face up as a still for
 * the whole take, and a frozen face reads as a hung device (Hugh,
 * 2026-11-17: "it is a wallpaper — can it move?"). The breath is
 * the squash's cheap trick aimed at calm instead of feedback: the
 * image's own scale rides a slow ease-in-out between -3% and +3%
 * (a 2.4 s full cycle), no layer buffer, no new assets. It runs
 * only while the listening still is statically displayed — the
 * player starts it from the statics branch, after the speculative
 * thinking build has had its pass — and every way out of the still
 * stops it: show_front(), hide_face() and show_still() all call
 * breath_stop_locked() under the display lock, and the stop
 * re-applies the resting (squash) factor, so the next face never
 * inherits a breath scale. Its exec callback is its own, so the
 * squash's delete-by-callback (present_squash_cancel) can never
 * kill it, nor it the squash. */
#define FR_BREATH_MIN 248        /* ~-3% */
#define FR_BREATH_MAX 264        /* ~+3% */
#define FR_BREATH_HALF_MS 1200   /* one way; a full breath is 2.4 s */

static bool s_breath_on;

static void breath_exec(void *obj, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)obj, v);
}

/* Caller holds the display lock. */
static void breath_stop_locked(void)
{
    if (!s_breath_on) {
        return;
    }
    lv_anim_delete(s_img, breath_exec);
    s_breath_on = false;
    lv_image_set_scale(s_img, s_squash_factor);
    ESP_LOGI(TAG, "breath: off");
}

/* Called from the player's statics branch while LISTENING is up;
 * idempotent, and the flag is checked before the lock is taken so
 * the 120 ms pass costs nothing once the breath is running. */
static void breath_start(void)
{
    if (s_breath_on) {
        return;
    }
    muse_board->display_lock(-1);
    if (!s_breath_on && s_img && !lv_obj_has_flag(s_img, LV_OBJ_FLAG_HIDDEN)) {
        lv_image_set_pivot(s_img, FR_W / 2, FR_H / 2);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_img);
        lv_anim_set_exec_cb(&a, breath_exec);
        lv_anim_set_values(&a, FR_BREATH_MIN, FR_BREATH_MAX);
        lv_anim_set_duration(&a, FR_BREATH_HALF_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_reverse_duration(&a, FR_BREATH_HALF_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        s_breath_on = true;
        ESP_LOGI(TAG, "breath: on (listening)");
    }
    muse_board->display_unlock();
}

static visual_t resolve_target(void)
{
    if (muse_state_asleep() || !muse_ui_face_active()) {
        return V_PAUSED;
    }
    if (muse_state_recording()) {
        return V_REST;
    }
    float mode_t = 0;
    muse_mode_t mode = muse_state_mode(&mode_t);
    float happy = muse_state_happiness();
    /* A voice turn just delivered: the voice code raises happiness as
     * it goes idle (muse_voice.c). A pet tap raises it too, but with
     * no turn behind it — that one gets the happy still instead. The
     * edge is consumed either way, and only a turn that ended within
     * the happiness window counts: without the freshness check, the
     * first pet tap after any long-ago turn would play the level-up. */
    if (s_prev_happy <= 0.3f && happy > 0.3f) {
        if ((s_last_active == V_SPEAKING || s_last_active == V_THINKING)
            && xTaskGetTickCount() - s_last_active_tick <= pdMS_TO_TICKS(3000)) {
            s_levelup_pending = true;
        }
        s_last_active = V_NONE;
    }
    s_prev_happy = happy;
    switch (mode) {
    case MUSE_MODE_BOOT:
        return s_waking_done ? V_IDLE : V_WAKING;
    case MUSE_MODE_IDLE:
        if (s_pet_pending) {
            /* A pat on the face: alternate the two one-shot reactions
             * the library already has. The celebration half queues the
             * same pending flag the happiness edge uses; the happy half
             * raises happiness through muse_state, exactly like a
             * turn's own pat, and the branch below picks it up on the
             * next pass. */
            s_pet_pending = false;
            if (s_pet_levelup_turn && !s_broken[V_LEVELUP]) {
                s_levelup_pending = true;
            } else {
                muse_state_make_happy();
            }
            s_pet_levelup_turn = !s_pet_levelup_turn;
        }
        if (s_levelup_pending) {
            if (!s_broken[V_LEVELUP]) {
                return V_LEVELUP;
            }
            s_levelup_pending = false;   /* unusable assets must not wedge idle */
        }
        if (happy > 0.05f) {
            return V_HAPPY;
        }
        if (muse_state_idle_secs() >= FR_SLEEPY_IDLE_S) {
            return V_SLEEPY;
        }
        return V_IDLE;
    case MUSE_MODE_LISTENING:
        s_last_active = V_LISTENING;
        s_last_active_tick = xTaskGetTickCount();
        return V_LISTENING;
    case MUSE_MODE_THINKING:
        s_last_active = V_THINKING;
        s_last_active_tick = xTaskGetTickCount();
        if (muse_voice_call_view() == MUSE_VOICE_VIEW_WORKING) {
            return mode_t >= FR_MAKING_AFTER_S ? V_MAKING : V_WORKING;
        }
        return V_THINKING;
    case MUSE_MODE_SPEAKING:
        s_last_active = V_SPEAKING;
        s_last_active_tick = xTaskGetTickCount();
        return V_SPEAKING;
    case MUSE_MODE_ERROR:
        return V_DIZZY;
    case MUSE_MODE_OFF:
    default:
        return V_NONE;
    }
}

/* ---- entering visuals ---- */

/* Decode visual v's still into the back buffer and swap it in.
 * False (and v marked broken) when the asset is unusable; the
 * fallback face stays up then. */
static bool enter_still(visual_t v)
{
    bool ok = decode_still(v, back_buffer());
    if (!ok) {
        s_broken[v] = true;
        ESP_LOGW(TAG, "%s: asset unusable, fallback face for this state", VIS_NAMES[v]);
        return false;
    }
    s_swap_vis = v;
    swap_buffers();
    return true;
}

static bool enter_seq_frame(visual_t v, int pos, uint16_t *into)
{
    int idx = seq_file_idx(v, pos);   /* cycle position -> stored frame */
    char path[96];
    snprintf(path, sizeof(path), "/meet/frames/%s/f%03d.jpg", SEQS[v].dir, idx);
    s_dec_what = VIS_NAMES[v];
    size_t clen;
    const uint8_t *cached = preload_get(path, &clen);
    bool ok = cached ? decode_jpeg_mem(cached, clen, into)
                     : decode_jpeg_file(path, into);
    if (!ok) {
        s_broken[v] = true;
        ESP_LOGW(TAG, "%s: frame %d unusable, fallback face for this state", VIS_NAMES[v], idx);
        return false;
    }
    return true;
}

static const char *const MOUTH_FILES[3] = {
    "/meet/frames/speaking/talk_closed.jpg",
    "/meet/frames/speaking/talk_half.jpg",
    "/meet/frames/speaking/talk_open.jpg",
};

static int mouth_for_level(float level)
{
    if (level >= 0.35f) {
        return 2;
    }
    if (level >= 0.12f) {
        return 1;
    }
    return 0;
}

/* ---- the player task ---- */

static void player_task(void *arg)
{
    (void)arg;
    s_player_alive = true;
    ESP_LOGI(TAG, "player running on core %d", xPortGetCoreID());
    int64_t born = esp_timer_get_time();

    /* Mount once boot has settled (see the file header). A partition
     * that will not mount is retried a few times in case the boot
     * prewarm was still running, then given up on for this boot. */
    bool mounted = false;
    bool hopeless = false;
    int tries = 0;   /* real mount attempts only — the gate waits below
                      * must not eat the retry budget */
    while (!mounted && !hopeless) {
        if (s_hold_req) {
            /* Asked to be quiet before the library is even up: there
             * is nothing to release yet — just don't mount (mounting
             * allocates) until the requester is done. */
            s_hold_ack = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int64_t now = esp_timer_get_time();
        bool gate = (muse_state_mode(NULL) != MUSE_MODE_BOOT && now - born >= FR_BOOT_DELAY_US)
                    || now - born >= FR_BOOT_ANYWAY_US;
        if (!gate) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        esp_err_t err = muse_meeting_storage_mount();
        if (err == ESP_OK) {
            mounted = true;
        } else if (err == ESP_ERR_NOT_FOUND || tries >= 5) {
            ESP_LOGW(TAG, "storage unavailable (%s): no library face this boot", esp_err_to_name(err));
            hopeless = true;
        } else {
            tries++;
            ESP_LOGW(TAG, "storage mount: %s (retry %d)", esp_err_to_name(err), tries);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (mounted) {
        s_buf[0] = heap_caps_aligned_alloc(32, FR_BYTES, MUSE_BIG_CAPS);
        s_buf[1] = heap_caps_aligned_alloc(32, FR_BYTES, MUSE_BIG_CAPS);
        s_file = heap_caps_malloc(FR_FILE_MAX, MUSE_BIG_CAPS);
        if (!s_buf[0] || !s_buf[1] || !s_file) {
            ESP_LOGE(TAG, "frame buffers unavailable: no library face this boot");
            mounted = false;
        }
    }
    if (!mounted) {
        s_player_done = true;
        vTaskSuspend(NULL);   /* nothing to do this boot */
    }
    ESP_LOGI(TAG, "library up: free internal %u, largest %u, free psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    visual_t cur = s_still_up ? V_STILL : V_NONE;   /* what the widget shows now */
    int seq_idx = 0;              /* frame in the front buffer */
    int seq_next = 0;             /* frame in the back buffer, if ready */
    bool seq_ready = false;       /* back buffer holds seq_next */
    int64_t deadline = 0;         /* when the next frame is due */
    int mouth = -1;               /* speaking frame on screen, -1 none */
    int64_t mouth_since = 0;
    float level_lp = 0;

    for (;;) {
        if (s_hold_req) {
            if (!s_hold_ack) {
                /* Go quiet: drop the JPEG decoder (its scratch is
                 * internal RAM) and decode nothing until released.
                 * The frame on screen stays up. */
                if (s_jdec) {
                    jpeg_dec_close(s_jdec);
                    s_jdec = NULL;
                }
                s_hold_ack = true;
                ESP_LOGI(TAG, "held: free %u, largest %u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (s_hold_ack) {
            s_hold_ack = false;
            deadline = 0;   /* resume the schedule from now: no catch-up skip */
            ESP_LOGI(TAG, "resumed");
        }
        visual_t want = resolve_target();
        /* The waking sequence gets the first calm moment after the
         * mount, even though the boot mode is long past by then; an
         * interactive state (or a recording) starting first spends it. */
        if (!s_waking_done && want != V_PAUSED) {
            if (cur == V_WAKING && (want == V_IDLE || want == V_HAPPY || want == V_WAKING)) {
                want = V_WAKING;   /* let it play out */
            } else if ((cur == V_NONE || cur == V_STILL) && (want == V_IDLE || want == V_HAPPY)) {
                want = V_WAKING;   /* first entry */
            } else if (want != V_WAKING) {
                s_waking_done = true;   /* interrupted or pre-empted */
            }
        }
        /* Idle rotation: the resolver's plain idle is whichever pool
         * variant the shuffle bag is visiting. After the waking gate,
         * so the boot sequence still gets its first calm moment. */
        if (want == V_IDLE) {
            want = s_rot_cur;
        }
        if (want == V_PAUSED) {
            /* Screen dark or settings up: hold the frame, and do not
             * let the schedule run away while nobody is watching. */
            deadline = 0;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        if (want == V_NONE) {
            if (cur != V_NONE) {
                s_dip = false;
                hide_face();
                cur = V_NONE;
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        if (s_broken[want]) {
            s_spec_ready = false;   /* a diversion spends the epoch too */
            s_epoch++;
            s_dip = false;   /* the fallback face comes up as-is */
            s_rot_pending = false;
            if (is_idle_variant(want) && want != V_IDLE) {
                s_rot_cur = V_IDLE;   /* rotation falls back to plain idle */
            }
            if (want == V_WAKING) {
                s_waking_done = true;   /* no waking asset: don't wedge on it */
            }
            if (want == V_LEVELUP) {
                s_levelup_pending = false;   /* same: idle must get its face back */
            }
            cur = fallback_face(cur);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (want != cur) {
            /* Enter a new visual. The two timestamps bracket the
             * switch for the log line below: total is what a state
             * request waits for a new face, build is the part the
             * preload can shrink (asset reads + decode). */
            int64_t t_req = esp_timer_get_time();
            s_enter_swap = true;   /* an enter, not a frame advance */
            int64_t t_build = esp_timer_get_time();
            bool ok = true;
            /* Any enter spends a pending rotation flag. Read
             * before the spec handling below, which can also
             * produce this swap. */
            s_rot_pending = false;
            /* A current-epoch thinking pre-build is pixel-identical
             * to what enter_still would decode: swap it straight in.
             * Any other enter spends the pre-build (logged wasted
             * when there was one). */
            bool spec_hit = want == V_THINKING && s_spec_ready
                            && s_spec_epoch == s_epoch;
            if (s_spec_ready && !spec_hit) {
                ESP_LOGI(TAG, "spec thinking: wasted");
            }
            s_spec_ready = false;
            s_epoch++;
            if (spec_hit) {
                s_swap_vis = want;
                swap_buffers();
                ESP_LOGI(TAG, "spec thinking: hit");
            } else if (SEQS[want].dir) {
                seq_idx = 0;
                seq_ready = false;
                ok = enter_seq_frame(want, 0, back_buffer());
                if (ok) {
                    s_swap_vis = want;
                    swap_buffers();
                    deadline = esp_timer_get_time() + FR_PERIOD_US;
                }
            } else if (want == V_SPEAKING) {
                mouth = mouth_for_level(muse_state_level());
                s_dec_what = "speaking";
                ok = decode_jpeg_file(MOUTH_FILES[mouth], back_buffer());
                if (!ok) {
                    s_broken[want] = true;
                } else {
                    s_swap_vis = want;
                    swap_buffers();
                    mouth_since = esp_timer_get_time();
                }
            } else {
                ok = enter_still(want);
            }
            if (!ok) {
                s_dip = false;   /* the fallback face comes up as-is */
                s_enter_swap = false;   /* no swap happened */
                if (want == V_WAKING) {
                    s_waking_done = true;
                }
                if (want == V_LEVELUP) {
                    s_levelup_pending = false;
                }
                cur = fallback_face(cur);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            ESP_LOGI(TAG, "show %s", VIS_NAMES[want]);
            ESP_LOGI(TAG, "enter %s: %lld ms (build %lld ms)", VIS_NAMES[want],
                     (long long)(esp_timer_get_time() - t_req) / 1000,
                     (long long)(esp_timer_get_time() - t_build) / 1000);
            if (s_dip) {
                s_dip = false;
                dip_fade_in();
            }
            if (is_idle_variant(want)) {
                s_rot_loops = visit_loops(want);   /* a fresh visit */
            }
            if (want != V_WAKING) {
                /* Convergence is waking-only; the swap above already
                 * re-framed this visual without it. */
                s_conv_active = false;
                s_conv_scale = 256;
                s_conv_dy = 0;
            }
            cur = want;
            continue;
        }

        /* Staying in the same visual. */
        if (SEQS[cur].dir) {
            int64_t now = esp_timer_get_time();
            if (!deadline) {
                deadline = now + FR_PERIOD_US;
            }
            bool last = SEQS[cur].oneshot && seq_idx >= seq_cycle(cur) - 1;
            if (now >= deadline && last) {
                /* A one-shot played out (its last frame had its dwell). */
                if (cur == V_WAKING) {
                    s_waking_done = true;
                    if (FR_WAKING_CONVERGE) {
                        /* Land the seam on the endpoint geometry even
                         * when a frame skip left the last displayed
                         * frame short of it (Codex review): force the
                         * final convergence step onto the frame that
                         * is up, then hold it into the cut — the
                         * widget keeps that scale and dy until the
                         * idle enter's own swap re-frames it. */
                        conv_update(SEQS[V_WAKING].frames - 1);
                        muse_board->display_lock(-1);
                        apply_framing(V_WAKING);
                        lv_obj_invalidate(s_img);
                        muse_board->display_unlock();
                        ESP_LOGI(TAG, "waking converge: seam at scale %d, dy %d",
                                 (int)FR_CONV_SCALE_END,
                                 (int)((muse_board->minimal_face ? FR_FACE_DY : 0)
                                       + FR_WAKING_DY + FR_CONV_DY_END));
                    } else {
                        dip_fade_out();   /* dip through black into idle */
                    }
                }
                if (cur == V_LEVELUP) {
                    s_levelup_pending = false;
                    /* No fade-out (see the note at the old fade_out
                     * site): the last frame stays up and the next
                     * visual's swap replaces it whole. */
                }
                if (is_idle_variant(cur)) {
                    /* A one-shot pool member (the flip) ends its
                     * visit here instead of at a loop wrap. */
                    rot_advance(cur);
                }
                cur = V_NONE;   /* the resolver picks the next visual */
                continue;
            }
            if (now >= deadline && seq_ready) {
                if (cur == V_WAKING) {
                    conv_update(seq_next);   /* geometry for the frame
                                              * this swap puts up */
                }
                s_swap_vis = cur;
                swap_buffers();
                seq_idx = seq_next;
                seq_ready = false;
                if (seq_idx == 0 && is_idle_variant(cur)
                    && s_rot_loops > 0 && --s_rot_loops == 0) {
                    /* The visit's loops are played: the next pass
                     * enters the picked variant, hard-cut. */
                    rot_advance(cur);
                }
                /* Keep the 125 ms grid: count whole periods the decode
                 * overran and skip that many frames ahead. */
                int64_t late = now - deadline;
                deadline += FR_PERIOD_US;
                if (late >= FR_PERIOD_US) {
                    int skip = (int)(late / FR_PERIOD_US);
                    s_dec_skipped += (uint32_t)skip;
                    deadline += (int64_t)skip * FR_PERIOD_US;
                    seq_idx += skip;
                    if (seq_idx >= seq_cycle(cur)) {
                        if (SEQS[cur].oneshot) {
                            seq_idx = seq_cycle(cur) - 1;
                        } else {
                            if (is_idle_variant(cur) && s_rot_loops > 0) {
                                /* Wraps the skip jumped over complete
                                 * loops too (Codex review, 2026-11-17):
                                 * count them, or a loaded player could
                                 * stretch a visit indefinitely. */
                                int wraps = seq_idx / seq_cycle(cur);
                                s_rot_loops -= wraps < s_rot_loops ? wraps : s_rot_loops;
                                if (s_rot_loops == 0) {
                                    rot_advance(cur);
                                }
                            }
                            seq_idx %= seq_cycle(cur);
                        }
                    }
                }
                now = esp_timer_get_time();
            }
            if (!seq_ready && !last) {
                int next = seq_idx + 1;
                if (next >= seq_cycle(cur)) {
                    next = 0;   /* loops wrap; one-shots never get here (last) */
                }
                if (!enter_seq_frame(cur, next, back_buffer())) {
                    if (cur == V_WAKING) {
                        s_waking_done = true;
                    }
                    if (cur == V_LEVELUP) {
                        s_levelup_pending = false;
                    }
                    cur = fallback_face(cur);
                    continue;
                }
                seq_next = next;
                seq_ready = true;
            }
            if (is_idle_variant(cur) && seq_ready && s_waking_done
                && esp_timer_get_time() + 30000 < deadline) {
                /* An idle variant with the next frame already decoded
                 * and a real margin before it is due: spend the slack
                 * warming the first-frame preload, one asset per
                 * pass (see FR_PRELOAD_PATHS). The deadline gate is
                 * the Codex review's point — seq_ready alone does
                 * not prove the read cannot crowd the swap. */
                preload_step();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (cur == V_SPEAKING) {
            float level = muse_state_level();
            level_lp += (level - level_lp) * (level > level_lp ? 0.6f : 0.25f);
            int m = mouth_for_level(level_lp);
            int64_t now = esp_timer_get_time();
            if (m != mouth && now - mouth_since >= 90000) {
                s_dec_what = "speaking";
                if (decode_jpeg_file(MOUTH_FILES[m], back_buffer())) {
                    s_swap_vis = V_SPEAKING;
                    swap_buffers();
                    mouth = m;
                    mouth_since = now;
                } else {
                    s_broken[cur] = true;
                    cur = fallback_face(cur);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        /* Statics: nothing to do until the state changes — except,
         * while listening sits up, speculatively building thinking
         * into the back buffer (see s_spec_ready) and breathing on
         * the still (see the breath section). The resolver runs
         * again immediately after, so a state change that landed
         * during the decode is picked up next pass. */
        if (cur == V_LISTENING) {
            breath_start();
        }
        if (cur == V_LISTENING && !s_spec_ready && !s_broken[V_THINKING]) {
            int64_t t_spec = esp_timer_get_time();
            if (decode_still(V_THINKING, back_buffer())) {
                s_spec_ready = true;
                s_spec_epoch = s_epoch;
                ESP_LOGI(TAG, "spec thinking: built in %lld ms",
                         (long long)(esp_timer_get_time() - t_spec) / 1000);
            } else {
                s_broken[V_THINKING] = true;
                ESP_LOGW(TAG, "thinking: asset unusable, fallback face for this state");
            }
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

void muse_frames_attach(lv_obj_t *face)
{
    s_img = lv_image_create(face);
    lv_obj_set_size(s_img, FR_W, FR_H);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, muse_board->minimal_face ? FR_FACE_DY : 0);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = FR_W;
    s_dsc.header.h = FR_H;
    s_dsc.header.stride = FR_W * 2;
    s_dsc.data_size = FR_BYTES;

    /* The embedded boot still, when this build links one: decode it
     * now — in the UI task, before the player starts — so the face on
     * screen from the first refresh is the current one, and the pixel
     * canvas underneath (blanked by muse_ui.c on minimal-face boards)
     * never shows through while the library mounts. The decoder is
     * opened and closed inside this block, so its internal scratch is
     * freed before the voice session makes its first allocations; the
     * decoded frame itself lives in PSRAM and stays resident all boot
     * as the player's fallback (see fallback_face()). No lock is taken
     * here: like the widget creation above, this runs in the UI's own
     * context during build_screen(). */
    size_t still_len = 0;
    const uint8_t *still_jpg = muse_boot_still_jpg(&still_len);
    if (still_jpg) {
        s_still = heap_caps_aligned_alloc(32, FR_BYTES, MUSE_BIG_CAPS);
        jpeg_dec_handle_t dec = NULL;
        jpeg_dec_config_t cfg = {
            .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
            .rotate = JPEG_ROTATE_0D,
        };
        bool ok = false;
        if (s_still && jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK && dec) {
            ok = jpeg_decode(dec, still_jpg, still_len, s_still);
            jpeg_dec_close(dec);
        }
        if (ok) {
            s_still_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_still_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_still_dsc.header.w = FR_W;
            s_still_dsc.header.h = FR_H;
            s_still_dsc.header.stride = FR_W * 2;
            s_still_dsc.data_size = FR_BYTES;
            s_still_dsc.data = (const uint8_t *)s_still;
            s_still_up = true;
            lv_image_set_src(s_img, &s_still_dsc);
            apply_framing(V_STILL);   /* same placement waking gets */
            lv_obj_set_style_opa(s_img, LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_img);
            ESP_LOGI(TAG, "boot still up: %u bytes of JPEG", (unsigned)still_len);
        } else {
            ESP_LOGW(TAG, "boot still unusable: library/pixel fallback only");
            heap_caps_free(s_still);
            s_still = NULL;
        }
    }

    /* PSRAM stack: this task reads the storage partition but never
     * writes flash itself (wear-levelling reads can still coincide
     * with the meeting writer's cache-off windows; while a recording
     * runs the player sits on the rest still and stays off the disk).
     * Pinned to FR_PLAYER_CORE (see its comment): the decode runs on
     * the network core, not the UI core, so a decode burst and the
     * UI's own rendering no longer preempt each other. Priority 3
     * stays below the network tasks on that core. */
    if (xTaskCreatePinnedToCoreWithCaps(player_task, "muse_frames", 8192, NULL, 3, NULL,
                                        FR_PLAYER_CORE, MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGW(TAG, "player task unavailable: no library face");
    }
}

bool muse_frames_boot_still_up(void)
{
    return s_still_up;
}

bool muse_frames_hold(bool on)
{
    if (!on) {
        s_hold_req = false;
        return true;
    }
    if (!s_player_alive || s_player_done) {
        return true;   /* nothing is decoding: already quiet */
    }
    s_hold_req = true;
    for (int waited = 0; waited < 2000; waited += 25) {
        if (s_hold_ack) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    ESP_LOGW(TAG, "hold not acknowledged in 2 s; caller proceeds anyway");
    return false;
}
