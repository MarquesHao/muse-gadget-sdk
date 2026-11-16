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

"""The settings pages' Link/Hatch state names (muse_settings_ui.c).

muse_link_state_name() / muse_hatch_state_name() stay English — the
button menu translates them through its own table and the BLE status
JSON reports them verbatim — so the settings ticks map them through
link_state_display_name() / hatch_state_display_name(). This compiles
those two helpers as extracted, with the enums parsed from the real
headers, and checks every state in both languages plus the
out-of-range fallback.
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"

LINK_ZH = {
    "MUSE_LINK_BOOT": "启动中",
    "MUSE_LINK_UNPAIRED": "等待配对",
    "MUSE_LINK_PAIRING": "应用已连接",
    "MUSE_LINK_CONFIRM": "确认配对",
    "MUSE_LINK_CONNECTING": "连接中",
    "MUSE_LINK_ONLINE": "在线",
    "MUSE_LINK_OFFLINE": "离线",
    "MUSE_LINK_ERROR": "出错",
}
HATCH_ZH = {
    "MUSE_HATCH_NOT_SET": "未设置",
    "MUSE_HATCH_OFFLINE": "离线",
    "MUSE_HATCH_UNTESTED": "已保存",
    "MUSE_HATCH_TESTING": "连接中",
    "MUSE_HATCH_REACHABLE": "已连接",
    "MUSE_HATCH_UNREACHABLE": "连不上",
}


def enum_names(header: Path, typedef_name: str) -> list[str]:
    source = header.read_text(encoding="utf-8")
    body = re.search(r"typedef enum \{(.*?)\} " + typedef_name + r";", source, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [part.strip() for part in body.split(",") if part.strip()]


def function(source: str, name: str) -> str:
    begin = source.index(f"static const char *{name}(")
    return source[begin:source.index("\n}\n", begin) + 3]


def void_function(source: str, name: str) -> str:
    begin = source.index(f"static void {name}(")
    return source[begin:source.index("\n}\n", begin) + 3]


class SettingsStateNamesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.settings = (MUSE / "muse_settings_ui.c").read_text(encoding="utf-8")
        cls.link_states = enum_names(MUSE / "muse_link.h", "muse_link_state_t")
        cls.hatch_states = enum_names(MUSE / "muse_chat.h", "muse_hatch_state_t")
        # The expected tables must name every state the headers define,
        # so a new state cannot slip through untranslated unnoticed.
        if sorted(cls.link_states) != sorted(LINK_ZH) or sorted(cls.hatch_states) != sorted(HATCH_ZH):
            raise AssertionError("a Link/Hatch state has no expected translation in this test")

        def stub(states: list[str], fn: str, typedef_name: str) -> str:
            names = ", ".join(f'"{name}"' for name in states)
            return (f"static const char *const EN_{fn}[] = {{ {names} }};\n"
                    f"const char *{fn}({typedef_name} s) {{\n"
                    f"    return (int)s >= 0 && (int)s < {len(states)} ? EN_{fn}[(int)s] : \"\";\n}}\n")

        harness = "\n".join([
            "#include <stdio.h>",
            "#include <stdlib.h>",
            "#define CONFIG_MUSE_CJK_FONT 1",
            "enum { MUSE_LANG_EN, MUSE_LANG_ZH, MUSE_LANG_COUNT };",
            "static int s_lang;",
            "int muse_settings_lang(void) { return s_lang; }",
            "typedef enum { " + ", ".join(cls.link_states) + " } muse_link_state_t;",
            "typedef enum { " + ", ".join(cls.hatch_states) + " } muse_hatch_state_t;",
            stub(cls.link_states, "muse_link_state_name", "muse_link_state_t"),
            stub(cls.hatch_states, "muse_hatch_state_name", "muse_hatch_state_t"),
            function(cls.settings, "link_state_display_name"),
            function(cls.settings, "hatch_state_display_name"),
            "int main(int argc, char **argv) {",
            "    (void)argc;",
            "    s_lang = atoi(argv[1]);",
            "    int state = atoi(argv[3]);",
            "    puts(argv[2][0] == 'l' ? link_state_display_name((muse_link_state_t)state)",
            "                           : hatch_state_display_name((muse_hatch_state_t)state));",
            "    return 0;",
            "}",
            "",
        ])
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / "state_names.c"
        src.write_text(harness, encoding="utf-8")
        cls.binary = Path(cls.temp.name) / "state_names"
        result = subprocess.run(
            [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(src), "-o", str(cls.binary)],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def shown(self, lang: int, kind: str, state: int) -> str:
        result = subprocess.run([str(self.binary), str(lang), kind, str(state)],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.rstrip("\n")

    def check_all(self, kind: str, states: list[str], zh: dict[str, str]) -> None:
        for i, name in enumerate(states):
            # English: the raw state name, exactly as the stub reports it.
            self.assertEqual(self.shown(0, kind, i), name, f"{kind} EN {name}")
            # Chinese: the translation this test pins.
            self.assertEqual(self.shown(1, kind, i), zh[name], f"{kind} ZH {name}")
        # An unknown state falls back to the English name (here: empty).
        self.assertEqual(self.shown(1, kind, 99), "")

    def test_link_state_names(self) -> None:
        self.check_all("l", self.link_states, LINK_ZH)

    def test_hatch_state_names(self) -> None:
        self.check_all("h", self.hatch_states, HATCH_ZH)

    def test_ticks_use_the_display_names(self) -> None:
        tick_hatch = void_function(self.settings, "tick_hatch")
        self.assertIn("link_state_display_name(muse_link_state())", tick_hatch)
        self.assertIn("hatch_state_display_name(h.state)", tick_hatch)
        self.assertNotIn("muse_link_state_name(", tick_hatch)
        self.assertNotIn("muse_hatch_state_name(", tick_hatch)
        tick_home = void_function(self.settings, "tick_home")
        self.assertIn("hatch_state_display_name(h.state)", tick_home)
        self.assertNotIn("muse_hatch_state_name(", tick_home)

    def test_raw_names_stay_english_elsewhere(self) -> None:
        # The BLE status JSON and the button menu keep the raw names.
        ble = (MUSE / "muse_ble.c").read_text(encoding="utf-8")
        self.assertIn("muse_link_state_name(muse_link_state())", ble)
        self.assertIn("muse_hatch_state_name(h.state)", ble)
        menu = (MUSE / "muse_menu.c").read_text(encoding="utf-8")
        self.assertIn("muse_lang_menu(muse_link_state_name(muse_link_state()))", menu)
        self.assertIn("muse_lang_menu(muse_hatch_state_name(h.state))", menu)


if __name__ == "__main__":
    unittest.main()
