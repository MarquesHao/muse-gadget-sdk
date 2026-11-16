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
    if (!decode_jpeg_file(path, out)) {
        return false;
    }
    snprintf(path, sizeof(path), "/meet/sprites/%s/geom.json", name);
    size_t len;
    if (!read_file(path, &len) || len >= FR_FILE_MAX) {
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
        if (!read_file(path, &plen)) {
            ok = false;
            break;
        }
        /* Decode from a copy: the PNG bytes share s_file with nothing
         * else right now, but decode_png_mem keeps no reference after
         * the call, so this is safe as-is. */
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        if (!decode_png_mem(s_file, plen, &rgba, &w, &h)) {
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

static void show_front(void)
{
    muse_board->display_lock(-1);
    s_dsc.data = (const uint8_t *)s_buf[s_front];
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_style_opa(s_img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_img);
    muse_board->display_unlock();
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

/* ---- which visual should be up ---- */

static visual_t s_last_active = V_NONE;   /* last SPEAKING/THINKING seen */
static TickType_t s_last_active_tick;     /* when it was last seen */
static bool s_levelup_pending;
static float s_prev_happy;
static bool s_waking_done;

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
    swap_buffers();
    return true;
}

static bool enter_seq_frame(visual_t v, int idx, uint16_t *into)
{
    char path[96];
    snprintf(path, sizeof(path), "/meet/frames/%s/f%03d.jpg", SEQS[v].dir, idx);
    s_dec_what = VIS_NAMES[v];
    if (!decode_jpeg_file(path, into)) {
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
    int64_t born = esp_timer_get_time();

    /* Mount once boot has settled (see the file header). A partition
     * that will not mount is retried a few times in case the boot
     * prewarm was still running, then given up on for this boot. */
    bool mounted = false;
    bool hopeless = false;
    int tries = 0;   /* real mount attempts only — the gate waits below
                      * must not eat the retry budget */
    while (!mounted && !hopeless) {
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
                hide_face();
                cur = V_NONE;
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        if (s_broken[want]) {
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
            /* Enter a new visual. */
            bool ok = true;
            if (SEQS[want].dir) {
                seq_idx = 0;
                seq_ready = false;
                ok = enter_seq_frame(want, 0, back_buffer());
                if (ok) {
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
                    swap_buffers();
                    mouth_since = esp_timer_get_time();
                }
            } else {
                ok = enter_still(want);
            }
            if (!ok) {
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
                }
                if (cur == V_LEVELUP) {
                    s_levelup_pending = false;
                    fade_out();
                }
                cur = V_NONE;   /* the resolver picks the next visual */
                continue;
            }
            if (now >= deadline && seq_ready) {
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
     * Core and priority sit below the UI task on its own core, so a
     * decode burst never delays the UI's own rendering. */
    if (xTaskCreatePinnedToCoreWithCaps(player_task, "muse_frames", 8192, NULL, 3, NULL,
                                        MUSE_UI_CORE, MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGW(TAG, "player task unavailable: no library face");
    }
}

bool muse_frames_boot_still_up(void)
{
    return s_still_up;
}
