#pragma once

#include <stdbool.h>

#include "buddy_types.h"
#include "lvgl.h"

/* MacBoard 音量页（EVA 风格，3 档 0 / 50 / 100）。显示时创建、隐藏即销毁。 */
void buddy_eva_volume_register_i4_screen(lv_obj_t *screen);
bool buddy_eva_volume_active(void);
void buddy_eva_volume_set_visible(bool visible);
void buddy_eva_volume_update(const buddy_ui_snapshot_t *snapshot);
