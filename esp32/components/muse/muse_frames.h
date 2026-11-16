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

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Frame-library face (project P8): plays the action library stored on
 * the meeting "storage" partition (JPEG frame sequences, sprite kits
 * and stills, all 466x466) instead of the procedural pixel face.
 *
 * muse_ui.c calls this once from build_screen(), right after the pixel
 * canvas is created, so the frame image sits above the canvas and below
 * the ring, icons and labels. A player task (started here) mounts the
 * storage partition read-only through muse_meeting, decodes frames into
 * PSRAM double buffers and swaps them in under the display lock.
 *
 * When the build links an embedded boot still (muse_boot_still.h),
 * attach decodes and shows it before returning, and it stays the
 * fallback for anything unavailable — no storage partition, a mount
 * the firmware refuses, a missing or undecodable asset. Only a build
 * with no still leaves the pixel face showing in those cases: the
 * image widget then simply stays hidden. Runs in the LVGL task
 * (build_screen), like every other widget creation.
 */
void muse_frames_attach(lv_obj_t *face);

/*
 * Whether the embedded boot still decoded and is up (see
 * muse_boot_still.h). muse_ui.c reads this right after
 * muse_frames_attach() to decide the pixel canvas's fate from the
 * same signal the player falls back on — the decode outcome, not
 * merely the still being linked — so a still that failed to decode
 * leaves the pixel face alive as the fallback instead of blanking it.
 */
bool muse_frames_boot_still_up(void);

#ifdef __cplusplus
}
#endif
