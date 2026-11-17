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

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "png.h"

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
 * sequences, sprites/<state>/ kits (blank.jpg + geom.json + PNG parts)
 * and poses/<state>.jpg stills, all 466x466. This player is the only
 * reader besides the meeting console's dump.
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
 * edges, pet reactions) used to hard-cut: the old frame froze for
 * the ~0.2 s the new one took to build, then snapped. A true
 * crossfade needs old and new frames on screen at once — a third
 * 434 KB frame buffer — and the library leaves only ~250-435 KB of
 * PSRAM free, so that does not fit. What fits is a shallow dip: fade
 * the old frame down to FR_SOFT_DIP_BOTTOM while the new one builds,
 * swap at the bottom, fade back up. ~40 ms each way, ~120 ms around
 * the swap. It is a dip, not a crossfade — the record says so too.
 * The waking handoff keeps its own full dip-to-black (Hugh-approved,
 * FR dip above); this one never applies to it. */
#define FR_SOFT_DIP_BOTTOM 160   /* of 255: ~63% */
#define FR_SOFT_DIP_STEP 95      /* one step each way: 255 <-> 160.
                                  * A finer staircase was measured on
                                  * device at ~270 ms of transition
                                  * overhead — every step takes the
                                  * display lock and waits out a full
                                  * QSPI flush — so the dip is a single
                                  * step down, swap, single step up. */
#define FR_SOFT_DIP_MS 20

/* The squash's low point (image scale, 256 = 1.0x). */
#define FR_SQUASH_MIN 246        /* ~96% */

/* First-frame preload: only the three big first frames behind the
 * hottest switches — the listening and thinking kit blanks and the
 * level-up celebration's first frame — read into PSRAM once the
 * player is idling, so entering those states skips the slowest FAT
 * reads. ~86 KB in total at the shipped sizes. The kits' small
 * sprites and geom files stay on the file path on purpose: the
 * first device run of this round warmed all fifteen assets (~148
 * KB) and the free-PSRAM margin it left (~100 KB) was thin enough
 * to hurt neighbours; the blanks are the reads that dominate the
 * build time anyway. Every entry is optional and a failed
 * allocation just leaves that asset on the file path. Listening
 * first (every voice turn starts there), then level-up, then
 * thinking. */
static const char *const FR_PRELOAD_PATHS[] = {
    "/meet/sprites/listening/blank.jpg",
    "/meet/frames/levelup/f000.jpg",
    "/meet/sprites/thinking/blank.jpg",
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
    int frames;
    bool oneshot;
} seq_def_t;

static const seq_def_t SEQS[V_COUNT] = {
    [V_WAKING] = { "waking", 56, true },
    [V_IDLE] = { "idle", 40, false },
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
};

/* Kit states are composited from sprites/<name>/; still states are a
 * single poses/<name>.jpg. Speaking is three frames picked by level. */
static bool vis_is_kit(visual_t v)
{
    return v == V_LISTENING || v == V_THINKING || v == V_SLEEPY || v == V_DIZZY;
}
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

/* ---- PNG sprites (kits) ---- */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
} png_src_t;

static void png_read_cb(png_structp png, png_bytep out, png_size_t n)
{
    png_src_t *s = (png_src_t *)png_get_io_ptr(png);
    if (s->pos + n > s->len) {
        png_error(png, "short read");
        return;
    }
    memcpy(out, s->data + s->pos, n);
    s->pos += n;
}

/* Decode a PNG in memory to a fresh PSRAM RGBA8888 buffer (row-major,
 * R,G,B,A per pixel). Caller frees with heap_caps_free. */
static bool decode_png_mem(const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return false;
    }
    /* Volatile: both are assigned after setjmp, so a longjmp out of a
     * corrupt PNG would otherwise leave them indeterminate and the
     * cleanup below could free a garbage pointer — exactly in the
     * broken-asset case this decoder exists to survive. */
    volatile bool ok = false;
    uint8_t *volatile buf = NULL;
    if (setjmp(png_jmpbuf(png))) {
        goto done;
    }
    {
        png_src_t src = { data, len, 0 };
        png_set_read_fn(png, &src, png_read_cb);
        png_read_info(png, info);
        png_uint_32 pw, ph;
        int bit_depth, color_type;
        png_get_IHDR(png, info, &pw, &ph, &bit_depth, &color_type, NULL, NULL, NULL);
        if (pw == 0 || ph == 0 || pw > 512 || ph > 512) {
            goto done;
        }
        if (color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_palette_to_rgb(png);
        }
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
            png_set_expand_gray_1_2_4_to_8(png);
        }
        if (png_get_valid(png, info, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png);
        }
        if (bit_depth == 16) {
            png_set_strip_16(png);
        }
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png);
        }
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
            png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
        }
        png_read_update_info(png, info);   /* rows come out R,G,B,A */
        buf = heap_caps_malloc((size_t)pw * ph * 4, MUSE_BIG_CAPS);
        if (!buf) {
            goto done;
        }
        for (png_uint_32 y = 0; y < ph; y++) {
            png_read_row(png, buf + (size_t)y * pw * 4, NULL);
        }
        *rgba = buf;
        *w = (int)pw;
        *h = (int)ph;
        ok = true;
    }
done:
    if (!ok) {
        heap_caps_free((void *)buf);
    }
    png_destroy_read_struct(&png, &info, NULL);
    return ok;
}

/* Alpha-blend one RGBA sprite over the RGB565 frame at (x0, y0),
 * clipped to the frame. Blending is done in 565 component space —
 * a hair of banding on the soft blush edges, invisible at arm's length. */
static void blend_sprite(uint16_t *frame, const uint8_t *rgba, int w, int h, int x0, int y0)
{
    for (int y = 0; y < h; y++) {
        int fy = y0 + y;
        if (fy < 0 || fy >= FR_H) {
            continue;
        }
        for (int x = 0; x < w; x++) {
            int fx = x0 + x;
            if (fx < 0 || fx >= FR_W) {
                continue;
            }
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            uint32_t a = p[3];
            if (a == 0) {
                continue;
            }
            uint16_t *dst = &frame[(size_t)fy * FR_W + fx];
            if (a == 255) {
                *dst = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                continue;
            }
            uint32_t ia = 255 - a;
            uint32_t r = (((*dst >> 11) & 0x1f) * ia + (p[0] >> 3) * a) / 255;
            uint32_t g = (((*dst >> 5) & 0x3f) * ia + (p[1] >> 2) * a) / 255;
            uint32_t b = ((*dst & 0x1f) * ia + (p[2] >> 3) * a) / 255;
            *dst = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* Build a kit state's face into out: the featureless blank, then the
 * five sprites at the rectangles geom.json records. */
static bool build_kit(const char *name, uint16_t *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/meet/sprites/%s/blank.jpg", name);
    size_t clen;
    const uint8_t *cached = preload_get(path, &clen);
    if (cached ? !decode_jpeg_mem(cached, clen, out) : !decode_jpeg_file(path, out)) {
        return false;
    }
    snprintf(path, sizeof(path), "/meet/sprites/%s/geom.json", name);
    size_t len;
    cached = preload_get(path, &clen);
    if (cached) {
        /* The parser wants a NUL-terminated buffer: stage the
         * resident copy through the scratch, like a file read. */
        if (clen >= FR_FILE_MAX) {
            return false;
        }
        memcpy(s_file, cached, clen);
        len = clen;
    } else if (!read_file(path, &len) || len >= FR_FILE_MAX) {
        return false;
    }
    s_file[len] = '\0';   /* len < FR_FILE_MAX here, so this is in bounds */
    cJSON *root = cJSON_Parse((const char *)s_file);
    if (!root) {
        return false;
    }
    cJSON *sprites = cJSON_GetObjectItemCaseSensitive(root, "sprites");
    static const char *const order[] = {
        "spr_blush_L", "spr_blush_R", "spr_eye_L", "spr_eye_R", "spr_mouth",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]) && ok; i++) {
        cJSON *sp = cJSON_GetObjectItemCaseSensitive(sprites, order[i]);
        cJSON *rect = sp ? cJSON_GetObjectItemCaseSensitive(sp, "rect") : NULL;
        if (!rect || cJSON_GetArraySize(rect) != 4) {
            ok = false;
            break;
        }
        int x0 = cJSON_GetArrayItem(rect, 0)->valueint;
        int y0 = cJSON_GetArrayItem(rect, 1)->valueint;
        snprintf(path, sizeof(path), "/meet/sprites/%s/%s.png", name, order[i]);
        size_t plen;
        const uint8_t *pdata = preload_get(path, &plen);
        if (!pdata) {
            if (!read_file(path, &plen)) {
                ok = false;
                break;
            }
            pdata = s_file;
        }
        /* Decode from the bytes as-is: decode_png_mem keeps no
         * reference after the call, so the shared scratch and the
         * resident copy are both safe sources. */
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        if (!decode_png_mem(pdata, plen, &rgba, &w, &h)) {
            ok = false;
            break;
        }
        blend_sprite(out, rgba, w, h, x0, y0);
        heap_caps_free(rgba);
    }
    cJSON_Delete(root);
    return ok;
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

/* Mid-soft-dip (interactive switches, see FR_SOFT_DIP_BOTTOM): the
 * incoming visual must start at the dip's bottom opa, not at full,
 * so the player can fade it back up. Set by the enter path just
 * before its swap; consumed — read and cleared — by show_front(),
 * so no later frame swap can inherit it. */
static bool s_soft_hold;

/* Display lock held by the caller (the show_* paths take it). */
/* The live squash factor (256 = 1.0x), written by the squash
 * animation in LVGL context and read here so a frame swap mid-squash
 * re-applies the in-flight factor instead of resetting it. */
static volatile int32_t s_squash_factor = 256;

static void apply_framing(visual_t v)
{
    lv_image_set_scale(s_img, s_squash_factor);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0,
                 (muse_board->minimal_face ? FR_FACE_DY : 0) +
                 ((v == V_WAKING || v == V_STILL) ? FR_WAKING_DY : 0));
}

static void squash_exec(void *obj, int32_t v);   /* defined with the squash */

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
    int opa = s_dip ? 0 : (s_soft_hold ? FR_SOFT_DIP_BOTTOM : LV_OPA_COVER);
    muse_board->display_lock(-1);
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
    /* Mid-dip the incoming visual starts black; the player fades it
     * in right after the swap (see dip_fade_out()). Mid-soft-dip it
     * starts at the dip's bottom instead (see s_soft_hold). */
    lv_obj_set_style_opa(s_img, (lv_opa_t)opa, 0);
    s_soft_hold = false;
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
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    muse_board->display_unlock();
}

static void show_still(void)
{
    muse_board->display_lock(-1);
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

/* levelup's exit: fade the last frame out (whatever is under the
 * widget shows through — the blanked canvas on still builds), then
 * the caller moves on to idle. */
static void fade_out(void)
{
    for (int opa = 200; opa >= 0; opa -= 55) {
        muse_board->display_lock(-1);
        lv_obj_set_style_opa(s_img, (lv_opa_t)opa, 0);
        lv_obj_invalidate(s_img);
        muse_board->display_unlock();
        vTaskDelay(pdMS_TO_TICKS(45));
    }
    hide_face();
}

/* Waking's exit dips through black, like fade_out() above but in
 * finer steps (~240 ms each way) and leaving the widget up at
 * opa 0: the waking and idle artwork was shot at different
 * distances — the face grows ~19% at the cut — and the dip turns
 * that size step into a blink. The ramp runs the full range from
 * LV_OPA_COVER in 32-opa steps: starting mid-way (or stepping
 * coarsely) is itself a visible jump at the cut. Only the waking
 * handoff uses this. The fade-out runs in the player task when
 * the one-shot plays out; s_dip then makes the next swap start
 * black (show_front), and the player fades the new visual in
 * right after entering it. The display lock is taken per step,
 * as in fade_out(), so the UI task is never blocked by the
 * delays. */
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

/* The soft dip for interactive switches (see FR_SOFT_DIP_BOTTOM):
 * the outgoing face sinks to ~63% while the incoming one builds,
 * the swap lands at the bottom (show_front honours s_soft_hold),
 * and soft_dip_fade_in() brings it back up. Same per-step lock
 * discipline as the waking dip. Not a crossfade: the two faces are
 * never on screen together. */
static void soft_dip_fade_out(void)
{
    muse_board->display_lock(-1);
    if (present_ok()) {   /* decided under the lock: see show_front */
        /* The same single step, presented whole (present_now): the
         * dimming used to be a four-band LVGL sweep of its own. The
         * border is NOT repaired here: it keeps the old face at full
         * brightness while the new one builds (the chrome over it
         * never dims either), and the swap's present_border() brings
         * it to the new face at the dip's bottom in one go — an LVGL
         * redraw here would only fight the build for PSRAM bandwidth
         * (measured: ~100 ms on a kit build). */
        present_squash_cancel();
        int y1 = present_bottom();
        lv_obj_set_style_opa(s_img, FR_SOFT_DIP_BOTTOM, 0);
        present_now(FR_SOFT_DIP_BOTTOM, y1);
        muse_board->display_unlock();
        return;
    }
    muse_board->display_unlock();
    for (int opa = LV_OPA_COVER; opa > FR_SOFT_DIP_BOTTOM; opa -= FR_SOFT_DIP_STEP) {
        muse_board->display_lock(-1);
        lv_obj_set_style_opa(s_img, (lv_opa_t)(opa - FR_SOFT_DIP_STEP < FR_SOFT_DIP_BOTTOM
                                                  ? FR_SOFT_DIP_BOTTOM
                                                  : opa - FR_SOFT_DIP_STEP),
                             0);
        lv_obj_invalidate(s_img);
        muse_board->display_unlock();
        vTaskDelay(pdMS_TO_TICKS(FR_SOFT_DIP_MS));
    }
}

static void soft_dip_fade_in(void)
{
    muse_board->display_lock(-1);
    if (present_ok()) {   /* decided under the lock: see show_front */
        present_squash_cancel();
        int y1 = present_bottom();
        lv_obj_set_style_opa(s_img, LV_OPA_COVER, 0);
        present_now(LV_OPA_COVER, y1);
        muse_board->display_unlock();
        present_border(y1);
        return;
    }
    muse_board->display_unlock();
    for (int opa = FR_SOFT_DIP_BOTTOM; opa < LV_OPA_COVER; opa += FR_SOFT_DIP_STEP) {
        muse_board->display_lock(-1);
        lv_obj_set_style_opa(s_img, (lv_opa_t)(opa + FR_SOFT_DIP_STEP > LV_OPA_COVER
                                                  ? LV_OPA_COVER
                                                  : opa + FR_SOFT_DIP_STEP),
                             0);
        lv_obj_invalidate(s_img);
        muse_board->display_unlock();
        vTaskDelay(pdMS_TO_TICKS(FR_SOFT_DIP_MS));
    }
}

/* Whether the cur -> want switch gets the soft dip: only between
 * two interactive faces. Boot (STILL/NONE), the waking handoff (its
 * own defined dip, and want==V_WAKING never), sleep (PAUSED) and
 * fallback entries stay exactly as they were. */
static bool soft_dip_pair(visual_t cur, visual_t want)
{
    if (want == V_WAKING || want == V_NONE || want == V_PAUSED) {
        return false;
    }
    switch (cur) {
    case V_IDLE:
    case V_LISTENING:
    case V_THINKING:
    case V_SPEAKING:
    case V_WORKING:
    case V_MAKING:
    case V_LEVELUP:
    case V_HAPPY:
    case V_SLEEPY:
    case V_DIZZY:
    case V_REST:
        return true;
    default:
        return false;
    }
}

/* ---- which visual should be up ---- */

static visual_t s_last_active = V_NONE;   /* last SPEAKING/THINKING seen */
static TickType_t s_last_active_tick;     /* when it was last seen */
static bool s_levelup_pending;
static float s_prev_happy;
static bool s_waking_done;

/* Pet taps (muse_frames_pet_tap): a pending request plus which of the
 * two reactions is next. Written by the UI task, consumed only by the
 * player's resolver in IDLE, so a tap lands between turns, never in
 * the middle of one. */
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

/* Decode visual v's still content (kit composite or poses still) into
 * the back buffer and swap it in. False (and v marked broken) when the
 * asset is unusable; the fallback face stays up then. */
static bool enter_still(visual_t v)
{
    bool ok;
    if (vis_is_kit(v)) {
        s_dec_what = VIS_NAMES[v];
        ok = build_kit(VIS_NAMES[v], back_buffer());
    } else {
        char path[96];
        snprintf(path, sizeof(path), "/meet/poses/%s.jpg", VIS_NAMES[v]);
        s_dec_what = VIS_NAMES[v];
        ok = decode_jpeg_file(path, back_buffer());
    }
    if (!ok) {
        s_broken[v] = true;
        ESP_LOGW(TAG, "%s: asset unusable, fallback face for this state", VIS_NAMES[v]);
        return false;
    }
    s_swap_vis = v;
    swap_buffers();
    return true;
}

static bool enter_seq_frame(visual_t v, int idx, uint16_t *into)
{
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
            s_dip = false;   /* the fallback face comes up as-is */
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
             * preload can shrink (asset reads + decode/composite). */
            int64_t t_req = esp_timer_get_time();
            s_enter_swap = true;   /* an enter, not a frame advance */
            bool soft = !s_dip && soft_dip_pair(cur, want);
            if (soft) {
                soft_dip_fade_out();   /* the build below runs dimmed */
            }
            int64_t t_build = esp_timer_get_time();
            bool ok = true;
            if (SEQS[want].dir) {
                seq_idx = 0;
                seq_ready = false;
                ok = enter_seq_frame(want, 0, back_buffer());
                if (ok) {
                    s_swap_vis = want;
                    s_soft_hold = soft;
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
                    s_soft_hold = soft;
                    swap_buffers();
                    mouth_since = esp_timer_get_time();
                }
            } else {
                s_soft_hold = soft;
                ok = enter_still(want);
            }
            if (!ok) {
                s_dip = false;   /* the fallback face comes up as-is */
                s_soft_hold = false;
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
            } else if (soft) {
                soft_dip_fade_in();
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
            bool last = SEQS[cur].oneshot && seq_idx >= SEQS[cur].frames - 1;
            if (now >= deadline && last) {
                /* A one-shot played out (its last frame had its dwell). */
                if (cur == V_WAKING) {
                    s_waking_done = true;
                    dip_fade_out();   /* dip through black into idle */
                }
                if (cur == V_LEVELUP) {
                    s_levelup_pending = false;
                    fade_out();
                }
                cur = V_NONE;   /* the resolver picks the next visual */
                continue;
            }
            if (now >= deadline && seq_ready) {
                s_swap_vis = cur;
                swap_buffers();
                seq_idx = seq_next;
                seq_ready = false;
                /* Keep the 125 ms grid: count whole periods the decode
                 * overran and skip that many frames ahead. */
                int64_t late = now - deadline;
                deadline += FR_PERIOD_US;
                if (late >= FR_PERIOD_US) {
                    int skip = (int)(late / FR_PERIOD_US);
                    s_dec_skipped += (uint32_t)skip;
                    deadline += (int64_t)skip * FR_PERIOD_US;
                    seq_idx += skip;
                    if (seq_idx >= SEQS[cur].frames) {
                        seq_idx = SEQS[cur].oneshot ? SEQS[cur].frames - 1
                                                     : seq_idx % SEQS[cur].frames;
                    }
                }
                now = esp_timer_get_time();
            }
            if (!seq_ready && !last) {
                int next = seq_idx + 1;
                if (next >= SEQS[cur].frames) {
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
            if (cur == V_IDLE && seq_ready && s_waking_done
                && esp_timer_get_time() + 30000 < deadline) {
                /* Idle with the next frame already decoded and a
                 * real margin before it is due: spend the slack
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

        /* Statics: nothing to do until the state changes. */
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
