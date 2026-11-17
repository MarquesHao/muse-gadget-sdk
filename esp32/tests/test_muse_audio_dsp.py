# Copyright (c) Meta Platforms, Inc. and affiliates.
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

"""The 1.75C microphone voice DSP (muse_audio.c).

The block between the MIC_DSP_EXTRACT markers — a second one-pole
high-pass, two one-pole high-shelf stages, the chunk-wise AGC and
the tanh soft limiter that muse_audio_read() runs on this board
only — is extracted verbatim,
compiled on the host and driven with sines: the shelf curve, the AGC's
convergence to its target, the noise gate's freeze, the gain clamp and
the limiter's ceiling are all pinned numerically. The expected values
come from the offline replica the constants were tuned with (dev
mic-dsp-sim.py). Source invariants pin the wiring: the block, its
reset and its two call sites all sit under the board's config gate, so
every other board compiles exactly what it compiled before.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIO = ROOT / "components" / "muse" / "muse_audio.c"
GATE = "#if CONFIG_MUSE_BOARD_WAVESHARE_S3_175C"

# Shelf gain of the extracted block alone (unity AGC, limiter clear of
# the test level), dB, from the tuning replica. The evening retune
# (2026-11-17) added the block's own 90 Hz high-pass pole ahead of
# the shelf, which costs the low end a little (250 Hz: 1.16 -> 0.49);
# the shelf stages themselves are unchanged at 2x7 dB.
SHELF_DB = {
    250: 0.49, 500: 3.23, 1000: 7.15, 2000: 10.14,
    3000: 11.01, 4000: 11.35, 6000: 11.59,
}

DRIVER = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ==== extracted block goes here ==== */
__BLOCK__
/* ==== end of extracted block ==== */

static mic_dsp_t d;

/* shelf <freq>: one second of sine through post() with the AGC never
 * updated (gain stays 1), print the steady-state gain in dB. */
static void shelf(int freq, double amp)
{
    mic_dsp_reset(&d, 16000.0f);
    double acc = 0.0;
    int n = 0;
    for (int i = 0; i < 16000; i++) {
        float x = (float)(amp * sin(2.0 * M_PI * freq * i / 16000.0));
        float y = mic_dsp_post(&d, x, i % 320, 320);
        if (i >= 8000) {
            acc += (double)y * y;
            n++;
        }
    }
    double out = sqrt(acc / n), in = amp / sqrt(2.0);
    printf("%.3f\n", 20.0 * log10(out / in));
}

/* agc <rms_db> <chunks>: a steady sine at the given RMS through post()
 * + chunk_end(); print the final gain (dB) and the last chunk's output
 * RMS (dBFS). */
static void agc(double rms_db, int chunks)
{
    mic_dsp_reset(&d, 16000.0f);
    double amp = 32768.0 * pow(10.0, rms_db / 20.0) * sqrt(2.0);
    double phase = 0.0, acc = 0.0;
    for (int c = 0; c < chunks; c++) {
        acc = 0.0;
        for (int i = 0; i < 320; i++) {
            float x = (float)(amp * sin(phase));
            phase += 2.0 * M_PI * 440.0 / 16000.0;
            float y = mic_dsp_post(&d, x, i, 320);
            acc += (double)y * y;
        }
        mic_dsp_chunk_end(&d);
    }
    printf("%.3f %.3f\n", 20.0 * log10(d.gain_to),
           20.0 * log10(sqrt(acc / 320.0) / 32768.0));
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "shelf") == 0) {
        shelf(atoi(argv[2]), 6000.0);
    } else if (argc >= 4 && strcmp(argv[1], "agc") == 0) {
        agc(atof(argv[2]), atoi(argv[3]));
    } else if (argc >= 3 && strcmp(argv[1], "limit") == 0) {
        printf("%.1f\n", mic_dsp_limit((float)atof(argv[2])));
    } else {
        return 2;
    }
    return 0;
}
"""


def extract_block(source: str) -> str:
    begin = source.index("/* MIC_DSP_EXTRACT_BEGIN */")
    end = source.index("/* MIC_DSP_EXTRACT_END */")
    return source[begin:end]


class MicDspTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = AUDIO.read_text(encoding="utf-8")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        src = Path(cls.temp.name) / "mic_dsp.c"
        src.write_text(DRIVER.replace("__BLOCK__", extract_block(cls.source)),
                       encoding="utf-8")
        cls.binary = Path(cls.temp.name) / "mic_dsp"
        result = subprocess.run(
            [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra",
             "-Werror", str(src), "-o", str(cls.binary), "-lm"],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_driver(self, *args: str) -> str:
        result = subprocess.run([str(self.binary), *args],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def test_shelf_curve(self) -> None:
        for freq, want in SHELF_DB.items():
            got = float(self.run_driver("shelf", str(freq)))
            self.assertAlmostEqual(got, want, delta=0.8,
                                   msg=f"shelf at {freq} Hz: {got} dB, want {want}")

    def test_shelf_is_a_shelf(self) -> None:
        # Low frequencies pass near unity next to the high lift: it is a
        # shelf, not a broadband gain.
        lo = float(self.run_driver("shelf", "250"))
        hi = float(self.run_driver("shelf", "3000"))
        self.assertLess(lo, 2.5)
        self.assertGreater(hi - lo, 8.0)

    def test_agc_rides_quiet_speech_to_target(self) -> None:
        gain, out = (float(v) for v in self.run_driver("agc", "-36", "200").split())
        self.assertAlmostEqual(out, -20.0, delta=2.0)
        self.assertGreater(gain, 8.0)   # it really rode up from unity

    def test_agc_gate_freezes_in_quiet(self) -> None:
        gain, _ = (float(v) for v in self.run_driver("agc", "-50", "100").split())
        self.assertAlmostEqual(gain, 0.0, delta=0.01)

    def test_agc_gain_is_clamped(self) -> None:
        gain, _ = (float(v) for v in self.run_driver("agc", "-44", "400").split())
        self.assertAlmostEqual(gain, 18.0, delta=0.05)

    def test_agc_pulls_loud_input_down_to_its_floor(self) -> None:
        # The down-range is floored at MIC_AGC_MIN_GAIN_DB: a loud input
        # is pulled to the floor and no further (the limiter, not the
        # AGC, owns loud peaks). -6 dBFS in, ~+3 dB of shelf at the test
        # tone, floor -6: the output sits near -9 dBFS.
        gain, out = (float(v) for v in self.run_driver("agc", "-6", "200").split())
        self.assertAlmostEqual(gain, -6.0, delta=0.05)
        self.assertAlmostEqual(out, -9.0, delta=1.5)

    def test_limiter(self) -> None:
        self.assertAlmostEqual(float(self.run_driver("limit", "1000")), 1000.0, delta=0.1)
        over = float(self.run_driver("limit", "40000"))
        self.assertGreater(over, 25887.0)             # past the knee...
        self.assertLess(over, 32768.0)                # ...but short of the rail
        huge = float(self.run_driver("limit", "200000"))
        # tanhf saturates to exactly 1.0f for extreme overs, so the
        # limiter can return exactly full scale; the caller's int16
        # clamp is the hard bound there. What matters is the approach:
        # monotonic, and never past the rail.
        self.assertLessEqual(huge, 32768.0)
        self.assertGreater(huge, over)                # still monotonic
        neg = float(self.run_driver("limit", "-40000"))
        self.assertAlmostEqual(neg, -over, delta=0.1)

    def test_wiring_is_gated_to_this_board(self) -> None:
        src = self.source
        block = extract_block(src)
        self.assertIn("mic_dsp_post", block)
        self.assertIn("mic_dsp_chunk_end", block)
        # The gate opens before the block and the instance, and the read
        # path's two call sites sit under the same gate: no other board
        # can compile any of it in.
        self.assertLess(src.index(GATE), src.index("/* MIC_DSP_EXTRACT_BEGIN */"))
        read = src[src.index("esp_err_t muse_audio_read"):]
        self.assertIn(GATE, read)
        self.assertIn("mic_dsp_post(&s_mic_dsp, y, (int)i, (int)n)", read)
        self.assertIn("mic_dsp_chunk_end(&s_mic_dsp)", read)
        init = src[src.index("esp_err_t muse_audio_init"):]
        self.assertIn("mic_dsp_reset(&s_mic_dsp, MUSE_AUDIO_RATE)", init)

    def test_constants_stay_in_the_block(self) -> None:
        # Retuning is a one-place edit: every tuning constant is defined
        # inside the extracted block, nowhere else in the file.
        block = extract_block(self.source)
        for name in ("MIC_EQ_CORNER_HZ", "MIC_EQ_STAGE_DB", "MIC_HPF2_HZ",
                     "MIC_AGC_TARGET_DBFS", "MIC_AGC_RELEASE",
                     "MIC_AGC_MAX_GAIN_DB", "MIC_AGC_GATE_DBFS", "MIC_LIMIT_KNEE"):
            self.assertEqual(self.source.count("#define " + name), 1, name)
            self.assertIn("#define " + name, block, name)


if __name__ == "__main__":
    unittest.main()
