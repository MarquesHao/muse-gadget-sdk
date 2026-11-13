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

#include "muse_meeting.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "wear_levelling.h"

static const char *TAG = "muse_meeting";

#define MEET_BASE "/meet"
#define MEET_PART_LABEL "storage"
#define MEET_POOL_BLOCKS 48   /* 1017 samples per block (~64 ms): the pool holds ~3 s, 24 KB in PSRAM */
#define MEET_HDR_BYTES 60     /* RIFF(12) + fmt(28) + fact(12) + data header(8) */
#define MEET_RESERVE_BYTES (256 * 1024)
#define MEET_MIN_FREE_BLOCKS 944   /* refuse to start with under ~1 minute of space */
#define MEET_NVS_NS "muse_meet"

/*
 * WAV header for IMA-ADPCM (format tag 0x11), one channel at 16 kHz,
 * 512-byte blocks of 1017 samples. Sizes and the fact chunk's sample
 * count are patched in when the file is finalized.
 */
static const uint8_t WAV_HDR[MEET_HDR_BYTES] = {
    'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ', 20, 0, 0, 0,
    0x11, 0x00,             /* IMA-ADPCM */
    1, 0,                   /* mono */
    0x80, 0x3e, 0x00, 0x00, /* 16000 Hz */
    0x77, 0x1f, 0x00, 0x00, /* byte rate: 16000 * 512 / 1017 = 8055 */
    0x00, 0x02,             /* block align 512 */
    4, 0,                   /* 4 bits per sample */
    2, 0,                   /* cbSize */
    0xf9, 0x03,             /* 1017 samples per block */
    'f', 'a', 'c', 't', 4, 0, 0, 0, 0, 0, 0, 0,
    'd', 'a', 't', 'a', 0, 0, 0, 0,
};
#define HDR_OFF_RIFF_SIZE 4
#define HDR_OFF_FACT_SAMPLES 48
#define HDR_OFF_DATA_SIZE 56

typedef struct {
    uint8_t b[MUSE_MEETING_BLOCK_BYTES];
} meet_blk_t;

enum { MSG_BEGIN, MSG_DATA, MSG_END };
typedef struct {
    int kind;
    meet_blk_t *blk;        /* MSG_DATA: ownership passes to the writer */
    uint32_t total_samples; /* MSG_END */
    char name[20];          /* MSG_BEGIN */
} meet_msg_t;

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;
static bool s_setup;
static QueueHandle_t s_wq, s_freeq;
static SemaphoreHandle_t s_done;
static SemaphoreHandle_t s_lock;   /* serializes mount + state snapshots across caller tasks */
static atomic_int s_init_state;    /* 0 new, 1 initializing, 2 done */
static esp_err_t s_init_err = ESP_OK;
static volatile esp_err_t s_op_err = ESP_OK;
static FILE *s_fp;                 /* writer task only */
static uint32_t s_data_bytes;      /* writer task only */
static char s_fp_path[32];         /* writer task only */
static char s_current[20];         /* file being recorded, "" when none */
static bool s_recording;

static void put_u32(uint8_t *at, uint32_t v)
{
    at[0] = v & 0xff;
    at[1] = (v >> 8) & 0xff;
    at[2] = (v >> 16) & 0xff;
    at[3] = (v >> 24) & 0xff;
}

static void writer_task(void *arg)
{
    (void)arg;
    meet_msg_t m;
    for (;;) {
        xQueueReceive(s_wq, &m, portMAX_DELAY);
        if (m.kind == MSG_BEGIN) {
            if (s_fp) {
                fclose(s_fp);   /* a previous session never ended: abandon it, don't leak the handle */
                s_fp = NULL;
            }
            snprintf(s_fp_path, sizeof(s_fp_path), MEET_BASE "/%s", m.name);
            s_data_bytes = 0;
            s_op_err = ESP_OK;
            s_fp = fopen(s_fp_path, "wb");
            if (!s_fp) {
                s_op_err = ESP_FAIL;
            } else if (fwrite(WAV_HDR, 1, sizeof(WAV_HDR), s_fp) != sizeof(WAV_HDR)) {
                s_op_err = ESP_FAIL;
                fclose(s_fp);
                s_fp = NULL;
            }
            xSemaphoreGive(s_done);
        } else if (m.kind == MSG_DATA) {
            if (s_fp) {
                if (fwrite(m.blk->b, 1, MUSE_MEETING_BLOCK_BYTES, s_fp) != MUSE_MEETING_BLOCK_BYTES) {
                    s_op_err = ESP_FAIL;
                } else {
                    s_data_bytes += MUSE_MEETING_BLOCK_BYTES;
                    /* Bound what a power cut can lose: flush every ~32 KB. */
                    if ((s_data_bytes & 0x7fff) == 0) {
                        fflush(s_fp);
                    }
                }
            }
            xQueueSend(s_freeq, &m.blk, 0);
        } else {   /* MSG_END */
            if (s_fp) {
                bool empty = m.total_samples == 0 && s_data_bytes == 0;
                uint8_t word[4];
                put_u32(word, MEET_HDR_BYTES - 8 + s_data_bytes);
                if (fseek(s_fp, HDR_OFF_RIFF_SIZE, SEEK_SET) || fwrite(word, 1, 4, s_fp) != 4) {
                    s_op_err = ESP_FAIL;
                }
                put_u32(word, m.total_samples);
                if (fseek(s_fp, HDR_OFF_FACT_SAMPLES, SEEK_SET) || fwrite(word, 1, 4, s_fp) != 4) {
                    s_op_err = ESP_FAIL;
                }
                put_u32(word, s_data_bytes);
                if (fseek(s_fp, HDR_OFF_DATA_SIZE, SEEK_SET) || fwrite(word, 1, 4, s_fp) != 4) {
                    s_op_err = ESP_FAIL;
                }
                fflush(s_fp);
                fclose(s_fp);
                s_fp = NULL;
                if (empty) {
                    remove(s_fp_path);   /* a zero-length meeting leaves no file */
                }
            }
            xSemaphoreGive(s_done);
        }
    }
}

static esp_err_t setup(void)
{
    if (s_setup) {
        return ESP_OK;
    }
    meet_blk_t *pool = heap_caps_malloc(MEET_POOL_BLOCKS * sizeof(meet_blk_t), MALLOC_CAP_SPIRAM);
    s_wq = xQueueCreate(MEET_POOL_BLOCKS + 4, sizeof(meet_msg_t));
    s_freeq = xQueueCreate(MEET_POOL_BLOCKS, sizeof(meet_blk_t *));
    s_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    if (!pool || !s_wq || !s_freeq || !s_done || !s_lock) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < MEET_POOL_BLOCKS; i++) {
        meet_blk_t *blk = &pool[i];
        xQueueSend(s_freeq, &blk, 0);
    }
    /* Plain xTaskCreate: an internal-RAM stack, safe under flash writes. */
    if (xTaskCreate(writer_task, "muse_meet_wr", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_setup = true;
    return ESP_OK;
}

/* setup() exactly once, whichever task (voice, console, boot) gets here
 * first; the losers wait for the winner instead of double-allocating. */
static esp_err_t init_once(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_init_state, &expected, 1)) {
        s_init_err = setup();
        atomic_store(&s_init_state, 2);
        return s_init_err;
    }
    while (atomic_load(&s_init_state) != 2) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return s_init_err;
}

/* Raw mount; the caller holds s_lock. format=true lets the VFS format a
 * partition that will not mount — the caller decides when that is OK. */
static esp_err_t mount(bool format)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, MEET_PART_LABEL)) {
        return ESP_ERR_NOT_FOUND;   /* this board's table has no storage partition */
    }
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = format,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MEET_BASE, MEET_PART_LABEL, &cfg, &s_wl);
    if (err == ESP_OK) {
        s_mounted = true;
    }
    return err;
}

/* The partition is formatted at most once in the device's life: an NVS
 * flag remembers it happened. A later mount failure is corruption to
 * report, never a silent reformat that would wipe every recording. */
static bool fmt_flag_done(void)
{
    nvs_handle_t h;
    if (nvs_open(MEET_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "fmt", &v);
    nvs_close(h);
    return err == ESP_OK && v == 1;
}

static void fmt_flag_set(void)
{
    nvs_handle_t h;
    if (nvs_open(MEET_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, "fmt", 1) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

/* Mount, formatting only if that has never happened. Caller holds s_lock. */
static esp_err_t mount_or_format_once(void)
{
    esp_err_t err = mount(false);
    if (err == ESP_OK || fmt_flag_done()) {
        return err;
    }
    err = mount(true);
    if (err == ESP_OK) {
        fmt_flag_set();
    }
    return err;
}

/*
 * Boot-time mount, called from muse_app_run (a normal internal-RAM stack).
 * This is the ONLY place a format may happen: the voice task's stack is
 * in PSRAM and flash writes during a format would crash it, so begin()
 * below mounts with format=false and simply fails if prewarm did not run.
 */
esp_err_t muse_meeting_mount(void)
{
    esp_err_t err = init_once();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = mount_or_format_once();
    xSemaphoreGive(s_lock);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "prewarm mount: %s", esp_err_to_name(err));
    }
    return err;
}

bool muse_meeting_mounted(void)
{
    return s_mounted;
}

/* The next free MEETnnnn.WAV name; recordings are only ever created in
 * sequence, so the first gap is the next index. */
static int next_index(void)
{
    char path[32];
    struct stat st;
    for (int i = 1; i < 10000; i++) {
        snprintf(path, sizeof(path), MEET_BASE "/MEET%04d.WAV", i);
        if (stat(path, &st) != 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t muse_meeting_begin(char *name_out, size_t name_cap, uint32_t *max_blocks)
{
    esp_err_t err = init_once();
    if (err != ESP_OK) {
        return err;
    }
    /* Never format here: this runs on the voice task (PSRAM stack). The
     * boot-time muse_meeting_mount() already formatted a fresh partition. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = mount(false);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "storage mount: %s", esp_err_to_name(err));
        return err;
    }
    int idx = next_index();
    if (idx < 0) {
        return ESP_ERR_NO_MEM;
    }
    struct statvfs sv;
    if (statvfs(MEET_BASE, &sv) != 0) {
        return ESP_FAIL;
    }
    uint64_t free_bytes = (uint64_t)sv.f_bfree * sv.f_frsize;
    if (free_bytes < MEET_RESERVE_BYTES + MEET_MIN_FREE_BLOCKS * MUSE_MEETING_BLOCK_BYTES) {
        return ESP_ERR_NO_MEM;   /* under a minute left: call it full */
    }
    if (max_blocks) {
        *max_blocks = (uint32_t)((free_bytes - MEET_RESERVE_BYTES) / MUSE_MEETING_BLOCK_BYTES);
    }
    meet_msg_t m = { .kind = MSG_BEGIN };
    snprintf(m.name, sizeof(m.name), "MEET%04d.WAV", idx);
    /* Drain a token a timed-out earlier call may have left behind, or this
     * take could complete against the wrong operation. The writer resets
     * s_op_err when it processes BEGIN; nothing else may. */
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }
    xQueueSend(s_wq, &m, portMAX_DELAY);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        muse_meeting_end(0);   /* resync the writer handshake, best effort */
        return ESP_ERR_TIMEOUT;
    }
    if (s_op_err != ESP_OK) {
        return s_op_err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_current, m.name, sizeof(s_current));
    s_recording = true;
    xSemaphoreGive(s_lock);
    if (name_out) {
        strlcpy(name_out, m.name, name_cap);
    }
    ESP_LOGI(TAG, "recording %s (room for %" PRIu32 " blocks)", m.name, max_blocks ? *max_blocks : 0);
    return ESP_OK;
}

bool muse_meeting_write(const uint8_t block[MUSE_MEETING_BLOCK_BYTES])
{
    meet_blk_t *blk = NULL;
    if (xQueueReceive(s_freeq, &blk, pdMS_TO_TICKS(250)) != pdTRUE || !blk) {
        return false;   /* writer fell behind: the caller ends the recording */
    }
    memcpy(blk->b, block, MUSE_MEETING_BLOCK_BYTES);
    meet_msg_t m = { .kind = MSG_DATA, .blk = blk };
    xQueueSend(s_wq, &m, portMAX_DELAY);
    return true;
}

esp_err_t muse_meeting_end(uint32_t total_samples)
{
    meet_msg_t m = { .kind = MSG_END, .total_samples = total_samples };
    /* Do NOT reset s_op_err here: the writer may already have recorded a
     * mid-recording write failure in it, and that failure is exactly what
     * this call must report. The writer resets it at the next BEGIN. */
    xQueueSend(s_wq, &m, portMAX_DELAY);
    esp_err_t err = xSemaphoreTake(s_done, pdMS_TO_TICKS(10000)) == pdTRUE ? s_op_err : ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_recording = false;
    s_current[0] = '\0';
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved %" PRIu32 " samples", total_samples);
    }
    return err;
}

void muse_meeting_console_list(void)
{
    if (init_once() != ESP_OK) {
        printf("@meet.error no storage: meeting storage failed to initialize\n");
        fflush(stdout);
        return;
    }
    if (!muse_meeting_mounted()) {
        /* The console never formats: muse_meeting_mount() at boot did. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = mount(false);
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            printf("@meet.error no storage: the meeting partition is not mounted\n");
            fflush(stdout);
            return;
        }
    }
    bool recording;
    char current[20];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    recording = s_recording;
    strlcpy(current, s_current, sizeof(current));
    xSemaphoreGive(s_lock);
    int count = 0;
    for (int i = 1; i < 10000; i++) {
        char path[32];
        struct stat st;
        snprintf(path, sizeof(path), MEET_BASE "/MEET%04d.WAV", i);
        if (stat(path, &st) != 0) {
            break;
        }
        uint32_t samples = 0;
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t word[4];
            if (fseek(f, HDR_OFF_FACT_SAMPLES, SEEK_SET) == 0 && fread(word, 1, 4, f) == 4) {
                samples = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16
                        | (uint32_t)word[3] << 24;
            }
            fclose(f);
        }
        printf("@meet.file {\"name\":\"MEET%04d.WAV\",\"bytes\":%ld,\"seconds\":%.1f%s}\n", i,
               (long)st.st_size, (double)samples / MUSE_MEETING_RATE,
               recording && !strcmp(path + 6, current) ? ",\"recording\":true" : "");
        count++;
    }
    printf("@meet.list_end {\"count\":%d}\n", count);
    fflush(stdout);
}

bool muse_meeting_console_dump(int index)
{
    if (index < 1 || index > 9999 || init_once() != ESP_OK) {
        return false;
    }
    if (!muse_meeting_mounted()) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = mount(false);
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            return false;
        }
    }
    char path[32];
    snprintf(path, sizeof(path), MEET_BASE "/MEET%04d.WAV", index);
    bool recording;
    char current[20];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    recording = s_recording;
    strlcpy(current, s_current, sizeof(current));
    xSemaphoreGive(s_lock);
    if (recording && !strcmp(path + 6, current)) {
        printf("@meet.error MEET%04d.WAV is being recorded\n", index);
        fflush(stdout);
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    struct stat st;
    stat(path, &st);
    printf("@meet.begin {\"name\":\"MEET%04d.WAV\",\"bytes\":%ld}\n", index, (long)st.st_size);
    uint8_t raw[384];
    char b64[516];
    size_t n;
    while ((n = fread(raw, 1, sizeof(raw), f)) > 0) {
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen, raw, n) != 0) {
            break;
        }
        b64[olen] = '\0';
        printf("@meet.data %s\n", b64);
        fflush(stdout);
    }
    fclose(f);
    printf("@meet.end\n");
    fflush(stdout);
    return true;
}
