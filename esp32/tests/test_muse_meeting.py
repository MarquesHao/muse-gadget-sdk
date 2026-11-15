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

"""Meeting recording path and the project device.guide assembly.

The meeting writer runs on a task with an internal stack while capture
runs on the voice task (PSRAM stack), so the two halves cannot be linked
into one host harness without re-creating most of FreeRTOS + FAT. What
can be pinned on the host, and is pinned here:

* the WAV contract itself -- the header byte array baked into
  muse_meeting.c is decoded and checked against the IMA-ADPCM format
  the header file promises, field by field;
* the arithmetic of the space gate (reserve + minimum-free blocks) and
  of the fact sample count in muse_voice.c's meeting_session();
* the ordering invariants the two reviews (P2, weekend) established:
  the dump guard is armed before the writer creates the file, s_op_err
  is reset only by the writer at BEGIN, a failed save is never reported
  as a success, and an error-path tail is dropped rather than padded;
* the device.guide assembly the firmware build performs for this
  project's custom guide, run for real through cmake in script mode,
  including its failure modes (missing / empty / oversize /
  unterminated-comment guides are refused).
"""

from __future__ import annotations

import hashlib
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEETING_C = (ROOT / "components/muse/muse_meeting.c").read_text()
MEETING_H = (ROOT / "components/muse/muse_meeting.h").read_text()
VOICE_C = (ROOT / "components/muse/muse_voice.c").read_text()
OVERLAY = (ROOT / "devices/sdkconfig.muse-gadget-v1").read_text()

# The assembled project guide is a frozen part of the Monday candidate:
# pinning its exact bytes makes any accidental guide drift a test
# failure instead of a silent firmware change. An intentional guide
# edit updates these two constants in the same commit.
GUIDE_ASSEMBLED_BYTES = 1856
GUIDE_ASSEMBLED_SHA256 = "10657d4a0699ac7bb255fa7b1ca8e37fcdfbd0458df5f57cc7dc1447614aac46"


def segment(source: str, start: str, end: str) -> str:
    """Source text from one top-level marker up to (not incl.) the next."""
    i = source.index(start)
    return source[i:source.index(end, i)]


def define_int(source: str, name: str) -> int:
    match = re.search(rf"#define {name}\s+\(?([0-9]+)(?:\s*\*\s*([0-9]+))?", source)
    if not match:
        raise AssertionError(f"define not found: {name}")
    value = int(match.group(1))
    if match.group(2):
        value *= int(match.group(2))
    return value


def wav_header_bytes() -> bytes:
    body = segment(MEETING_C, "static const uint8_t WAV_HDR", "#define HDR_OFF_RIFF_SIZE")
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    inner = body[body.index("{") + 1:body.rindex("}")]
    values = []
    for token in inner.split(","):
        token = token.strip()
        if not token:
            continue
        if token.startswith("'"):
            values.append(ord(token[1]))
        else:
            values.append(int(token, 0))
    return bytes(values)


WRITER = segment(MEETING_C, "static void writer_task(", "\nstatic esp_err_t setup(")
BEGIN = segment(MEETING_C, "esp_err_t muse_meeting_begin(", "\nbool muse_meeting_write(")
WRITE = segment(MEETING_C, "bool muse_meeting_write(", "\nesp_err_t muse_meeting_end(")
END = segment(MEETING_C, "esp_err_t muse_meeting_end(", "\nvoid muse_meeting_console_list(")
SESSION = segment(VOICE_C, "static void meeting_session(void)", "\nstatic void voice_task(")


class WavContractTest(unittest.TestCase):
    def test_header_constants_agree_with_the_public_header(self):
        self.assertEqual(define_int(MEETING_H, "MUSE_MEETING_BLOCK_BYTES"), 512)
        self.assertEqual(define_int(MEETING_H, "MUSE_MEETING_BLOCK_SAMPLES"), 1017)
        self.assertEqual(define_int(MEETING_H, "MUSE_MEETING_RATE"), 16000)

    def test_header_is_sixty_bytes_and_well_formed(self):
        hdr = wav_header_bytes()
        self.assertEqual(len(hdr), define_int(MEETING_C, "MEET_HDR_BYTES"))
        self.assertEqual(len(hdr), 60)
        self.assertEqual(hdr[0:4], b"RIFF")
        self.assertEqual(hdr[8:12], b"WAVE")
        self.assertEqual(hdr[12:16], b"fmt ")
        self.assertEqual(struct.unpack_from("<I", hdr, 16)[0], 20)  # fmt size

    def test_header_fields_match_ima_adpcm_at_the_promised_rate(self):
        hdr = wav_header_bytes()
        rate = define_int(MEETING_H, "MUSE_MEETING_RATE")
        block = define_int(MEETING_H, "MUSE_MEETING_BLOCK_BYTES")
        samples = define_int(MEETING_H, "MUSE_MEETING_BLOCK_SAMPLES")
        self.assertEqual(struct.unpack_from("<H", hdr, 20)[0], 0x11)  # IMA-ADPCM
        self.assertEqual(struct.unpack_from("<H", hdr, 22)[0], 1)     # mono
        self.assertEqual(struct.unpack_from("<I", hdr, 24)[0], rate)
        self.assertEqual(struct.unpack_from("<I", hdr, 28)[0], rate * block // samples)
        self.assertEqual(struct.unpack_from("<H", hdr, 32)[0], block)
        self.assertEqual(struct.unpack_from("<H", hdr, 34)[0], 4)     # bits/sample
        self.assertEqual(struct.unpack_from("<H", hdr, 36)[0], 2)     # cbSize
        self.assertEqual(struct.unpack_from("<H", hdr, 38)[0], samples)

    def test_patch_offsets_point_at_the_fields_they_patch(self):
        hdr = wav_header_bytes()
        self.assertEqual(hdr[40:44], b"fact")
        self.assertEqual(struct.unpack_from("<I", hdr, 44)[0], 4)     # fact size
        self.assertEqual(hdr[52:56], b"data")
        self.assertEqual(define_int(MEETING_C, "HDR_OFF_RIFF_SIZE"), 4)
        self.assertEqual(define_int(MEETING_C, "HDR_OFF_FACT_SAMPLES"), 48)
        self.assertEqual(define_int(MEETING_C, "HDR_OFF_DATA_SIZE"), 56)
        # Each offset lands inside its chunk, not on a neighbouring field.
        self.assertEqual(40 + 8, define_int(MEETING_C, "HDR_OFF_FACT_SAMPLES"))
        self.assertEqual(52 + 4, define_int(MEETING_C, "HDR_OFF_DATA_SIZE"))

    def test_block_encoding_seeds_each_block_with_its_first_sample(self):
        encode = segment(VOICE_C, "static void meet_encode_block(", "\nstatic void meeting_session(")
        self.assertIn("out[0] = (uint8_t)(pcm[0] & 0xff)", encode)
        self.assertIn(".pred = pcm[0], .index = 0", encode)
        self.assertIn("MUSE_MEETING_BLOCK_SAMPLES - 1", encode)
        self.assertIn("out + 4", encode)  # nibbles follow the 4-byte preamble


class SpaceGateTest(unittest.TestCase):
    def test_minimum_free_space_is_about_one_minute(self):
        blocks = define_int(MEETING_C, "MEET_MIN_FREE_BLOCKS")
        byte_rate = 16000 * 512 // 1017
        seconds = blocks * 512 / byte_rate
        self.assertGreaterEqual(seconds, 55)
        self.assertLessEqual(seconds, 65)

    def test_begin_refuses_below_reserve_plus_minimum(self):
        self.assertIn("free_bytes < MEET_RESERVE_BYTES + MEET_MIN_FREE_BLOCKS"
                      " * MUSE_MEETING_BLOCK_BYTES", BEGIN)
        self.assertIn("return ESP_ERR_NO_MEM", BEGIN)
        self.assertEqual(define_int(MEETING_C, "MEET_RESERVE_BYTES"), 256 * 1024)

    def test_max_blocks_spends_everything_above_the_reserve(self):
        self.assertIn("*max_blocks = (uint32_t)((free_bytes - MEET_RESERVE_BYTES)"
                      " / MUSE_MEETING_BLOCK_BYTES)", BEGIN)

    def test_pool_holds_about_three_seconds_of_audio(self):
        pool = define_int(MEETING_C, "MEET_POOL_BLOCKS")
        seconds = pool * 1017 / 16000
        self.assertGreaterEqual(seconds, 2.5)
        self.assertLessEqual(seconds, 3.5)


class BeginPathTest(unittest.TestCase):
    def test_begin_never_formats(self):
        # begin() runs on the voice task; formatting there would crash it.
        # The only format path is the boot-time mount.
        self.assertIn("mount(false)", BEGIN)
        self.assertNotIn("mount(true)", BEGIN)
        self.assertNotIn("mount_or_format_once", BEGIN)
        mount_fn = segment(MEETING_C, "esp_err_t muse_meeting_mount(void)",
                           "\nbool muse_meeting_mounted(")
        self.assertIn("mount_or_format_once()", mount_fn)

    def test_next_name_is_the_first_gap_in_the_sequence(self):
        nxt = segment(MEETING_C, "static int next_index(void)",
                      "\nesp_err_t muse_meeting_begin(")
        self.assertIn('MEET_BASE "/MEET%04d.WAV"', nxt)
        self.assertIn("stat(path, &st) != 0", nxt)

    def test_dump_guard_is_armed_before_the_writer_is_told(self):
        armed = BEGIN.index("s_recording = true")
        sent = BEGIN.index("xQueueSend(s_wq, &m, portMAX_DELAY)")
        self.assertLess(armed, sent)
        self.assertIn("strlcpy(s_current, m.name", BEGIN[:sent])

    def test_stale_done_token_is_drained_before_sending(self):
        drain = BEGIN.index("while (xSemaphoreTake(s_done, 0) == pdTRUE)")
        sent = BEGIN.index("xQueueSend(s_wq, &m, portMAX_DELAY)")
        self.assertLess(drain, sent)

    def test_begin_timeout_resyncs_through_end(self):
        self.assertIn("muse_meeting_end(0);", BEGIN)
        self.assertIn("ESP_ERR_TIMEOUT", BEGIN)

    def test_writer_failure_clears_the_recording_state(self):
        fail = BEGIN.index("if (s_op_err != ESP_OK)")
        self.assertIn("s_recording = false", BEGIN[fail:])
        self.assertIn("s_current[0] = '\\0'", BEGIN[fail:])
        self.assertIn("return s_op_err", BEGIN[fail:])

    def test_name_is_only_reported_after_success(self):
        ok_check = BEGIN.index("if (s_op_err != ESP_OK)")
        named = BEGIN.index("strlcpy(name_out, m.name, name_cap)")
        self.assertLess(ok_check, named)


class WriterPathTest(unittest.TestCase):
    def test_write_gives_up_when_the_pool_is_empty(self):
        self.assertIn("pdMS_TO_TICKS(250)", WRITE)
        self.assertIn("return false", WRITE)
        self.assertIn("memcpy(blk->b, block, MUSE_MEETING_BLOCK_BYTES)", WRITE)

    def test_data_bytes_only_grow_on_a_full_successful_write(self):
        data = segment(WRITER, "m.kind == MSG_DATA", "} else {   /* MSG_END */")
        self.assertIn("fwrite(m.blk->b, 1, MUSE_MEETING_BLOCK_BYTES, s_fp)"
                      " != MUSE_MEETING_BLOCK_BYTES", data)
        wrote = data.index("s_data_bytes += MUSE_MEETING_BLOCK_BYTES")
        checked = data.index("!= MUSE_MEETING_BLOCK_BYTES")
        self.assertLess(checked, wrote)
        # The block returns to the free pool on both paths.
        self.assertIn("xQueueSend(s_freeq, &m.blk, 0)", data)

    def test_writer_bounds_what_a_power_cut_can_lose(self):
        self.assertIn("(s_data_bytes & 0x7fff) == 0", WRITER)
        self.assertIn("fflush(s_fp)", WRITER)

    def test_header_write_failure_removes_the_stub_file(self):
        begin = segment(WRITER, "m.kind == MSG_BEGIN", "} else if (m.kind == MSG_DATA)")
        self.assertIn("fwrite(WAV_HDR, 1, sizeof(WAV_HDR), s_fp) != sizeof(WAV_HDR)", begin)
        failed = begin.index("!= sizeof(WAV_HDR)")
        self.assertIn("remove(s_fp_path)", begin[failed:])

    def test_topped_session_with_audio_is_salvaged_by_backfill(self):
        begin = segment(WRITER, "m.kind == MSG_BEGIN", "} else if (m.kind == MSG_DATA)")
        # The salvage patches the OLD file's header: it must run before
        # the new file is opened and before the byte counter is reset
        # for the new session.
        salvage = begin.index("HDR_OFF_DATA_SIZE")
        self.assertLess(salvage, begin.index('fopen(s_fp_path, "wb")'))
        self.assertLess(salvage, begin.index("s_data_bytes = 0"))
        for offset in ("HDR_OFF_RIFF_SIZE", "HDR_OFF_FACT_SAMPLES", "HDR_OFF_DATA_SIZE"):
            self.assertIn(f"fseek(s_fp, {offset}, SEEK_SET)", begin)
        # The fact count is derived from whole counted blocks, the same
        # arithmetic MSG_END relies on: every MSG_DATA block is full.
        self.assertIn("(s_data_bytes / MUSE_MEETING_BLOCK_BYTES)", begin)
        self.assertIn("* MUSE_MEETING_BLOCK_SAMPLES", begin)
        self.assertIn("MEET_HDR_BYTES - 8 + s_data_bytes", begin)

    def test_topped_session_without_audio_is_removed(self):
        begin = segment(WRITER, "m.kind == MSG_BEGIN", "} else if (m.kind == MSG_DATA)")
        topped = begin.index("if (s_data_bytes == 0)")
        self.assertIn("remove(s_fp_path)", begin[topped:])
        # The removal still names the old file: it happens before
        # s_fp_path is overwritten with the new session's name.
        self.assertLess(topped, begin.index("snprintf(s_fp_path"))

    def test_salvage_never_touches_the_new_sessions_error(self):
        begin = segment(WRITER, "m.kind == MSG_BEGIN", "} else if (m.kind == MSG_DATA)")
        topped = begin[:begin.index("snprintf(s_fp_path")]
        self.assertNotIn("s_op_err =", topped)

    def test_op_err_is_reset_only_by_the_writer_at_begin(self):
        begin = segment(WRITER, "m.kind == MSG_BEGIN", "} else if (m.kind == MSG_DATA)")
        self.assertIn("s_op_err = ESP_OK", begin)
        # end() must not clear a mid-recording failure before reporting it.
        self.assertNotIn("s_op_err =", END)

    def test_end_patches_all_three_size_fields(self):
        end = WRITER[WRITER.index("/* MSG_END */"):]
        for offset in ("HDR_OFF_RIFF_SIZE", "HDR_OFF_FACT_SAMPLES", "HDR_OFF_DATA_SIZE"):
            self.assertIn(f"fseek(s_fp, {offset}, SEEK_SET)", end)
        self.assertIn("MEET_HDR_BYTES - 8 + s_data_bytes", end)

    def test_end_removes_a_zero_sample_recording(self):
        end = WRITER[WRITER.index("/* MSG_END */"):]
        self.assertIn("m.total_samples == 0 && s_data_bytes == 0", end)
        self.assertIn("remove(s_fp_path)", end)

    def test_end_reports_the_writers_error(self):
        self.assertIn("s_op_err : ESP_ERR_TIMEOUT", END)
        self.assertIn("s_recording = false", END)


class SessionPathTest(unittest.TestCase):
    def test_begin_failure_is_a_plain_storage_caption(self):
        self.assertIn('muse_state_set_caption("MEETING STORAGE UNAVAILABLE")', SESSION)
        self.assertIn("s_meet_failed = true", SESSION)
        self.assertIn("muse_state_set_mode(MUSE_MODE_IDLE)", SESSION)

    def test_stage_failure_ends_the_file_and_names_memory(self):
        alloc = SESSION.index("if (!stage)")
        block = SESSION[alloc:SESSION.index("return;", alloc)]
        self.assertIn("muse_meeting_end(0)", block)
        self.assertIn('muse_state_set_caption("MEETING OUT OF MEMORY")', block)
        self.assertIn("muse_state_set_mode(MUSE_MODE_IDLE)", block)

    def test_full_disk_ends_at_the_begin_reported_limit(self):
        self.assertIn("if (++blocks >= max_blocks)", SESSION)
        self.assertIn("full = true", SESSION)
        self.assertIn('"STORAGE FULL - SAVED %s"', SESSION)

    def test_any_press_stops_and_no_epoch_can_veto_it(self):
        self.assertIn("if (ev.type == MUSE_PTT_DOWN)", SESSION)
        self.assertIn("stop_press = true", SESSION)
        self.assertNotIn("ev.epoch", SESSION)
        self.assertNotIn(".epoch ==", SESSION)

    def test_leaving_the_mode_or_shutting_down_stops(self):
        self.assertIn("muse_settings_gadget_mode() != MUSE_GADGET_MODE_MEETING", SESSION)
        self.assertIn("atomic_load(&s_shutdown)", SESSION)

    def test_audio_error_drops_the_staged_tail(self):
        self.assertIn("if (!error && staged > 0)", SESSION)
        tail = SESSION[SESSION.index("if (!error && staged > 0)"):]
        self.assertIn("stage[i] = stage[staged - 1]", tail)  # pad with last sample

    def test_fact_count_is_written_samples_not_captured(self):
        self.assertIn("enc_samples += MUSE_MEETING_BLOCK_SAMPLES", SESSION)
        self.assertIn("enc_samples += tail_samples", SESSION)
        self.assertIn("muse_meeting_end(enc_samples)", SESSION)
        self.assertNotIn("muse_meeting_end(total)", SESSION)
        self.assertIn("enc_samples / MUSE_AUDIO_RATE", SESSION)

    def test_write_failure_is_an_early_stop_not_a_clean_save(self):
        self.assertIn("if (!muse_meeting_write(block))", SESSION)
        self.assertIn('"SAVED %s (STOPPED EARLY)"', SESSION)

    def test_failed_save_is_a_caption_that_stays(self):
        self.assertIn('muse_state_set_caption("MEETING SAVE FAILED")', SESSION)
        self.assertNotIn('muse_state_set_hint("MEETING SAVE FAILED")', SESSION)

    def test_recording_flag_drops_before_finalize(self):
        cleared = SESSION.index("muse_state_set_recording(false)")
        ended = SESSION.index("muse_meeting_end(enc_samples)")
        self.assertLess(cleared, ended)


class ConsoleGuardTest(unittest.TestCase):
    def test_dump_refuses_the_file_being_recorded(self):
        dump = MEETING_C[MEETING_C.index("bool muse_meeting_console_dump(int index)"):]
        self.assertIn("is being recorded", dump)
        self.assertIn("recording && !strcmp(path + 6, current)", dump)

    def test_dump_index_is_bounded(self):
        dump = MEETING_C[MEETING_C.index("bool muse_meeting_console_dump(int index)"):]
        self.assertIn("index < 1 || index > 9999", dump)


def assemble(out: Path, guide: str | None) -> subprocess.CompletedProcess:
    defines = [f"-DCONFIG_MUSE_DEVICE_GUIDE={guide}"] if guide else []
    return subprocess.run(
        ["cmake", f"-DGUIDE_OUT={out}", *defines, "-P",
         str(ROOT / "cmake/device_guide.cmake")],
        capture_output=True, text=True)


def flat(text: str) -> str:
    """CMake wraps its error messages to the terminal width, and the wrap
    point moves with the length of the paths in the message (a long
    TMPDIR alone is enough). Match refusal phrases on whitespace-flattened
    stderr so the assertion does not depend on where the wrap landed."""
    return " ".join(text.split())


@unittest.skipUnless(shutil.which("cmake"), "cmake unavailable")
class DeviceGuideAssemblyTest(unittest.TestCase):
    def test_overlay_selects_the_project_guide(self):
        self.assertIn('CONFIG_MUSE_DEVICE_GUIDE="devices/guides/muse-gadget-175c.md"',
                      OVERLAY)

    def test_project_guide_assembles_to_the_pinned_bytes(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "guide.md"
            ran = assemble(out, "devices/guides/muse-gadget-175c.md")
            self.assertEqual(ran.returncode, 0, ran.stderr)
            data = out.read_bytes()
            self.assertEqual(len(data), GUIDE_ASSEMBLED_BYTES)
            self.assertEqual(hashlib.sha256(data).hexdigest(), GUIDE_ASSEMBLED_SHA256)

    def test_project_guide_says_what_the_firmware_does(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "guide.md"
            self.assertEqual(assemble(out, "devices/guides/muse-gadget-175c.md").returncode, 0)
            text = out.read_text()
            self.assertTrue(text.startswith("# Muse desk gadget (project build)"))
            self.assertNotIn("<!--", text)  # license comment stripped
            for phrase in ("two buttons", "Voice call", "Text replies", "Meeting",
                           "starts recording the room at once", "stored only on the gadget",
                           "nothing is uploaded", "quick\npanel", "volume"):
                self.assertIn(phrase, text)

    def test_default_guide_still_assembles(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "guide.md"
            ran = assemble(out, None)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertTrue(out.read_text().startswith("# "))

    def test_leading_comment_is_stripped_from_a_synthetic_guide(self):
        with tempfile.TemporaryDirectory() as name:
            src = Path(name) / "g.md"
            src.write_text("<!-- license\nheader -->\n# Synthetic\n\nBody.\n")
            out = Path(name) / "out.md"
            ran = assemble(out, str(src))
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertEqual(out.read_text(), "# Synthetic\n\nBody.\n")

    def test_missing_guide_is_refused(self):
        with tempfile.TemporaryDirectory() as name:
            ran = assemble(Path(name) / "out.md", "no/such-guide.md")
            self.assertNotEqual(ran.returncode, 0)
            self.assertIn("not found", flat(ran.stderr))

    def test_unterminated_comment_is_refused(self):
        with tempfile.TemporaryDirectory() as name:
            src = Path(name) / "g.md"
            src.write_text("<!-- never closed\n# Hi\n")
            ran = assemble(Path(name) / "out.md", str(src))
            self.assertNotEqual(ran.returncode, 0)
            self.assertIn("unterminated HTML comment", flat(ran.stderr))

    def test_empty_guide_is_refused(self):
        with tempfile.TemporaryDirectory() as name:
            src = Path(name) / "g.md"
            src.write_text("<!-- only a comment -->\n")
            ran = assemble(Path(name) / "out.md", str(src))
            self.assertNotEqual(ran.returncode, 0)
            self.assertIn("must be 1 to 4096", flat(ran.stderr))

    def test_oversize_guide_is_refused(self):
        with tempfile.TemporaryDirectory() as name:
            src = Path(name) / "g.md"
            src.write_text("# Big\n" + "x" * 5000 + "\n")
            ran = assemble(Path(name) / "out.md", str(src))
            self.assertNotEqual(ran.returncode, 0)
            self.assertIn("must be 1 to 4096", flat(ran.stderr))


if __name__ == "__main__":
    unittest.main()
