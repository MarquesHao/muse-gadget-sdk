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

"""The settings pages' iOS grouped-list restyle (muse_settings_ui.c).

Hugh's ask (2026-11-18, after approving a mock): copy Apple's Settings
layout — one rounded container per group with hairline dividers inset
to the label, a coloured icon tile on navigation rows, iOS pill
switches (grey off, blue on, white knob), iOS sliders (thin track,
light fill, white knob), blue checkmarks on choice rows, a capsule
behind the back chevron, and group headers above their containers.
Function must not move: same rows, same order, same callbacks.
"""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / "components" / "muse" / "muse_settings_ui.c"


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


class GroupedContainerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SETTINGS.read_text(encoding="utf-8")

    def test_group_builder_is_one_rounded_clipped_surface(self) -> None:
        body = body_of(self.src, "static lv_obj_t *group(")
        self.assertIn("lv_obj_set_style_radius(g, s_tiny ? 10 : 14, 0)", body)
        self.assertIn("lv_obj_set_style_clip_corner(g, true, 0)", body)
        self.assertIn("COLOR_CARD", body)
        self.assertIn("LV_FLEX_FLOW_COLUMN", body)

    def test_dividers_are_hairlines_inset_past_the_icon(self) -> None:
        body = body_of(self.src, "static void group_sep(")
        self.assertIn("lv_obj_set_size(sep, lv_pct(100), 1)", body)
        self.assertIn("COLOR_SEP", body)
        self.assertIn("icon ? 60 : 16", body)
        # The first row of a group gets no divider.
        self.assertIn("group_rows(g) < 1", body)

    def test_grouped_rows_are_flat_but_epaper_rows_stay_filled(self) -> None:
        body = body_of(self.src, "static lv_obj_t *card(")
        self.assertIn("is_group(list) && !muse_board->epaper", body)
        self.assertIn("LV_STATE_PRESSED", body)

    def test_rebuilt_wifi_groups_reset_their_divider_count(self) -> None:
        self.assertIn("group_reset(s_wifi_saved)", self.src)
        self.assertIn("group_reset(s_wifi_list)", self.src)


class RowStyleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SETTINGS.read_text(encoding="utf-8")

    def test_icon_tiles_are_small_white_glyphs_on_colour(self) -> None:
        body = body_of(self.src, "static void icon_tile(")
        self.assertIn("lv_obj_set_size(t, 32, 32)", body)
        self.assertIn("lv_obj_set_style_radius(t, 7, 0)", body)
        self.assertIn("0xffffff", body)
        # The palette covers the home rows' symbols.
        palette = body_of(self.src, "static uint32_t icon_color(")
        for symbol in ("LV_SYMBOL_WIFI", "LV_SYMBOL_BLUETOOTH", "LV_SYMBOL_VOLUME_MAX",
                       "LV_SYMBOL_BATTERY_FULL", "LV_SYMBOL_POWER", "LV_SYMBOL_HOME",
                       "LV_SYMBOL_EYE_CLOSE", "LV_SYMBOL_VIDEO", "LV_SYMBOL_IMAGE"):
            self.assertIn(symbol, palette)

    def test_chevron_is_quiet_grey(self) -> None:
        body = body_of(self.src, "static lv_obj_t *row(")
        self.assertIn("COLOR_CHEVRON, LV_SYMBOL_RIGHT", body)

    def test_choice_checkmarks_are_ios_blue(self) -> None:
        for page in ("build_sound_page", "build_language_page", "build_sleep_page"):
            body = body_of(self.src, f"static void {page}(")
            self.assertIn("COLOR_IOS_BLUE", body, page)


class ControlStyleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SETTINGS.read_text(encoding="utf-8")

    def test_switch_is_the_ios_pill(self) -> None:
        body = body_of(self.src, "static lv_obj_t *switch_row(")
        self.assertIn("COLOR_SWITCH_OFF", body)
        self.assertIn("COLOR_IOS_BLUE", body)
        self.assertIn("LV_RADIUS_CIRCLE, LV_PART_KNOB", body)
        self.assertIn("lv_color_white(), LV_PART_KNOB", body)
        # The old purple track/on colours are gone.
        self.assertNotIn("0x3a3358", body)
        self.assertNotIn("COLOR_ACCENT), LV_PART_INDICATOR", body)

    def test_slider_style_is_shared_and_ios_shaped(self) -> None:
        body = body_of(self.src, "static void style_slider_ios(")
        self.assertIn("COLOR_SLIDER_TRACK", body)
        self.assertIn("LV_RADIUS_CIRCLE, LV_PART_KNOB", body)
        self.assertIn("lv_obj_set_style_shadow_width(s, 8, LV_PART_KNOB)", body)
        slider = body_of(self.src, "static lv_obj_t *slider(")
        self.assertIn("style_slider_ios(s)", slider)
        # A lone slider carries its own group surface.
        self.assertIn("lv_obj_set_style_clip_corner(c, true, 0)", slider)

    def test_slider_fill_is_apple_blue_with_tick_dots(self) -> None:
        # Style round (2026-11-18): the settings sliders take the same
        # Apple shape as the quick tile's — iOS-blue fill and seven
        # tick dots under the track. The numeric readout stays.
        body = body_of(self.src, "static void style_slider_ios(")
        self.assertIn("COLOR_IOS_BLUE), LV_PART_INDICATOR", body)
        slider = body_of(self.src, "static lv_obj_t *slider(")
        self.assertIn("LV_ALIGN_OUT_BOTTOM_MID", slider)
        self.assertIn("for (int i = 0; i < 7; i++)", slider)
        self.assertIn("0x636366", slider)


class PageLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = SETTINGS.read_text(encoding="utf-8")

    def test_home_rows_live_in_one_group_in_the_same_order(self) -> None:
        body = body_of(self.src, "static void build_home(")
        self.assertIn("lv_obj_t *g = group(list)", body)
        order = [body.index(sym) for sym in (
            "LV_SYMBOL_WIFI", "LV_SYMBOL_HOME", "LV_SYMBOL_BLUETOOTH",
            "LV_SYMBOL_EYE_CLOSE", "LV_SYMBOL_BATTERY_FULL")]
        self.assertEqual(order, sorted(order))
        # The about note stays outside the group, under it.
        self.assertLess(body.index("LV_SYMBOL_BATTERY_FULL"), body.index("s_about = note(list"))

    def test_back_button_sits_on_a_capsule(self) -> None:
        body = body_of(self.src, "static lv_obj_t *back_button(")
        self.assertIn("LV_RADIUS_CIRCLE, 0)", body)
        self.assertIn("0x2c2c2e", body)

    def test_no_back_button_on_minimal_face_swipe_right_goes_back(self) -> None:
        # Style round (2026-11-18, Hugh): the minimal-face board drops
        # the back button entirely — the top-left corner is the face
        # chrome's arc — and relies on the swipe-right gesture every
        # page already catches. No hint element is added.
        page = body_of(self.src, "static lv_obj_t *page(")
        self.assertIn("back && !muse_board->minimal_face", page)
        text_page = body_of(self.src, "static void build_text_page(")
        self.assertIn("muse_board->minimal_face ? NULL : back_button(s_text)", text_page)
        gesture = body_of(self.src, "static void on_gesture(")
        self.assertIn("LV_DIR_RIGHT", gesture)
        self.assertIn("go_back();", gesture)
        show = body_of(self.src, "static void show(")
        self.assertIn("muse_ui_set_swipe_enabled(p == s_home)", show)

    def test_wifi_saved_strings_are_translated(self) -> None:
        # Style round: the Wi-Fi page's last two English holdouts.
        self.assertIn('TR("Saved networks", "已保存的网络")', self.src)
        self.assertIn('TR("Connected", "已连接")', self.src)

    def test_titles_are_plain_not_accent(self) -> None:
        body = body_of(self.src, "static lv_obj_t *page(")
        self.assertIn("COLOR_TEXT, title", body)
        self.assertNotIn("COLOR_ACCENT, title", body)

    def test_sound_page_groups_modes_and_speaker_controls(self) -> None:
        body = body_of(self.src, "static void build_sound_page(")
        self.assertIn("mode_group = group(list)", body)
        self.assertIn("row(mode_group, NULL, gadget_mode_name(i)", body)
        self.assertIn("switch_row(spk_group, SOUND_SPEAKER", body)
        self.assertIn("slider(spk_group, SOUND_VOLUME", body)

    def test_battery_info_rows_share_one_group(self) -> None:
        body = body_of(self.src, "static void build_battery_page(")
        self.assertIn("info_row(g, ", body)


if __name__ == "__main__":
    unittest.main()
