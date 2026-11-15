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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Meeting mode storage (project V1.1, P2): recordings live as WAV files
 * (IMA-ADPCM, 16 kHz mono) on a FAT partition labelled "storage" — the
 * project partition table (partitions_muse_gadget.csv) gives it the flash
 * past the OTA slots. Files never leave the device on their own; the
 * serial console's meet.list / meet.dump=N commands are the way out.
 *
 * The voice task owns capture and must never write flash (its stack is in
 * PSRAM), so all file work runs on a writer task with an internal stack:
 * the caller fills fixed-size blocks and queues them.
 */

/* One WAV block: 4-byte IMA preamble + 508 bytes of nibbles. */
#define MUSE_MEETING_BLOCK_BYTES 512
#define MUSE_MEETING_BLOCK_SAMPLES 1017   /* the preamble sample + 1016 coded ones */
#define MUSE_MEETING_RATE 16000

/* Boot-time mount: call once from a task with an internal-RAM stack
 * (muse_app_run does). Formats the partition on first use in the
 * device's life (an NVS flag remembers; later corruption is reported,
 * never silently reformatted). Boards whose partition table has no
 * "storage" partition get ESP_ERR_NOT_FOUND. muse_meeting_begin() never
 * formats — it runs on the voice task, whose PSRAM stack cannot survive
 * flash writes. */
esp_err_t muse_meeting_mount(void);
bool muse_meeting_mounted(void);

/* Mount for read-only browsing (the frame player, muse_frames.c): the
 * mount lock and mount(false) only — no writer setup, and never a
 * format (the boot-time muse_meeting_mount() owns the one-time format;
 * if it has not run or the store is corrupt this simply fails). The
 * partition then stays mounted, exactly as after a begin/list/dump.
 * ESP_ERR_NOT_FOUND on boards without a storage partition. */
esp_err_t muse_meeting_storage_mount(void);

/* Starts a recording: picks the next MEETnnnn.WAV name (into name_out) and
 * reports how many blocks fit in the free space (max_blocks). The file is
 * open on the writer task once this returns ESP_OK. */
esp_err_t muse_meeting_begin(char *name_out, size_t name_cap, uint32_t *max_blocks);

/* Queues one full block for writing. False when the writer has fallen
 * behind (pool exhausted): the caller should end the recording. */
bool muse_meeting_write(const uint8_t block[MUSE_MEETING_BLOCK_BYTES]);

/* Finalizes the current file (patches the WAV header with total_samples,
 * closes it) and waits until that is done. */
esp_err_t muse_meeting_end(uint32_t total_samples);

/* Serial console helpers (muse_input.c): list the recordings, and dump
 * one of them (1-based index over the MEETnnnn sequence) as base64 lines
 * on stdout. Dump returns false when there is no such file, when the
 * index is outside 1..9999, when the file is the one being recorded, or
 * when the storage partition is not available. */
void muse_meeting_console_list(void);
bool muse_meeting_console_dump(int index);

#ifdef __cplusplus
}
#endif
