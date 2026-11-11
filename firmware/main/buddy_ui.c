#include "buddy_ui.h"

#include <stdio.h>
#include <string.h>

#include "buddy_eva_build.h"
#include "buddy_eva_music.h"
#include "buddy_eva_volume.h"
#include "eva_player_frame.h"
#include "lvgl.h"

/* MacBoard UI：只有三张 EVA 页（音乐 / 编译 / 音量），按需创建、隐藏即销毁。
 * 基础屏 s_screen 是纯黑底，只承载必须盖在页面上的系统提示（BLE 配对码、
 * Mac 发起的 unpair 确认、Claude 权限请求）。旧 buddy 的 I4 画布（38KB RAM）、
 * HOME/PET/INFO/消息/SETTINGS/菜单 画面已移除。 */

LV_FONT_DECLARE(font_zh_16);

typedef enum {
    UI_NOTICE_NONE,
    UI_NOTICE_CONFIRMATION,
    UI_NOTICE_PAIRING,
    UI_NOTICE_APPROVAL,
} ui_notice_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_notice_title;
static lv_obj_t *s_notice_body;
static lv_obj_t *s_notice_hint;
static buddy_ui_snapshot_t s_snapshot;
static bool s_have_snapshot;
static uint64_t s_elapsed_ms;

static ui_notice_t notice_select(const buddy_ui_snapshot_t *s)
{
    if (s->confirmation_pending) return UI_NOTICE_CONFIRMATION;
    if (s->passkey_visible) return UI_NOTICE_PAIRING;
    if (s->prompt_id[0] != '\0') return UI_NOTICE_APPROVAL;
    return UI_NOTICE_NONE;
}

static lv_obj_t *notice_label(int y, int h, uint32_t color)
{
    lv_obj_t *lab = lv_label_create(s_screen);

    lv_obj_set_pos(lab, 12, y);
    lv_obj_set_size(lab, 216, h);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lab, &font_zh_16, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lab, "");
    return lab;
}

static void draw_notice(ui_notice_t notice, const buddy_ui_snapshot_t *s)
{
    char body[BUDDY_TOOL_MAX + BUDDY_HINT_MAX + 4];

    if (notice == UI_NOTICE_CONFIRMATION) {
        lv_label_set_text(s_notice_title, "确认操作");
        lv_label_set_text(s_notice_body, s->confirmation == BUDDY_CONFIRM_FACTORY_RESET
                                             ? "恢复出厂设置？\n设置与统计将被清除"
                                             : "解除蓝牙配对？\n已保存的配对将被删除");
        lv_label_set_text(s_notice_hint, "OK 确认   DOWN 取消");
    } else if (notice == UI_NOTICE_PAIRING) {
        snprintf(body, sizeof(body), "在 Mac 上输入配对码\n\n%06lu", (unsigned long)s->passkey);
        lv_label_set_text(s_notice_title, "蓝牙配对");
        lv_label_set_text(s_notice_body, body);
        lv_label_set_text(s_notice_hint, "配对完成前请保持本页");
    } else if (notice == UI_NOTICE_APPROVAL) {
        snprintf(body, sizeof(body), "%s\n\n%s", s->prompt_tool, s->prompt_hint);
        lv_label_set_text(s_notice_title, "需要批准");
        lv_label_set_text(s_notice_body, body);
        lv_label_set_text(s_notice_hint,
                          s->approval_locked
                              ? (s->permission_delivery == BUDDY_PERMISSION_DELIVERY_FAILED ? "发送失败"
                                                                                          : "发送中…")
                              : "OK 允许   DOWN 拒绝");
    } else {
        lv_label_set_text(s_notice_title, "");
        lv_label_set_text(s_notice_body, "");
        lv_label_set_text(s_notice_hint, "");
    }
}

static void redraw(void)
{
    ui_notice_t notice;

    if (!s_screen || !s_have_snapshot) return;
    notice = notice_select(&s_snapshot);
    if (notice != UI_NOTICE_NONE) {
        buddy_eva_volume_set_visible(false);
        buddy_eva_music_set_visible(false);
        buddy_eva_build_set_visible(false);
        draw_notice(notice, &s_snapshot);
        return;
    }
    draw_notice(UI_NOTICE_NONE, &s_snapshot);
    if (s_snapshot.page == BUDDY_PAGE_VOLUME) {
        buddy_eva_music_set_visible(false);
        buddy_eva_build_set_visible(false);
        buddy_eva_volume_set_visible(true);
        buddy_eva_volume_update(&s_snapshot);
    } else if (s_snapshot.page == BUDDY_PAGE_BUILD) {
        buddy_eva_volume_set_visible(false);
        buddy_eva_music_set_visible(false);
        buddy_eva_build_set_visible(true);
        buddy_eva_build_update(&s_snapshot, s_elapsed_ms);
    } else {
        /* 音乐页；Mac 还没认领（owner 不是 MacBoard）前的旧页面也落在这里 */
        buddy_eva_volume_set_visible(false);
        buddy_eva_build_set_visible(false);
        buddy_eva_music_set_visible(true);
        buddy_eva_music_update(&s_snapshot, s_elapsed_ms);
    }
}

void buddy_ui_init(void)
{
    if (s_screen) return;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 240, 320);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(EVA_FRAME_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    s_notice_title = notice_label(60, 24, EVA_FRAME_ORANGE);
    s_notice_body = notice_label(104, 140, 0xF7E9D7);
    s_notice_hint = notice_label(270, 24, EVA_FRAME_GREEN);
    buddy_eva_music_init();
    buddy_eva_music_register_i4_screen(s_screen);
    buddy_eva_build_init();
    buddy_eva_build_register_i4_screen(s_screen);
    buddy_eva_volume_register_i4_screen(s_screen);
    lv_screen_load(s_screen);
}

void buddy_ui_render(const buddy_ui_snapshot_t *snapshot)
{
    if (!snapshot) return;
    buddy_ui_init();
    s_snapshot = *snapshot;
    s_have_snapshot = true;
    redraw();
}

void buddy_ui_show_passkey(uint32_t passkey)
{
    buddy_ui_snapshot_t snapshot = {.passkey_visible = true, .passkey = passkey};
    buddy_ui_render(&snapshot);
}

void buddy_ui_tick(uint64_t elapsed_ms)
{
    s_elapsed_ms = elapsed_ms;
    if (!s_have_snapshot) return;
    if (buddy_eva_music_active()) {
        buddy_eva_music_update(&s_snapshot, s_elapsed_ms);
    }
    if (buddy_eva_build_active()) {
        buddy_eva_build_update(&s_snapshot, s_elapsed_ms);
    }
}

void buddy_ui_scroll(int delta)
{
    /* EVA 编译页的日志始终显示最新几行，没有滚动位置；保留接口给 UI_SCROLL 动作。 */
    (void)delta;
}
