#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_types.h"
#include "lvgl.h"

void buddy_eva_music_init(void);
void buddy_eva_music_register_i4_screen(lv_obj_t *screen);
bool buddy_eva_music_active(void);
void buddy_eva_music_set_visible(bool visible);
void buddy_eva_music_update(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms);
