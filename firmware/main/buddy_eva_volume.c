/*
 * MacBoard 音量页：
 *
 *   顶部   "VOLUME 音量"
 *   中间   三档 100 / 50 / 0 竖排，UP / DOWN 移动高亮（品红），已保存的档位右侧标 "SAVED"
 *   底部   操作提示：OK 保存（停留本页），长按 DOWN 切到下一页
 *
 * 与音乐 / 编译页一样按需创建、隐藏即销毁（LVGL 内置堆 24KB，只能有一个 EVA 屏）。
 */
#include "buddy_eva_volume.h"

#include <string.h>

#include "eva_player_frame.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_zh_16);

#define ROW_X 24
#define ROW_W 192
#define ROW_H 44
#define ROW_TOP 70
#define ROW_GAP 14

static const uint8_t s_levels[3] = {100U, 50U, 0U};

static lv_obj_t *s_scr;
static lv_obj_t *s_i4_screen;
static bool s_visible;
static bool s_built;
static lv_obj_t *s_rows[3];
static lv_obj_t *s_row_labels[3];
static lv_obj_t *s_row_saved[3];
static int s_last_pick = -1;
static int s_last_saved = -1;

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, const char *txt, uint32_t color,
                            lv_text_align_t align)
{
    lv_obj_t *lab = lv_label_create(parent);

    lv_obj_set_pos(lab, x, y);
    lv_obj_set_width(lab, w);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lab, &font_zh_16, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lab, align, 0);
    lv_label_set_text(lab, txt);
    return lab;
}

static void build_screen(void)
{
    if (s_built) return;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_scr, 240, 320);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(EVA_FRAME_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    make_label(s_scr, 0, 24, 240, "VOLUME 音量", EVA_FRAME_ORANGE, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        int y = ROW_TOP + i * (ROW_H + ROW_GAP);
        lv_obj_t *row = eva_player_frame_panel(s_scr, ROW_X, y, ROW_W, ROW_H, EVA_FRAME_BG,
                                               EVA_FRAME_ORANGE);
        s_rows[i] = row;
        s_row_labels[i] = make_label(s_scr, ROW_X + 12, y + 13, 80,
                                     s_levels[i] == 100U ? "100" : (s_levels[i] == 50U ? "50" : "0  静音"),
                                     EVA_FRAME_ORANGE, LV_TEXT_ALIGN_LEFT);
        s_row_saved[i] = make_label(s_scr, ROW_X + 96, y + 13, ROW_W - 108, "", EVA_FRAME_GREEN,
                                    LV_TEXT_ALIGN_RIGHT);
    }
    make_label(s_scr, 0, 262, 240, "UP/DOWN 选择  OK 保存", EVA_FRAME_ORANGE, LV_TEXT_ALIGN_CENTER);
    make_label(s_scr, 0, 286, 240, "长按 DOWN 下一页", EVA_FRAME_GREEN, LV_TEXT_ALIGN_CENTER);
    s_last_pick = -1;
    s_last_saved = -1;
    s_built = true;
}

static void destroy_screen(void)
{
    if (!s_built) return;
    lv_obj_delete(s_scr);
    s_scr = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_row_labels, 0, sizeof(s_row_labels));
    memset(s_row_saved, 0, sizeof(s_row_saved));
    s_last_pick = -1;
    s_last_saved = -1;
    s_built = false;
}

void buddy_eva_volume_register_i4_screen(lv_obj_t *screen)
{
    s_i4_screen = screen;
}

bool buddy_eva_volume_active(void)
{
    return s_visible;
}

void buddy_eva_volume_set_visible(bool visible)
{
    if (visible == s_visible) {
        return;
    }
    s_visible = visible;
    if (visible) {
        build_screen();
        lv_screen_load(s_scr);
        eva_player_frame_log_mem("volume shown");
    } else {
        if (s_i4_screen) {
            lv_screen_load(s_i4_screen);
        }
        destroy_screen();
    }
}

void buddy_eva_volume_update(const buddy_ui_snapshot_t *snapshot)
{
    if (!snapshot || !s_visible || !s_built) {
        return;
    }
    if (s_last_pick == snapshot->volume_pick && s_last_saved == snapshot->volume) {
        return;
    }
    s_last_pick = snapshot->volume_pick;
    s_last_saved = snapshot->volume;
    for (int i = 0; i < 3; i++) {
        const bool focused = s_levels[i] == snapshot->volume_pick;
        const bool saved = s_levels[i] == snapshot->volume;

        lv_obj_set_style_bg_color(s_rows[i],
                                  lv_color_hex(focused ? EVA_FRAME_MAGENTA : EVA_FRAME_BG), 0);
        lv_obj_set_style_text_color(s_row_labels[i],
                                    lv_color_hex(focused ? EVA_FRAME_BG : EVA_FRAME_ORANGE), 0);
        lv_obj_set_style_text_color(s_row_saved[i],
                                    lv_color_hex(focused ? EVA_FRAME_BG : EVA_FRAME_GREEN), 0);
        lv_label_set_text(s_row_saved[i], saved ? "SAVED" : "");
    }
}
