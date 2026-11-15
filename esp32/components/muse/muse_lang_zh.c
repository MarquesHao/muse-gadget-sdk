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

#include "muse_lang_zh.h"

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_MUSE_GADGET_ZH

/*
 * The Chinese tables (project V1.1 fix2). Kept out of muse_lang.c on
 * purpose: upstream's tables there are one build-time language, checked
 * by tests/test_muse_lang.py, while these are chosen at run time and
 * cover only what a font with hanzi draws — the state names (caption
 * font with the CJK fallback) and the captions (same). A translation
 * keeps its English format conversions, like upstream's tables.
 */
typedef struct {
    const char *en, *text;
} muse_lang_zh_entry_t;

/* Short names heading the screen, like the English. */
static const muse_lang_zh_entry_t STATUS_ZH[] = {
    { "WAKING UP", "正在开机" },
    { "READY", "就绪" },
    { "LISTENING", "聆听中" },
    { "THINKING", "思考中" },
    { "SPEAKING", "说话中" },
    { "ERROR", "出错" },
    { "GOODBYE", "再见" },
    { "WI-FI OFF", "无线已关" },
    { "SET UP WI-FI", "请设置无线网" },
    { "NO WI-FI", "无无线网" },
    { "RECONNECTING", "重连中" },
    { "CONNECTING", "连接中" },
    { "USB POWER", "USB供电" },
    { "CHARGING %d%%", "充电中 %d%%" },
    { "BATTERY %d%%", "电量 %d%%" },
};

static const muse_lang_zh_entry_t MESSAGES_ZH[] = {
    /* The voice path */
    { "WAKING UP...", "正在开机..." },
    { "LISTENING...", "聆听中..." },
    { "RECORDING...", "录音中..." },
    { "LISTENING %.1fs", "聆听中 %.1fs" },
    { "RECORDING %.1fs", "录音中 %.1fs" },
    { "SENDING VOICE NOTE", "正在发送语音" },
    { "NOTE SENT - WAITING FOR MUSE", "已发送，等 Muse 回复" },
    { "HOLD LONGER TO TALK", "按住久一点再说话" },
    { "RECORD A LONGER NOTE", "请录长一点" },
    { "MICROPHONE ERROR - TRY AGAIN", "麦克风出错，再试一次" },
    { "SET UP MUSE FIRST", "请先完成 Muse 设置" },
    { "NO WI-FI", "没有无线网" },
    { "AUDIO INIT FAILED", "音频初始化失败" },
    { "CONNECTING TO MUSE...", "正在连接 Muse..." },
    { "HANGING UP...", "正在挂断..." },
    { "ONE MOMENT...", "稍等..." },
    { "MIC BUFFER FULL - RELEASE AND RETRY", "麦克风缓冲满了，松开再试" },
    { "REPLY MISSED - HOLD TO TRY AGAIN", "漏了回复，按住再试" },
    /* Notes saved to send later */
    { "COULDN'T SAVE THE NOTE", "语音没存上" },
    { "SAVED, WILL TRY AGAIN", "已保存，稍后重发" },
    { "SAVED, SENDS WHEN ONLINE", "已保存，联网后发送" },
    { "SENDING SAVED NOTE", "正在发送已存语音" },
    { "SAVED NOTE SENT", "已存语音已发送" },
    { "COULDN'T SEND A SAVED NOTE", "已存语音发送失败" },
    { "SAVED NOTE: WILL TRY AGAIN", "语音已保存，稍后重发" },
    { "NOTES STILL WAITING TO SEND", "还有语音没发出去" },
    /* A turn that failed */
    { "CAN'T REACH MUSE", "连不上 Muse" },
    { "MUSE NOT SET UP", "Muse 还没设置好" },
    { "LOST CONNECTION TO MUSE", "和 Muse 断开了" },
    { "NO REPLY FROM MUSE", "Muse 没有回复" },
    { "DIDN'T CATCH THAT", "没听清，再说一遍" },
    { "MUSE STOPPED LISTENING", "Muse 停止聆听了" },
    { "MUSE COULDN'T LISTEN", "Muse 听不了" },
    { "MUSE DIDN'T TAKE IT", "Muse 没收到" },
    { "INTERRUPTED", "被打断了" },
    { "CANCELLED", "已取消" },
    { "SETTINGS CHANGED", "设置已更改" },
    { "REPLY TOO LONG", "回复太长了" },
    { "REPLY TOO LONG: SEE MUSE APP", "回复太长，去 Muse 应用看" },
    { "REPLY BUFFER LIMIT - TRY AGAIN", "回复缓冲满了，再试一次" },
    { "MUSE REPLY ACCESS DENIED (403)", "Muse 拒绝访问（403）" },
    { "MUSE REPLY AUTH REQUIRED (401)", "Muse 需要重新登录（401）" },
    { "CAN'T SUBSCRIBE TO MUSE", "订阅不上 Muse" },
    { "CAN'T KEEP UP", "处理不过来了" },
    { "OUT OF MEMORY", "内存不够了" },
    /* Buttons and power */
    { "SPEAKER ON", "扬声器已开" },
    { "SPEAKER OFF", "扬声器已关" },
    { "HOLD TO MUTE", "按住静音" },
    { "HOLD TO UNMUTE", "按住取消静音" },
    { "HOLD TO POWER OFF", "按住关机" },
    { "GOODBYE!", "再见！" },
    { "COULDN'T POWER OFF", "关不了机" },
    { "PHONE SETUP ON", "手机设置已开" },
    { "PHONE SETUP: %s", "手机设置：%s" },
    { "PHONE SETUP OFF", "手机设置已关" },
    { "RESETTING...", "正在重置..." },
    /* Meeting mode (project V1.1) */
    { "MEETING - STARTING", "会议录音启动中" },
    { "MEETING STORAGE UNAVAILABLE", "会议存储不可用" },
    { "MEETING OUT OF MEMORY", "内存不够，录不了会议" },
    { "MEETING SAVE FAILED", "会议录音保存失败" },
    { "REC %02d:%02d", "录音 %02d:%02d" },
};

static const char *lookup_zh(const muse_lang_zh_entry_t *t, size_t n, const char *en)
{
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(t[i].en, en)) {
            return t[i].text;
        }
    }
    return en;
}

const char *muse_lang_zh_lookup(int kind, const char *en)
{
    if (kind == MUSE_LANG_ZH_STATUS) {
        return lookup_zh(STATUS_ZH, sizeof(STATUS_ZH) / sizeof(STATUS_ZH[0]), en);
    }
    return lookup_zh(MESSAGES_ZH, sizeof(MESSAGES_ZH) / sizeof(MESSAGES_ZH[0]), en);
}

#else   /* !CONFIG_MUSE_GADGET_ZH */

const char *muse_lang_zh_lookup(int kind, const char *en)
{
    (void)kind;
    return en;
}

#endif
