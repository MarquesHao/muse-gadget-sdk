# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#     http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The frame player's task placement (muse_frames.c, round 17).

Decoding a 466x466 JPEG frame takes ~100 ms of *wall* time against a
125 ms frame period, and the decode stats measure wall time, so the
core the player shares decides how much of that time is preemption.
These tests pin the placement the round chose -- the network core
(0), like the video decoder -- and the two things that must not
change along the way: the stack stays in PSRAM caps (internal RAM is
the VM lookup's scarce resource) and the decode stats line stays in
place as the device A/B instrument.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FRAMES = ROOT / "components" / "muse" / "muse_frames.c"


class FramesPlayerPlacementTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = FRAMES.read_text(encoding="utf-8")

    def test_pet_tap_is_a_flag_the_resolver_consumes_in_idle(self) -> None:
        # Night round (2026-11-16): a face tap must never reach into
        # the player mid-turn. It raises a flag here; only the IDLE
        # branch of resolve_target consumes it, alternating between
        # the level-up celebration and the happy still.
        self.assertIn("void muse_frames_pet_tap(void)", self.src)
        self.assertIn("s_pet_pending = true", self.src)
        idle = self.src.index("case MUSE_MODE_IDLE:")
        pet = self.src.index("if (s_pet_pending)", idle)
        levelup = self.src.index("if (s_levelup_pending)", idle)
        self.assertLess(pet, levelup)
        body = self.src[pet:levelup]
        self.assertIn("s_levelup_pending = true", body)
        self.assertIn("muse_state_make_happy()", body)

    def test_player_core_is_the_network_core(self) -> None:
        self.assertIn("#define FR_PLAYER_CORE 0", self.src)

    def test_player_task_is_pinned_to_player_core(self) -> None:
        create = re.search(
            r"xTaskCreatePinnedToCoreWithCaps\(player_task,.*?\)", self.src, re.S
        )
        self.assertIsNotNone(create)
        self.assertIn("FR_PLAYER_CORE", create.group(0))
        self.assertNotIn("MUSE_UI_CORE", create.group(0))

    def test_player_stack_stays_in_big_caps(self) -> None:
        create = re.search(
            r"xTaskCreatePinnedToCoreWithCaps\(player_task,.*?\)", self.src, re.S
        )
        self.assertIsNotNone(create)
        self.assertIn("MUSE_BIG_CAPS", create.group(0))
        self.assertNotIn("MALLOC_CAP_INTERNAL", create.group(0))

    def test_player_logs_its_core_for_the_ab_capture(self) -> None:
        self.assertIn('"player running on core %d", xPortGetCoreID()', self.src)

    def test_decode_stats_instrument_unchanged(self) -> None:
        # The A/B is read off this line: avg/max over 80 decodes plus
        # the skip count, fed by the wall-time measurement in
        # decode_jpeg_mem. Losing any of it loses the comparison.
        self.assertIn('"decode %s: avg %u ms, max %u ms (n=%u, skipped %u)"', self.src)
        self.assertIn("dec_stats_note((uint32_t)((esp_timer_get_time() - t0) / 1000))",
                      self.src)
        self.assertIn("#define FR_PERIOD_US 125000", self.src)


class FramesFeelRoundTest(unittest.TestCase):
    """The B-route "feel" round (2026-11-17): instant squash feedback,
    a shallow dip (not a crossfade) on interactive switches, and a
    compressed first-frame preload for the hottest states."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = FRAMES.read_text(encoding="utf-8")

    def test_squash_rides_the_image_scale_not_transform(self) -> None:
        # The squash must ride the image widget's own scale: a style
        # transform makes LVGL render into a layer buffer this board
        # cannot allocate (measured on device, 2026-11-17: dozens of
        # lv_draw_layer_alloc_buf failures around a press). The live
        # factor is shared with apply_framing so frame swaps re-apply
        # it instead of resetting the animation mid-flight.
        self.assertIn("void muse_frames_squash(void)", self.src)
        self.assertIn("lv_image_set_scale((lv_obj_t *)obj, v)", self.src)
        self.assertIn("lv_image_set_pivot(s_img, FR_W / 2, FR_H / 2)",
                      self.src)
        self.assertIn("lv_image_set_scale(s_img, s_squash_factor)",
                      self.src)
        self.assertNotIn("lv_obj_set_style_transform_scale_x(s_img",
                         self.src)
        self.assertIn("#define FR_SQUASH_MIN 246", self.src)
        squash = self.src.index("void muse_frames_squash(void)")
        body = self.src[squash:squash + 1600]
        self.assertIn("lv_anim_delete(s_img, squash_exec)", body)
        self.assertIn("LV_OBJ_FLAG_HIDDEN", body)

    def test_soft_dip_is_shallow_and_never_for_waking(self) -> None:
        self.assertIn("#define FR_SOFT_DIP_BOTTOM 160", self.src)
        # Single-step staircase (see the define's comment: finer
        # steps pay a full display flush each on this board).
        self.assertIn("#define FR_SOFT_DIP_STEP 95", self.src)
        pair = self.src.index("static bool soft_dip_pair")
        body = self.src[pair:pair + 900]
        # want==V_WAKING is refused outright, and the waking visual
        # is not among the outgoing faces that qualify either.
        self.assertIn("want == V_WAKING", body)
        self.assertNotIn("case V_WAKING:", body)
        self.assertIn("case V_IDLE:", body)
        self.assertIn("case V_LISTENING:", body)

    def test_soft_hold_is_consumed_by_show_front(self) -> None:
        show = self.src.index("static void show_front(void)")
        body = self.src[show:show + 1200]
        self.assertIn("s_soft_hold ? FR_SOFT_DIP_BOTTOM : LV_OPA_COVER", body)
        self.assertIn("s_soft_hold = false;", body)

    def test_preload_covers_kits_and_levelup_first_frame(self) -> None:
        # Only the three big first frames are warmed; the kits' small
        # sprites stay on the file path (PSRAM margin discipline —
        # see FR_PRELOAD_PATHS comment).
        for path in (
            "/meet/sprites/listening/blank.jpg",
            "/meet/sprites/thinking/blank.jpg",
            "/meet/frames/levelup/f000.jpg",
        ):
            self.assertIn(path, self.src)
        table = self.src.index("FR_PRELOAD_PATHS[]")
        body = self.src[table:table + 400]
        self.assertNotIn("spr_eye_L.png", body)
        self.assertNotIn("geom.json", body)
        # Listening is warmed before thinking: it heads the table.
        self.assertLess(self.src.index("/meet/sprites/listening/blank.jpg"),
                        self.src.index("/meet/sprites/thinking/blank.jpg"))

    def test_preload_feeds_kit_build_and_seq_enter(self) -> None:
        kit = self.src.index("static bool build_kit")
        self.assertIn("preload_get(path, &clen)", self.src[kit:kit + 4200])
        seq = self.src.index("static bool enter_seq_frame")
        self.assertIn("preload_get(path, &clen)", self.src[seq:seq + 900])
        # Warmed only from idle passes with the next frame ready and
        # a real margin before it is due (Codex review, 2026-11-17:
        # seq_ready alone does not prove the read fits the slack).
        self.assertIn("if (cur == V_IDLE && seq_ready && s_waking_done",
                      self.src)
        self.assertIn("esp_timer_get_time() + 30000 < deadline", self.src)

    def test_enter_latency_is_logged(self) -> None:
        self.assertIn('"enter %s: %lld ms (build %lld ms)"', self.src)


if __name__ == "__main__":
    unittest.main()
