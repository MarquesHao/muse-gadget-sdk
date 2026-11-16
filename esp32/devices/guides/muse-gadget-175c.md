<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->
# Muse desk gadget (project build)

A pocket gadget the owner built from a Waveshare ESP32-S3-Touch-AMOLED-1.75C:
a round 466 by 466 touch screen showing the Jolly face, a microphone and a
speaker, and two buttons on one side: the talk button on top, and below it
one that puts the gadget to sleep and wakes it again. It runs a project
build of the Muse Gadget firmware with three modes the stock build does
not have. The owner talks to Muse from it, on the desk or carried around,
over Wi-Fi.

## Modes

The owner picks one mode and it stays until changed: in Settings, on the
Sound page, or by swiping right on the face, which opens a quick settings
page with the same three mode rows and the volume slider. Swiping left on
the face opens Settings. Tapping the face only makes Muse happy; it opens
nothing.

- Voice call: tapping the top button starts or ends a live call; holding it
  records a voice note that is sent on release. Replies are spoken aloud and
  shown as captions.
- Text replies: the owner holds the top button and speaks; replies appear as
  text on the screen only and are never played aloud. Keep answers short
  enough to read on a small round screen.
- Meeting: entering this mode starts recording the room at once. The face
  lies down, a red dot pulses in the corner, and the screen counts up the
  recording time. Any press of the top button stops and saves; switching
  modes stops it too. Recordings are WAV files stored only on the gadget
  (about 50 minutes in total); nothing is uploaded, and Muse cannot fetch
  them. The owner exports them over a USB cable later.

## Volume and the face

Speaker volume is set on the gadget itself, on the quick settings page
(swipe right on the face) or on the Sound page; if the owner says replies
are too quiet or too loud, point them there. The face mirrors the conversation with its usual expressions, yawns
when left alone for a while, and sleeps flat with the red recording dot
while a meeting is being recorded.
