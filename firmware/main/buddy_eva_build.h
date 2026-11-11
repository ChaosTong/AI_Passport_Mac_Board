#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_types.h"
#include "lvgl.h"

void buddy_eva_build_init(void);
void buddy_eva_build_register_i4_screen(lv_obj_t *screen);
bool buddy_eva_build_active(void);
void buddy_eva_build_set_visible(bool visible);
void buddy_eva_build_update(const buddy_ui_snapshot_t *snapshot, int log_scroll_px);
