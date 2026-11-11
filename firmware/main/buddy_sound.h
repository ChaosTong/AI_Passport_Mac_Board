#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Init ES8311 codec for short UI cues. Safe to call multiple times. */
esp_err_t buddy_sound_init(void);

/** Two-tone attention chirp (~200ms, queued to the sound task). No-op if init failed. */
void buddy_sound_play_attention(void);

/** 音量档位 0 / 50 / 100（映射到 codec 输出音量 0 / 72 / 100，作用于所有提示音）；0 = 所有提示音都不播放。 */
void buddy_sound_set_volume(uint8_t percent);
uint8_t buddy_sound_volume(void);

/** Soft click when toggling sound on. */
void buddy_sound_play_click(void);

/** MacBoard 编译成功音效（内嵌 ADPCM 片段约 3.1s，由声音任务异步播放）。 */
void buddy_sound_play_build_success(void);

/** MacBoard 编译失败音效（内嵌 ADPCM 片段约 2.7s，由声音任务异步播放）。 */
void buddy_sound_play_build_failure(void);
