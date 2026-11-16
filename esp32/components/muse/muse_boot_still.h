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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The embedded boot/fallback still: one 466x466 JPEG of the current
 * face, linked into the firmware so the frame-library face
 * (muse_frames.c) has a face of its own to show before the storage
 * library mounts, and one to fall back on when a library asset is
 * missing or the storage itself is unusable. On minimal-face boards
 * muse_ui.c also blanks the pixel canvas when a still is linked, so
 * the old procedural face never shows through anywhere.
 *
 * The artwork is not part of this repository: a board that ships one
 * generates muse_boot_still.c from its own JPEG (see
 * tools/gen_boot_still.py) into the gitignored
 * components/muse/avatar/ directory, and CMake prefers that file
 * over the committed stub in muse_boot_still.c. The stub returns
 * NULL: no embedded still, and the pixel face stays the fallback,
 * exactly as before.
 *
 * Returns the JPEG bytes (and their length, when len_out is not
 * NULL), or NULL when this build has no embedded still.
 */
const uint8_t *muse_boot_still_jpg(size_t *len_out);

#ifdef __cplusplus
}
#endif
