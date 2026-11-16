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


if __name__ == "__main__":
    unittest.main()
