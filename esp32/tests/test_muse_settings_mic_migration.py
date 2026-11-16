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

"""The one-time mic-gain migration in muse_settings_init().

A stored mic gain above the 30 dB factory default — one device sat at
the 36 dB ceiling — leaves the noise floor high and the clipping
headroom thin, which reads as muddy recordings. The migration pulls a
stored value above 30 back to 30 exactly once: the "micgain_mig" NVS
marker makes it strictly one-shot, values at or below 30 are never
touched, and whatever the user picks on the slider afterwards stands.

These are source pins: the settings store sits on NVS, which the host
cannot link. The on-device half of the evidence is the boot log — the
migration line, then "mic gain 30 dB" in the audio-ready line.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETTINGS_C = (ROOT / "components/muse/muse_settings.c").read_text()
SETTINGS_H = (ROOT / "components/muse/muse_settings.h").read_text()


def segment(source: str, start: str, end: str) -> str:
    """Source text from one top-level marker up to (not incl.) the next."""
    i = source.index(start)
    return source[i:source.index(end, i)]


INIT = segment(SETTINGS_C, "esp_err_t muse_settings_init(void)",
               "\nvoid muse_settings_set_listener(")


class MicGainMigrationTest(unittest.TestCase):
    def test_marker_gates_the_migration(self):
        self.assertIn('nvs_get_u8(s_nvs, "micgain_mig", &mig) != ESP_OK', INIT)

    def test_only_values_above_30_are_pulled_back(self):
        gate = INIT.index("if (s.mic_gain > 30)")
        pulled = INIT.index("s.mic_gain = 30;")
        self.assertLess(gate, pulled)
        self.assertIn('save_u8("mic_gain", s.mic_gain)', INIT)

    def test_marker_is_written_in_the_same_pass(self):
        read = INIT.index('"micgain_mig", &mig')
        written = INIT.index('save_u8("micgain_mig", 1)')
        self.assertLess(read, written)

    def test_migration_runs_before_the_summary_log(self):
        migrated = INIT.index("mic gain migration:")
        summary = INIT.index('"vol %d%s, mic %d dB')
        self.assertLess(migrated, summary)

    def test_ceiling_and_slider_range_are_untouched(self):
        # The migration moves one stored value; the gain ceiling the
        # slider and BLE setter clamp to stays where it was.
        self.assertIn("#define MUSE_MIC_GAIN_MAX 36", SETTINGS_H)


if __name__ == "__main__":
    unittest.main()
