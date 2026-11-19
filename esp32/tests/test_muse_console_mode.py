# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The serial console's gadget-mode bench command (">mode=...").

The command exists so the meeting flow can be driven end to end over the
USB console without fingers: switch to meeting, let it record, switch
away, then meet.list / meet.dump. It must stay a pure test channel:

* it switches through the same persisted setter the settings UI uses
  (muse_settings_set_gadget_mode), never a private side path, so the
  voice loop sees exactly the change a finger would make -- entering
  meeting arms a recording, leaving it stops and saves;
* the three names map one-to-one onto the gadget modes, in enum order;
* an unknown name changes nothing and reports the valid names;
* the command is claimed by the console (returns true) before the
  chat fallthrough can see it.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INPUT_C = (ROOT / "components/muse/muse_input.c").read_text()
SETTINGS_H = (ROOT / "components/muse/muse_settings.h").read_text()


def segment(text: str, start: str, end: str) -> str:
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


SET_MODE = segment(INPUT_C, "static void set_gadget_mode(", "\n/*\n * Console-only commands")
DISPATCH = segment(INPUT_C, 'if (!strncmp(line, "mode=", 5))', "return true;\n    }")


class ModeCommandTest(unittest.TestCase):
    def test_names_cover_every_gadget_mode_in_order(self) -> None:
        enum = re.search(r"enum \{ (MUSE_GADGET_MODE_[A-Z]+(?:, MUSE_GADGET_MODE_[A-Z]+)*), MUSE_GADGET_MODE_COUNT \};",
                         SETTINGS_H)
        self.assertIsNotNone(enum)
        modes = [m.strip() for m in enum.group(1).split(",")]
        self.assertEqual(modes, ["MUSE_GADGET_MODE_CALL", "MUSE_GADGET_MODE_TEXT",
                                 "MUSE_GADGET_MODE_MEETING"])
        for mode, name in zip(modes, ["call", "text", "meeting"]):
            self.assertIn(f'[{mode}] = "{name}"', SET_MODE)

    def test_switch_goes_through_the_settings_setter(self) -> None:
        self.assertIn("muse_settings_set_gadget_mode(m);", SET_MODE)
        # The echo reports the mode the setter actually landed on.
        self.assertIn('printf("@mode %s\\n", names[muse_settings_gadget_mode()]);', SET_MODE)

    def test_unknown_mode_changes_nothing_and_lists_names(self) -> None:
        err = SET_MODE.index("@mode.error")
        setter = SET_MODE.index("muse_settings_set_gadget_mode(m);")
        self.assertGreater(err, setter)  # only reached after no name matched
        self.assertIn('unknown mode \\"%s\\": call text meeting', SET_MODE)

    def test_poke_precedes_the_switch(self) -> None:
        self.assertLess(SET_MODE.index("muse_state_poke();"),
                        SET_MODE.index("muse_settings_set_gadget_mode(m);"))

    def test_console_claims_the_command(self) -> None:
        self.assertIn("set_gadget_mode(line + 5);", DISPATCH)
        console = segment(INPUT_C, "static bool console_command(", "\n/*\n * Bench testing over the USB cable")
        self.assertLess(console.index('strncmp(line, "mode=", 5)'),
                        console.index('strncmp(line, "chat", 4)'))


if __name__ == "__main__":
    unittest.main()
