#include "eva_player_frame.h"
#include "eva_clock.h"
#include "esp_log.h"
#include "eva_text_assets.h"

lv_obj_t *eva_player_frame_panel(lv_obj_t *parent, int x, int y, int w, int h,
    uint32_t bg, uint32_t border)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    return obj;
}

lv_obj_t *eva_player_frame_header(lv_obj_t *parent, const char *mode, const lv_font_t *font)
{
    lv_obj_t *title = eva_player_frame_panel(parent, 4, 14, 132, 34, EVA_FRAME_GREEN, EVA_FRAME_ORANGE);
    lv_obj_t *image = lv_image_create(title);
    lv_image_set_src(image, &eva_text_evangelion);
    lv_obj_set_style_image_recolor(image, lv_color_hex(EVA_FRAME_ORANGE), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_center(image);
    lv_obj_t *battery = eva_player_frame_panel(parent, 142, 12, 94, 44, EVA_FRAME_BG, EVA_FRAME_ORANGE);
    lv_obj_t *battery_image = lv_image_create(battery);
    lv_image_set_src(battery_image, &eva_text_internal_battery_0);
    lv_obj_center(battery_image);
    lv_obj_t *system = eva_player_frame_panel(parent, 142, 63, 94, 30, EVA_FRAME_BG, EVA_FRAME_ORANGE);
    if (mode && font) {
        lv_obj_t *label = lv_label_create(system);
        lv_label_set_text(label, mode);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(EVA_FRAME_ORANGE), 0);
        lv_obj_center(label);
    } else {
        image = lv_image_create(system);
        lv_image_set_src(image, &eva_text_system_jp);
        lv_obj_center(image);
    }
    image = lv_image_create(parent);
    lv_image_set_src(image, &eva_text_status);
    lv_obj_set_style_image_recolor(image, lv_color_hex(EVA_FRAME_ORANGE), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_set_pos(image, 9, 57);
    return battery_image;
}

void eva_player_frame_battery(lv_obj_t *image, int bars)
{
    static const lv_image_dsc_t *const images[] = {
        &eva_text_internal_battery_0, &eva_text_internal_battery_1,
        &eva_text_internal_battery_2, &eva_text_internal_jp,
    };
    if (bars < 0 || bars > 3) bars = 0;
    lv_image_set_src(image, images[bars]);
}

void eva_player_frame_control_bar(lv_obj_t *parent)
{
    eva_player_frame_panel(parent, 3, EVA_FRAME_CTRL_TOP, 234, 45, EVA_FRAME_GREEN, EVA_FRAME_ORANGE);
}

lv_obj_t *eva_player_frame_button(lv_obj_t *parent, int x, const lv_image_dsc_t *icon,
    lv_obj_t **icon_out)
{
    lv_obj_t *button = eva_player_frame_panel(parent, x, EVA_FRAME_BUTTON_Y, EVA_FRAME_BUTTON_W,
        EVA_FRAME_BUTTON_H, EVA_FRAME_BG, EVA_FRAME_ORANGE);
    lv_obj_t *img = lv_image_create(button);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(EVA_FRAME_ORANGE), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    if (icon_out) *icon_out = img;
    return button;
}

void eva_player_frame_button_style(lv_obj_t *button, lv_obj_t *icon, bool focused, bool enabled)
{
    uint32_t bg = (enabled && focused) ? EVA_FRAME_MAGENTA : EVA_FRAME_BG;
    uint32_t ink = enabled ? EVA_FRAME_ORANGE : EVA_FRAME_GREEN;
    if (button) {
        lv_obj_set_style_bg_color(button, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(button, lv_color_hex(ink), 0);
    }
    if (icon) {
        lv_obj_set_style_image_recolor(icon, lv_color_hex(ink), 0);
    }
}

int eva_player_frame_battery_bars(unsigned percent)
{
    if (percent == 0) return 0;
    if (percent <= 33) return 1;
    if (percent <= 66) return 2;
    return 3;
}

LV_DRAW_BUF_DEFINE_STATIC(s_clock_draw_buf, EVA_CLOCK_WIDTH, EVA_CLOCK_HEIGHT, LV_COLOR_FORMAT_A8);

lv_draw_buf_t *eva_player_frame_clock_buf(void)
{
    static bool s_ready;
    if (!s_ready) {
        LV_DRAW_BUF_INIT_STATIC(s_clock_draw_buf);
        s_ready = true;
    }
    return &s_clock_draw_buf;
}

void eva_player_frame_log_mem(const char *what)
{
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI("eva", "%s: lvgl heap free=%u/%u used=%u%% frag=%u%% max_free=%u",
             what ? what : "-", (unsigned)mon.free_size, (unsigned)mon.total_size,
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct, (unsigned)mon.free_biggest_size);
#else
    (void)what;
#endif
}
