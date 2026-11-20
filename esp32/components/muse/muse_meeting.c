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
#include <unistd.h>

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
/* Writer task stack size. The stack itself is static (s_writer_stack
 * below); the size comes from the writer's measured stack high-water
 * mark, logged after every recording: four hardware recordings at
 * 4096 B (three 65 s, one 300 s, 2026-11-20) never used more than
 * 2036 B, so 3072 B keeps 1036 B of headroom — half the worst
 * observed use again — and hands the other kilobyte back to the
 * scarcest resource on this board. If the logged mark ever climbs
 * past ~2500 B used, raise this again. */
#define MEET_WRITER_STACK_BYTES 3072

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

enum { MSG_BEGIN, MSG_DATA, MSG_END, MSG_SPACE };
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
static atomic_int s_init_state;    /* 0 new/retryable, 1 initializing, 2 done (setup succeeded) */
static atomic_int s_lock_state;    /* same protocol, for s_lock alone */
static esp_err_t s_init_err = ESP_OK;
static volatile esp_err_t s_op_err = ESP_OK;
static volatile uint64_t s_free_bytes;   /* MSG_SPACE result: writer fills, begin() reads after s_done */
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

static uint32_t get_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16
           | (uint32_t)at[3] << 24;
}

static void writer_task(void *arg)
{
    (void)arg;
    meet_msg_t m;
    for (;;) {
        xQueueReceive(s_wq, &m, portMAX_DELAY);
        if (m.kind == MSG_BEGIN) {
            if (s_fp) {
                /* A previous session never ended (its END never reached
                 * the writer). Its file still wears the placeholder
                 * header — every size field zero — but holds however many
                 * whole blocks the writer counted into s_data_bytes, and
                 * those blocks are real audio: every MSG_DATA block is a
                 * full 1017-sample block. (The one padded tail block a
                 * session can produce is written immediately before its
                 * END, and an END queued ahead of this BEGIN would have
                 * finalized the file instead of leaving it to be topped.)
                 * Salvage it: patch the three header fields from the
                 * counted bytes, exactly as MSG_END does, so the recording
                 * stays listed with its real length and stays playable.
                 * A topped file with no blocks holds no audio and is
                 * removed, like the header-failure and zero-sample cases.
                 * A salvage failure is logged only: it must not poison
                 * s_op_err, which belongs to the session this BEGIN is
                 * starting. */
                bool salvaged = false;
                if (s_data_bytes > 0) {
                    uint8_t word[4];
                    uint32_t samples = (s_data_bytes / MUSE_MEETING_BLOCK_BYTES)
                                       * MUSE_MEETING_BLOCK_SAMPLES;
                    put_u32(word, MEET_HDR_BYTES - 8 + s_data_bytes);
                    salvaged = fseek(s_fp, HDR_OFF_RIFF_SIZE, SEEK_SET) == 0
                               && fwrite(word, 1, 4, s_fp) == 4;
                    put_u32(word, samples);
                    salvaged = salvaged && fseek(s_fp, HDR_OFF_FACT_SAMPLES, SEEK_SET) == 0
                               && fwrite(word, 1, 4, s_fp) == 4;
                    put_u32(word, s_data_bytes);
                    salvaged = salvaged && fseek(s_fp, HDR_OFF_DATA_SIZE, SEEK_SET) == 0
                               && fwrite(word, 1, 4, s_fp) == 4;
                    if (salvaged) {
                        fflush(s_fp);
                    }
                }
                fclose(s_fp);
                s_fp = NULL;
                if (s_data_bytes == 0) {
                    remove(s_fp_path);   /* no audio in it: leave no stub behind */
                } else if (salvaged) {
                    ESP_LOGW(TAG, "previous session never ended: salvaged %s "
                                  "(%" PRIu32 " bytes of audio)",
                             s_fp_path, s_data_bytes);
                } else {
                    ESP_LOGW(TAG, "previous session never ended: %s header "
                                  "salvage failed, sizes stay zero",
                             s_fp_path);
                }
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
                /* The file holds at most a partial header and no audio:
                 * remove it, or it would squat on this MEETnnnn name
                 * forever (next_index() only reuses gaps) and show up
                 * in the list as a zero-second recording. */
                remove(s_fp_path);
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
        } else if (m.kind == MSG_SPACE) {
            /* Free-space query for begin(), run here on purpose:
             * f_getfree() can flush a dirty FAT window — a flash
             * write — and flash writes belong on this task (internal
             * stack), never on the voice task's PSRAM stack. */
            uint64_t total = 0, freeb = 0;
            s_op_err = esp_vfs_fat_info(MEET_BASE, &total, &freeb);
            s_free_bytes = freeb;
            xSemaphoreGive(s_done);
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
            /* One line per recording: the writer stack's high-water
             * mark is what MEET_WRITER_STACK_BYTES is sized from, so
             * if a future change starts eating the stack, the
             * shrinking margin shows here while there is still
             * headroom. (The mark is the lifetime minimum of unused
             * stack, in words; the log prints bytes.) */
            ESP_LOGI(TAG, "writer stack high-water: %" PRIu32 " of %d B never used",
                     (uint32_t)uxTaskGetStackHighWaterMark(NULL) * (uint32_t)sizeof(StackType_t),
                     MEET_WRITER_STACK_BYTES);
            xSemaphoreGive(s_done);
        }
    }
}

/* The mount lock on its own, creatable without setup(): the boot-time
 * prewarm mount takes s_lock but must not drag in setup() — the writer
 * task's internal-RAM stack and the queues are memory the audio pipeline
 * needs first at boot (a starved I2S DMA allocation crashed voice init
 * on the 1.75C). First real use (begin/list/dump) runs full setup(). */
static esp_err_t lock_init_once(void)
{
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_lock_state, &expected, 1)) {
            if (!s_lock) {
                s_lock = xSemaphoreCreateMutex();
            }
            if (s_lock) {
                atomic_store(&s_lock_state, 2);
                return ESP_OK;
            }
            /* Creation failed (boot-time memory pressure): re-arm so the
             * next caller retries instead of caching the failure for the
             * whole boot. Each caller attempts creation at most once. */
            atomic_store(&s_lock_state, 0);
            return ESP_ERR_NO_MEM;
        }
        if (atomic_load(&s_lock_state) == 2) {
            return ESP_OK;   /* state 2 is only stored once s_lock exists */
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* The writer task's TCB and stack are static (plain .bss = internal
 * DRAM, contiguous by construction). See the creation site in setup()
 * for the full reasoning: a runtime heap block of this size is exactly
 * what the settled heap cannot provide. Plain statics (no section
 * attribute) also keep the stack out of PSRAM, which the flash writes
 * the writer performs would not survive. */
static StaticTask_t s_writer_tcb;
static StackType_t s_writer_stack[MEET_WRITER_STACK_BYTES / sizeof(StackType_t)];

/* Undo a partial setup() so a later retry starts from nothing. Only
 * setup() calls this, before s_setup is set, so no other task can be
 * holding the queues yet. */
static void setup_undo(meet_blk_t *pool)
{
    if (s_wq) {
        vQueueDelete(s_wq);
        s_wq = NULL;
    }
    if (s_freeq) {
        vQueueDelete(s_freeq);
        s_freeq = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    heap_caps_free(pool);
}

static esp_err_t setup(void)
{
    if (s_setup) {
        return ESP_OK;
    }
    if (lock_init_once() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    meet_blk_t *pool = heap_caps_malloc(MEET_POOL_BLOCKS * sizeof(meet_blk_t), MALLOC_CAP_SPIRAM);
    s_wq = xQueueCreate(MEET_POOL_BLOCKS + 4, sizeof(meet_msg_t));
    s_freeq = xQueueCreate(MEET_POOL_BLOCKS, sizeof(meet_blk_t *));
    s_done = xSemaphoreCreateBinary();
    if (!pool || !s_wq || !s_freeq || !s_done) {
        setup_undo(pool);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < MEET_POOL_BLOCKS; i++) {
        meet_blk_t *blk = &pool[i];
        xQueueSend(s_freeq, &blk, 0);
    }
    /* Static creation (s_writer_stack, plain .bss): creating the
     * writer asks the heap for no stack block at all, so first use
     * succeeds in the settled state too. The dynamic version's 4 KB
     * stack was the one allocation meeting setup could not satisfy
     * once the VM session was up and the frame player was running:
     * the largest free internal block sits below 4 KB there, so
     * creation returned pdFAIL on every attempt (13/13 measured on
     * hardware, 2026-11-19; setup() reports it as ESP_ERR_NO_MEM) and
     * meeting mode could never start recording. The stack stays
     * internal DRAM, as it must: the writer is the task that writes
     * flash (FAT + wear levelling), and a PSRAM stack does not
     * survive the cache-off windows those writes cause. The price is
     * .bss holding stack + TCB for the whole boot — but once a
     * setup() has succeeded it is never repeated, and the writer
     * then lives until reboot, so a heap stack was never actually
     * transient either. */
    if (xTaskCreateStatic(writer_task, "muse_meet_wr",
                          MEET_WRITER_STACK_BYTES / sizeof(StackType_t),
                          NULL, 5, s_writer_stack, &s_writer_tcb) == NULL) {
        setup_undo(pool);
        return ESP_ERR_NO_MEM;
    }
    s_setup = true;
    return ESP_OK;
}

/* setup() to success, whichever task (voice, console, boot) gets here
 * first; concurrent callers wait for the winner instead of
 * double-allocating. A failed attempt does NOT latch: it usually means
 * the caller raced the audio pipeline for internal RAM during boot, so
 * the state re-arms and the next begin/list/dump retries setup() once
 * memory has settled. Each caller runs setup() at most once. */
static esp_err_t init_once(void)
{
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_init_state, &expected, 1)) {
            esp_err_t err = setup();
            s_init_err = err;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "setup failed: %s (next use retries)", esp_err_to_name(err));
            }
            atomic_store(&s_init_state, err == ESP_OK ? 2 : 0);
            return err;
        }
        if (atomic_load(&s_init_state) == 2) {
            return s_init_err;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
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
    if (err == ESP_OK) {
        /* A partition that mounts is a formatted partition. If the flag
         * is missing anyway — power died between the one-time format and
         * its NVS commit — record the fact now: with the flag lost AND a
         * later corruption, the branch below would mistake this for a
         * fresh partition and reformat over every recording. */
        if (!fmt_flag_done()) {
            fmt_flag_set();
        }
        return ESP_OK;
    }
    if (fmt_flag_done()) {
        return err;
    }
    err = mount(true);
    if (err == ESP_OK) {
        fmt_flag_set();
    }
    return err;
}

/*
 * Power-loss salvage. A recording interrupted by a power cut or a reset
 * never reaches MSG_END and the writer is gone with the boot, so nobody
 * tops it: its file keeps the placeholder header (all three size fields
 * zero) over however many whole blocks reached the disk (the writer
 * flushes every ~32 KB). Such a file lists as 0 seconds and exports as
 * a stub. The disposition is the topped-session one from the writer:
 * patch the three header fields from the whole blocks actually on disk,
 * and remove the file when it holds no whole block at all.
 *
 * The scan runs only from muse_meeting_mount(): the one mount point
 * that is guaranteed an internal-RAM stack (flash writes from the voice
 * task's PSRAM stack would crash it) and where no session can have
 * finalized anything yet this boot. The one file a session may already
 * have open — a meeting-mode device can auto-start a recording before
 * the prewarm runs — wears a placeholder header legitimately and is
 * skipped by name; every other file with a zero size field is dead.
 */
static void salvage_one(const char *path, long size)
{
    if (size < MEET_HDR_BYTES) {
        /* Not even a full header landed: no whole block can follow it,
         * so there is no audio to save, and the stub would squat on
         * this MEETnnnn name forever. */
        remove(path);
        ESP_LOGW(TAG, "removed stub %s (%ld bytes, no complete header)", path, size);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    uint8_t hdr[MEET_HDR_BYTES];
    bool read_ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    fclose(f);
    if (!read_ok) {
        return;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0
        || memcmp(hdr + 12, "fmt ", 4) != 0 || memcmp(hdr + 40, "fact", 4) != 0
        || memcmp(hdr + 52, "data", 4) != 0) {
        return;   /* not a meeting WAV layout we understand: leave it alone */
    }
    if (get_u32(hdr + HDR_OFF_RIFF_SIZE) != 0 && get_u32(hdr + HDR_OFF_FACT_SAMPLES) != 0
        && get_u32(hdr + HDR_OFF_DATA_SIZE) != 0) {
        return;   /* finalized (a padded tail block keeps fact below the
                   * whole-block count, but never at zero): not ours */
    }
    uint32_t audio = (uint32_t)size - MEET_HDR_BYTES;
    uint32_t whole = audio / MUSE_MEETING_BLOCK_BYTES * MUSE_MEETING_BLOCK_BYTES;
    if (whole == 0) {
        remove(path);   /* header but no whole block: no audio, no stub */
        ESP_LOGW(TAG, "removed stub %s (header only, no audio)", path);
        return;
    }
    f = fopen(path, "r+b");
    if (!f) {
        ESP_LOGW(TAG, "salvage %s: cannot reopen for patching", path);
        return;
    }
    /* Truncate a partial tail block BEFORE patching the header: if
     * power dies again mid-salvage, the file still wears a zero header
     * at a whole-block size and the next boot simply redoes the job.
     * Patching first would leave sizes that disagree with the length. */
    if (audio > whole && ftruncate(fileno(f), (off_t)(MEET_HDR_BYTES + whole)) != 0) {
        ESP_LOGW(TAG, "salvage %s: tail truncate failed, left unpatched", path);
        fclose(f);
        return;
    }
    uint8_t word[4];
    put_u32(word, MEET_HDR_BYTES - 8 + whole);
    bool ok = fseek(f, HDR_OFF_RIFF_SIZE, SEEK_SET) == 0 && fwrite(word, 1, 4, f) == 4;
    put_u32(word, whole / MUSE_MEETING_BLOCK_BYTES * MUSE_MEETING_BLOCK_SAMPLES);
    ok = ok && fseek(f, HDR_OFF_FACT_SAMPLES, SEEK_SET) == 0 && fwrite(word, 1, 4, f) == 4;
    put_u32(word, whole);
    ok = ok && fseek(f, HDR_OFF_DATA_SIZE, SEEK_SET) == 0 && fwrite(word, 1, 4, f) == 4;
    if (ok) {
        fflush(f);
    }
    fclose(f);
    if (ok) {
        ESP_LOGW(TAG, "salvaged %s (%" PRIu32 " bytes of audio, session never ended)",
                 path, whole);
    } else {
        ESP_LOGW(TAG, "salvage %s: header patch failed, sizes stay zero", path);
    }
}

/* Caller holds s_lock with the partition mounted (muse_meeting_mount).
 * Recordings are created in sequence, so the first missing index ends
 * the scan, exactly as in the console list. */
static void salvage_orphans(void)
{
    /* Snapshot the live recording under the caller's lock — begin()
     * arms these under the same lock — and skip its file: its zero
     * header is legitimate while the writer has it open. Appending to
     * that one file is the only meeting write that can be in flight. */
    bool recording = s_recording;
    char current[sizeof(s_current)];
    strlcpy(current, s_current, sizeof(current));
    char path[32];
    struct stat st;
    for (int i = 1; i < 10000; i++) {
        snprintf(path, sizeof(path), MEET_BASE "/MEET%04d.WAV", i);
        if (stat(path, &st) != 0) {
            break;
        }
        if (recording && strcmp(path + 6, current) == 0) {
            continue;
        }
        salvage_one(path, (long)st.st_size);
    }
}

/*
 * Boot-time mount, called from muse_app_run (a normal internal-RAM stack).
 * This is the ONLY place a format may happen: the voice task's stack is
 * in PSRAM and flash writes during a format would crash it, so begin()
 * below mounts with format=false and simply fails if prewarm did not run.
 */
esp_err_t muse_meeting_mount(void)
{
    /* Lock only, NOT init_once(): the prewarm must not create the writer
     * task or the queues — see lock_init_once. */
    esp_err_t err = lock_init_once();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = mount_or_format_once();
    if (err == ESP_OK) {
        /* Rescue any recording a power cut left with a placeholder
         * header (see salvage_one) while this internal-stack task is
         * the one holding the mount; after the unmount below, no later
         * mount point can promise both that and a not-yet-recording
         * store. */
        salvage_orphans();
        /* The prewarm's jobs were the one-time format and proving the
         * store works; nothing needs it mounted afterwards, and every
         * real use (begin/list/dump) mounts on demand. Unmount again
         * right away: a mounted wear-levelling layer holds several KB
         * of internal RAM for the device's whole life, and on the 1.75C
         * that leaves the largest free block too small for the chat
         * session's VM-lookup task — voice then fails with CAN'T REACH
         * MUSE while Link is up (measured on hardware, 2026-11-15). */
        esp_err_t uerr = esp_vfs_fat_spiflash_unmount_rw_wl(MEET_BASE, s_wl);
        if (uerr == ESP_OK) {
            s_wl = WL_INVALID_HANDLE;
            s_mounted = false;
        } else {
            ESP_LOGW(TAG, "prewarm unmount: %s", esp_err_to_name(uerr));
        }
    }
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

/* Read-only mount for the frame player: same lock + mount discipline as
 * the console paths, but without init_once() — browsing assets must not
 * create the writer task and its queues. Never formats. */
esp_err_t muse_meeting_storage_mount(void)
{
    esp_err_t err = lock_init_once();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = mount(false);
    xSemaphoreGive(s_lock);
    return err;
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
    /* Free space comes from the FAT driver's own API, not POSIX
     * statvfs(): in ESP-IDF that function is an ENOSYS stub (esp_libc
     * syscalls.c, "TODO IDF-9879"), so it failed on every call and no
     * recording could ever start. The writer-task allocation failure
     * masked this until 2026-11-20 — setup() always failed first, so
     * begin() never got far enough to reach the check.
     * The query itself runs on the writer (MSG_SPACE): f_getfree()
     * can flush a dirty FAT window, which is a flash write, and flash
     * writes must not run on this task's PSRAM stack. A timed-out
     * query leaves a late s_done token and a stale result behind;
     * both are harmless — the drain before each send clears the
     * token, and every query overwrites the result before it is read. */
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }
    meet_msg_t q = { .kind = MSG_SPACE };
    xQueueSend(s_wq, &q, portMAX_DELAY);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_op_err != ESP_OK) {
        return s_op_err;
    }
    uint64_t free_bytes = s_free_bytes;
    if (free_bytes < MEET_RESERVE_BYTES + MEET_MIN_FREE_BLOCKS * MUSE_MEETING_BLOCK_BYTES) {
        return ESP_ERR_NO_MEM;   /* under a minute left: call it full */
    }
    if (max_blocks) {
        *max_blocks = (uint32_t)((free_bytes - MEET_RESERVE_BYTES) / MUSE_MEETING_BLOCK_BYTES);
    }
    meet_msg_t m = { .kind = MSG_BEGIN };
    snprintf(m.name, sizeof(m.name), "MEET%04d.WAV", idx);
    /* Arm the recording guard BEFORE the writer creates the file: the
     * console dump refuses s_current while s_recording is set, and that
     * refusal must already hold in the window between the writer opening
     * the file and this call returning — otherwise a dump in that window
     * exports the placeholder header as an empty WAV. Cleared again on
     * the failure return below (the timeout path clears it in end()). */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_current, m.name, sizeof(s_current));
    s_recording = true;
    xSemaphoreGive(s_lock);
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
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_recording = false;
        s_current[0] = '\0';
        xSemaphoreGive(s_lock);
        return s_op_err;
    }
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
