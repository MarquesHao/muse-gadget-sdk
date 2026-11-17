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
 * The embedded headphones sprite: the accessory the frame face wears
 * for a pat reaction (see muse_frames.c), cut out of the board's own
 * "happy" pose render and split into pieces (headband, two ear
 * cups). Each piece's data is a ready-made LVGL I8 (indexed) image —
 * a 256-entry BGRA palette (1024 bytes, index 0 fully transparent)
 * followed by one index byte per pixel — so the player points an
 * image descriptor straight at the embedded bytes.
 *
 * The format is a budget decision, not a preference: boards here
 * copy .rodata into PSRAM at boot (CONFIG_SPIRAM_RODATA), so
 * embedded bytes are PSRAM bytes. RGB565A8 for these pieces would
 * charge ~200 KB to that scarce pool and starve the frame player's
 * preloads; the indexed form costs ~70 KB at native resolution
 * (per-piece median-cut palette in RGBA space, so the feathered
 * edge keeps its alpha steps).
 *
 * The artwork is not part of this repository: a board that ships it
 * generates muse_phones_sprite.c from its own render (see
 * tools/gen_phones_sprite.py) into the gitignored
 * components/muse/avatar/ directory, and CMake prefers that file
 * over the committed stub in muse_phones_sprite.c. The stub returns
 * no pieces: no embedded sprite, and pats keep the player's original
 * alternating reactions, exactly as before.
 *
 * x/y are the piece's seated origin in the 466x466 artwork space the
 * frame library uses (the player adds the face's screen placement).
 */
typedef struct {
    const uint8_t *data;
    size_t data_size;   /* 1024 palette bytes + w*h index bytes (I8) */
    uint16_t w, h;
    int16_t x, y;
} muse_phones_piece_t;

const muse_phones_piece_t *muse_phones_pieces(size_t *count_out);

#ifdef __cplusplus
}
#endif
