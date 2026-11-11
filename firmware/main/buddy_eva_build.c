#include "buddy_eva_build.h"

#include <stdio.h>
#include <string.h>

#include "eva_player_frame.h"
#include "eva_text_buttons.h"
#include "lvgl.h"

#define COLOR_BG EVA_FRAME_BG
#define COLOR_ORANGE EVA_FRAME_ORANGE
#define COLOR_GREEN EVA_FRAME_GREEN
#define COLOR_MAGENTA EVA_FRAME_MAGENTA
#define COLOR_RED 0xFF3359U
#define COLOR_YELLOW 0xFFF14CU
#define STATUS_Y 104
#define STATUS_H 22
#define LOG_Y 130
#define LOG_BOTTOM 258
#define LOG_ROW_H 16

LV_FONT_DECLARE(font_zh_16);

static lv_obj_t *s_scr;
static lv_obj_t *s_i4_screen;
static bool s_visible;
static lv_obj_t *s_battery_image;
static lv_obj_t *s_buttons[4];
static lv_obj_t *s_button_icons[4];
static lv_obj_t *s_status_panel;
static lv_obj_t *s_status_label;
static lv_obj_t *s_log_rows[BUDDY_ENTRY_COUNT];
static lv_obj_t *s_log_labels[BUDDY_ENTRY_COUNT];
static bool s_built;
static char s_last_status[BUDDY_MESSAGE_MAX];
static unsigned s_last_status_color;
static char s_last_log[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
static int s_battery_bars = -1;

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, uint32_t border)
{
    return eva_player_frame_panel(parent, x, y, w, h, bg, border);
}

static void make_button(lv_obj_t *parent, int index, const lv_image_dsc_t *icon, int x)
{
    s_buttons[index] = panel(parent, x, 266, 52, 30, COLOR_BG, COLOR_ORANGE);
    lv_obj_t *img = lv_image_create(s_buttons[index]);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);
    s_button_icons[index] = img;
}

static int battery_bar_count(unsigned percent)
{
    if (percent == 0) return 0;
    if (percent <= 33) return 1;
    if (percent <= 66) return 2;
    return 3;
}

static uint32_t status_color_for(const char *status)
{
    if (status == NULL || status[0] == '\0') {
        return COLOR_ORANGE;
    }
    if (strcmp(status, "busy") == 0 || strcmp(status, "starting") == 0) {
        return COLOR_YELLOW;
    }
    if (strcmp(status, "success") == 0) {
        return COLOR_GREEN;
    }
    if (strcmp(status, "failure") == 0 || strcmp(status, "no-xcode") == 0) {
        return COLOR_RED;
    }
    return COLOR_ORANGE;
}

static void update_button_styles(void)
{
    for (int i = 0; i < 4; i++) {
        uint32_t bg = (i == 1) ? COLOR_MAGENTA : COLOR_BG;
        lv_obj_set_style_bg_color(s_buttons[i], lv_color_hex(bg), 0);
    }
}

static void update_status(const buddy_ui_snapshot_t *snapshot)
{
    const char *status = snapshot->message[0] ? snapshot->message : "idle";
    uint32_t color = status_color_for(status);

    if (strcmp(status, s_last_status) != 0) {
        snprintf(s_last_status, sizeof(s_last_status), "%s", status);
        lv_label_set_text(s_status_label, status);
    }
    if (color != s_last_status_color) {
        s_last_status_color = color;
        lv_obj_set_style_bg_color(s_status_panel, lv_color_hex(color), 0);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_BG), 0);
    }
}

static int count_log_lines(const buddy_ui_snapshot_t *snapshot)
{
    int n = 0;
    for (int i = 0; i < BUDDY_ENTRY_COUNT; i++) {
        if (snapshot->entries[i][0] != '\0') {
            n++;
        }
    }
    return n;
}

static const char *log_line_at(const buddy_ui_snapshot_t *snapshot, int index)
{
    int seen = 0;
    for (int i = 0; i < BUDDY_ENTRY_COUNT; i++) {
        if (snapshot->entries[i][0] == '\0') {
            continue;
        }
        if (seen == index) {
            return snapshot->entries[i];
        }
        seen++;
    }
    return NULL;
}

static void update_log(const buddy_ui_snapshot_t *snapshot, int log_scroll_px)
{
    const int visible = (LOG_BOTTOM - LOG_Y) / LOG_ROW_H;
    int line_count = count_log_lines(snapshot);
    int first = log_scroll_px / LOG_ROW_H;
    if (first < 0) {
        first = 0;
    }
    if (line_count > visible && first > line_count - visible) {
        first = line_count - visible;
    }

    for (int row = 0; row < BUDDY_ENTRY_COUNT; row++) {
        if (row >= visible) {
            lv_obj_add_flag(s_log_rows[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_log_rows[row], LV_OBJ_FLAG_HIDDEN);
        const char *line = log_line_at(snapshot, first + row);
        const char *text = (line != NULL && line[0] != '\0') ? line : " ";
        if (strcmp(text, s_last_log[row]) != 0) {
            snprintf(s_last_log[row], sizeof(s_last_log[row]), "%s", text);
            lv_label_set_text(s_log_labels[row], text);
        }
    }
}

static void update_battery(const buddy_ui_snapshot_t *snapshot)
{
    int bars;
    if (!s_battery_image) return;
    bars = snapshot->battery_available ? battery_bar_count(snapshot->battery_percent) : 0;
    if (bars == s_battery_bars) return;
    eva_player_frame_battery(s_battery_image, bars);
    s_battery_bars = bars;
}

static void build_screen(void)
{
    if (s_built) return;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_scr, 240, 320);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    s_battery_image = eva_player_frame_header(s_scr, "BUILD", &font_zh_16);

    s_status_panel = panel(s_scr, 4, STATUS_Y, 232, STATUS_H, COLOR_ORANGE, COLOR_ORANGE);
    s_status_label = lv_label_create(s_status_panel);
    lv_obj_set_width(s_status_label, 220);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_font(s_status_label, &font_zh_16, 0);
    lv_label_set_text(s_status_label, "idle");
    lv_obj_center(s_status_label);

    for (int i = 0; i < BUDDY_ENTRY_COUNT; i++) {
        int y = LOG_Y + i * LOG_ROW_H;
        if (y + LOG_ROW_H > LOG_BOTTOM) {
            break;
        }
        s_log_rows[i] = panel(s_scr, 6, y, 228, LOG_ROW_H - 1, COLOR_BG, COLOR_ORANGE);
        lv_obj_t *lab = lv_label_create(s_log_rows[i]);
        s_log_labels[i] = lab;
        lv_obj_set_width(lab, 214);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(lab, lv_color_hex(COLOR_ORANGE), 0);
        lv_obj_set_style_text_font(lab, &font_zh_16, 0);
        lv_label_set_text(lab, " ");
        lv_obj_center(lab);
    }

    panel(s_scr, 3, 258, 234, 45, COLOR_GREEN, COLOR_ORANGE);
    make_button(s_scr, 0, &eva_text_btn_prev, 5);
    make_button(s_scr, 1, &eva_text_btn_play, 64);
    make_button(s_scr, 2, &eva_text_btn_next, 123);
    make_button(s_scr, 3, &eva_text_btn_menu, 182);
    panel(s_scr, 3, 313, 234, 2, COLOR_GREEN, COLOR_GREEN);

    s_built = true;
}

void buddy_eva_build_init(void)
{
    build_screen();
}

bool buddy_eva_build_active(void)
{
    return s_visible;
}

void buddy_eva_build_register_i4_screen(lv_obj_t *screen)
{
    s_i4_screen = screen;
}

void buddy_eva_build_set_visible(bool visible)
{
    if (!s_built) {
        buddy_eva_build_init();
    }
    if (visible == s_visible) {
        return;
    }
    s_visible = visible;
    if (visible) {
        lv_screen_load(s_scr);
        memset(s_last_log, 0, sizeof(s_last_log));
        s_last_status[0] = '\0';
        lv_obj_invalidate(s_scr);
    }
}

void buddy_eva_build_update(const buddy_ui_snapshot_t *snapshot, int log_scroll_px)
{
    if (!s_built) {
        buddy_eva_build_init();
    }
    if (!snapshot || !s_visible) {
        return;
    }

    update_status(snapshot);
    update_log(snapshot, log_scroll_px);
    update_battery(snapshot);
    update_button_styles();
}
