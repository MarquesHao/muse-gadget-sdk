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

"""The quick settings tile and the caption dialog box (muse_ui.c).

Hugh's two asks (2026-11-16): tapping the face must not pop a card up
any more -- the mode/volume controls the card carried moved to a tile
reached by swiping right from the face, while swiping left still goes
to settings -- and the caption gets a game-style dialog box in the
bottom quarter of the minimal-face board's screen instead of floating
over the face art, shown only while a caption is up.

His follow-up the same evening: the reply body joins the caption in
that box -- read-aloud replies were still on the answer layouts,
floating over the face where he couldn't read them. On the
minimal-face board both answer layouts take the box's page geometry
and the box stays up for whichever text is showing.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "components" / "muse" / "muse_ui.c"


def body_of(src: str, signature: str) -> str:
    """The brace-matched body of the function whose definition starts
    with `signature`."""
    start = src.index(signature)
    open_at = src.index("{", start)
    depth = 0
    for i in range(open_at, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_at:i + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


class QuickSettingsTileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = UI.read_text(encoding="utf-8")

    def test_tap_face_only_makes_happy(self) -> None:
        body = body_of(self.src, "static void on_canvas_clicked")
        self.assertIn("muse_state_make_happy()", body)
        self.assertNotIn("quick_", body)   # no panel call left in the tap path
        self.assertNotIn("lv_", body)      # and no UI object is touched at all

    def test_popup_panel_is_gone(self) -> None:
        for gone in ("s_quick_catch", "quick_show(", "quick_hide(",
                     "on_quick_catch", "s_quick_open"):
            self.assertNotIn(gone, self.src)

    def test_tiles_face_in_the_middle_settings_unchanged_side(self) -> None:
        quick = self.src.index("lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT)")
        face = self.src.index("lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT)")
        settings = self.src.index("lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_LEFT)")
        self.assertLess(quick, face)
        self.assertLess(face, settings)
        # The screen starts on the face, not on the first tile added.
        self.assertIn("lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF)", self.src)

    def test_tile_carries_the_mode_switch(self) -> None:
        build = body_of(self.src, "static void build_quick_tile")
        self.assertIn("on_quickset_mode", build)
        self.assertIn("quick_mode_name(i)", build)
        handler = body_of(self.src, "static void on_quickset_mode")
        self.assertIn("muse_settings_set_gadget_mode(mode)", handler)
        # Meeting starts recording at once: back to the face that shows it.
        self.assertIn("lv_tileview_set_tile(s_tv, s_face, LV_ANIM_ON)", handler)

    def test_tile_carries_the_volume_slider(self) -> None:
        build = body_of(self.src, "static void build_quick_tile")
        self.assertIn("lv_slider_create", build)
        self.assertIn("on_quickset_volume_done", build)
        done = body_of(self.src, "static void on_quickset_volume_done")
        self.assertIn("muse_settings_set_volume(v)", done)
        # The tile refreshes from the chrome tick, like the settings pages.
        self.assertIn("quickset_refresh();", body_of(self.src, "static void update_chrome"))

    def test_page_dots_cover_three_tiles(self) -> None:
        self.assertIn("static lv_obj_t *s_dots[3];", self.src)
        self.assertIn("for (int i = 0; i < 3; i++)", self.src)

    def test_settled_face_offset_goes_through_the_helper(self) -> None:
        # The face is the middle tile now: "settled on the face" is one
        # tile width of scroll, not zero, everywhere it is checked.
        self.assertIn("lv_obj_get_scroll_x(s_tv) != face_scroll_x()", self.src)
        self.assertIn("lv_obj_get_scroll_x(s_tv) == face_scroll_x()", self.src)
        self.assertIn("lv_obj_get_scroll_x(s_tv) > face_scroll_x()", self.src)


class CaptionDialogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = UI.read_text(encoding="utf-8")

    def test_box_is_minimal_face_only_and_starts_hidden(self) -> None:
        build = body_of(self.src, "static void build_screen")
        at = build.index("s_dialog = lv_obj_create(face)")
        self.assertIn("if (muse_board->minimal_face)", build[max(0, at - 1600):at])
        self.assertIn("lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_HIDDEN)", build)
        # Bottom quarter of the screen, and it must not eat touches.
        self.assertIn("lv_obj_align(s_dialog, LV_ALIGN_BOTTOM_MID, 0, -8)", build)
        self.assertIn("lv_obj_remove_flag(s_dialog, LV_OBJ_FLAG_CLICKABLE)", build)

    def test_caption_moves_into_the_box(self) -> None:
        self.assertIn("lv_obj_set_parent(s_caption_lbl, s_dialog)", self.src)

    def test_box_lives_and_dies_with_the_showing_text(self) -> None:
        # The box frames the caption while listening and the reply body
        # while an answer is up: it is visible whenever the showing
        # label has text, in either phase.
        self.assertIn(
            "lv_obj_set_flag(s_dialog, LV_OBJ_FLAG_HIDDEN, !shown[0])",
            self.src)
        self.assertNotIn("answer >= 0 || !shown[0]", self.src)


class ReplyInDialogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = UI.read_text(encoding="utf-8")

    def test_both_answer_layouts_become_the_box_page(self) -> None:
        build = body_of(self.src, "static void build_answer")
        at = build.index("a->lines = 2;")
        gate = build.rindex("if (muse_board->minimal_face)", 0, at)
        self.assertGreater(at, gate)   # the override is minimal-face only
        # Both layouts (heard and read) are overridden, in one loop.
        self.assertIn("for (int i = 0; i < 2; i++)", build)
        self.assertIn("answer_layout_t *a = &s_answers[i];", build)
        self.assertIn("a->scale = 1;", build)
        self.assertIn("a->w = 360;", build)   # the 384 box less its insets
        self.assertIn("a->h = 2 * box_pitch - CAPTION_LINE_SPACE;", build)

    def test_box_page_rides_where_the_caption_rides(self) -> None:
        build = body_of(self.src, "static void build_answer")
        # The box's top (108 tall, bottom-aligned, lifted 8) plus the
        # caption's 12 px inset, from the centre: the same band the
        # caption label occupies inside the box.
        self.assertIn("a->top = s_h - 8 - 108 + 12 - s_h / 2;", build)

    def test_pager_still_follows_the_layout(self) -> None:
        # No second page size anywhere: the UI tick keeps sizing the
        # pager from the layout, so the box geometry above is what the
        # voice task pages the reply into.
        tick = body_of(self.src, "static void update_status")
        self.assertIn("muse_chat_set_caption_px(s_answers[layout].w, reply_glyph_px)", tick)
        self.assertIn("s_answers[layout].lines - 1,\n                                s_answers[layout].lines)",
                      tick)


if __name__ == "__main__":
    unittest.main()
