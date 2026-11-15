/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Host fake: muse_voice_call.cpp includes esp_attr.h for its section
 * attributes; on the host they are all no-ops. */

#pragma once

#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR   // muse_voice_call's call state lives in PSRAM on device (the PSRAM move)
