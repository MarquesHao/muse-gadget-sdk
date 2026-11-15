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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Chinese translations for the screen's own text (project V1.1 fix2),
 * looked up by the English the code says, like muse_lang.c's tables —
 * but kept in their own file and chosen at run time by the Language
 * setting (muse_settings_lang()), where upstream's languages are a
 * build-time choice. Only the state names and the captions are covered:
 * both are drawn in fonts with a CJK fallback. The button menu and the
 * pairing card are drawn in Montserrat, which has no hanzi, so they
 * stay English in both languages. A string with no entry here falls
 * back to English.
 */
enum { MUSE_LANG_ZH_STATUS, MUSE_LANG_ZH_MESSAGE };

/* `en` translated, or `en` itself when the table has none. */
const char *muse_lang_zh_lookup(int kind, const char *en);

#ifdef __cplusplus
}
#endif
