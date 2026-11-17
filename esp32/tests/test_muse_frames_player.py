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
    """The B-route "feel" round (2026-11-17): instant squash feedback
    on presses and a compressed first-frame preload for the hottest
    states. (The round's shallow dip on interactive switches was
    removed again later the same day — see FramesDipRemovalTest.)"""

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

    def test_preload_covers_kits_and_levelup_first_frame(self) -> None:
        # Only the three big first frames are warmed (PSRAM margin
        # discipline — see the FR_PRELOAD_PATHS comment). Since the
        # smooth round the two kit faces are baked poses stills, so
        # the table warms those single files instead of kit blanks.
        for path in (
            "/meet/poses/listening.jpg",
            "/meet/poses/thinking.jpg",
            "/meet/frames/levelup/f000.jpg",
        ):
            self.assertIn(path, self.src)
        table = self.src.index("FR_PRELOAD_PATHS[]")
        body = self.src[table:table + 400]
        self.assertNotIn("sprites/", body)
        # Listening is warmed before thinking: it heads the table.
        self.assertLess(self.src.index("/meet/poses/listening.jpg"),
                        self.src.index("/meet/poses/thinking.jpg"))

    def test_preload_feeds_still_decode_and_seq_enter(self) -> None:
        still = self.src.index("static bool decode_still")
        self.assertIn("preload_get(path, &clen)", self.src[still:still + 900])
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


class FramesDipRemovalTest(unittest.TestCase):
    """The interactive soft dip is gone (2026-11-17, Hugh's "darkens
    first" complaint; Codex consultation on file in the project
    records): interactive enters swap whole at full opa. It shipped
    in the headphones round; the headphones overlay itself was
    removed whole later the same day on Hugh's call — see
    FramesSmoothRoundTest. The waking dip-to-black is Hugh-approved
    and stays exactly as it was."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = FRAMES.read_text(encoding="utf-8")

    def test_no_soft_dip_anywhere(self) -> None:
        self.assertNotIn("FR_SOFT_DIP", self.src)
        self.assertNotIn("soft_dip", self.src)
        self.assertNotIn("s_soft_hold", self.src)
        # show_front's only opa choice left: the waking dip's black,
        # or full. No intermediate hold.
        show = self.src.index("static void show_front(void)")
        body = self.src[show:show + 400]
        self.assertIn("int opa = s_dip ? 0 : LV_OPA_COVER;", body)

    def test_levelup_exit_no_longer_fades(self) -> None:
        self.assertNotIn("static void fade_out(void)", self.src)
        done = self.src.index("if (cur == V_LEVELUP) {")
        body = self.src[done:done + 400]
        self.assertNotIn("fade_out();", body)
        self.assertIn("s_levelup_pending = false;", body)

    def test_waking_dip_untouched(self) -> None:
        self.assertIn("static void dip_fade_out(void)", self.src)
        self.assertIn("static void dip_fade_in(void)", self.src)
        self.assertIn("dip_fade_out();   /* dip through black into idle */",
                      self.src)
        # The ramp itself is part of the approved feel: full range,
        # 32-opa steps, 30 ms a step, and the fade-in is consumed
        # from the enter path when s_dip is set.
        self.assertIn("for (int opa = LV_OPA_COVER; opa > 0; opa -= 32)",
                      self.src)
        self.assertIn("for (int opa = 0; opa < LV_OPA_COVER; opa += 32)",
                      self.src)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(30))", self.src)
        self.assertIn("dip_fade_in();", self.src)

class FramesSmoothRoundTest(unittest.TestCase):
    """The smooth round (2026-11-17): the four kit faces are baked
    offline into single poses stills (no runtime compositing), the
    player speculatively pre-builds thinking while listening sits
    static, and the headphones overlay is removed whole on Hugh's
    call (he saw it on the device and did not like it)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = FRAMES.read_text(encoding="utf-8")

    def test_kit_faces_are_baked_stills_not_runtime_sprites(self) -> None:
        self.assertNotIn("build_kit", self.src)
        self.assertNotIn("png.h", self.src)
        self.assertNotIn("/meet/sprites/", self.src)
        still = self.src.index("static bool decode_still")
        body = self.src[still:still + 900]
        self.assertIn('"/meet/poses/%s.jpg"', body)
        self.assertIn("decode_jpeg_file(path, out)", body)

    def test_speculative_thinking_prebuild_epoch_rules(self) -> None:
        # Built only while listening sits static, into the back
        # buffer, by the player itself (sole decoder, no display
        # lock held while decoding).
        self.assertIn(
            "if (cur == V_LISTENING && !s_spec_ready && !s_broken[V_THINKING])",
            self.src)
        self.assertIn("decode_still(V_THINKING, back_buffer())", self.src)
        self.assertIn('"spec thinking: built in %lld ms"', self.src)
        # Consumed only at the recorded epoch; anything else spends
        # it and says so.
        self.assertIn("s_spec_epoch == s_epoch", self.src)
        self.assertIn('"spec thinking: hit"', self.src)
        self.assertIn('"spec thinking: wasted"', self.src)

    def test_headphones_overlay_removed(self) -> None:
        for gone in (
            "phones_tap", "phones_force_off", "muse_phones_pieces",
            "FR_SWAY", "FR_PHONES_TRAVEL", "phones_apply_locked",
        ):
            self.assertNotIn(gone, self.src)
        # The pat is back to the plain pending flag.
        tap = self.src.index("void muse_frames_pet_tap(void)")
        self.assertIn("s_pet_pending = true;", self.src[tap:tap + 300])


if __name__ == "__main__":
    unittest.main()
