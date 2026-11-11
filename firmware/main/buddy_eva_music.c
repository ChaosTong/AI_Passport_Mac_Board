#include "buddy_eva_music.h"

#include "buddy_types.h"
#include <stdio.h>
#include <string.h>

#include "eva_clock.h"
#include "eva_player_frame.h"
#include "eva_text_buttons.h"
#include "lvgl.h"

#define COLOR_BG EVA_FRAME_BG
#define COLOR_ORANGE EVA_FRAME_ORANGE
#define COLOR_GREEN EVA_FRAME_GREEN
#define COLOR_MAGENTA EVA_FRAME_MAGENTA
#define TRACK_PANEL_W 232
#define TRACK_PANEL_H 46
/* 列表须落在 y=258 控制条之上（按钮 y=266）。8 行 × 16px + 起点 130 = 258。 */
#define PLAYLIST_TITLE_Y 104
#define PLAYLIST_TITLE_H 22
#define PLAYLIST_LIST_Y 130
#define PLAYLIST_CTRL_TOP 258
#define PLAYLIST_ROW_H ((PLAYLIST_CTRL_TOP - PLAYLIST_LIST_Y) / BUDDY_ENTRY_COUNT)
#define PLAYLIST_ROW_COUNT BUDDY_ENTRY_COUNT

LV_FONT_DECLARE(font_zh_16);

static lv_obj_t *s_scr;
static lv_obj_t *s_clock;
static lv_obj_t *s_battery_image;
static lv_obj_t *s_buttons[4];
static lv_obj_t *s_button_icons[4];
static lv_obj_t *s_track_panel;
static lv_obj_t *s_track_label;
static lv_obj_t *s_playlist_title_panel;
static lv_obj_t *s_playlist_title;
static lv_obj_t *s_playlist_rows[PLAYLIST_ROW_COUNT];
static lv_obj_t *s_playlist_row_labels[PLAYLIST_ROW_COUNT];
static lv_obj_t *s_i4_screen;
static uint16_t s_last_list_pick = UINT16_MAX;
static uint16_t s_last_list_window = UINT16_MAX;
static char s_last_list_rows[PLAYLIST_ROW_COUNT][BUDDY_ENTRY_MAX];
static char s_last_list_title[BUDDY_MESSAGE_MAX];
static bool s_built;
static bool s_visible;
static char s_last_title[sizeof(((buddy_ui_snapshot_t *)0)->message)];
static char s_last_artist[BUDDY_ENTRY_MAX];
static uint32_t s_last_drawn_remaining = UINT32_MAX;
static unsigned s_last_running = UINT32_MAX;
static unsigned s_last_toggle_running = UINT32_MAX;
static int s_battery_bars = -1;
static uint32_t s_position_base_ms;
static uint64_t s_position_base_at_ms;

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, uint32_t border)
{
    return eva_player_frame_panel(parent, x, y, w, h, bg, border);
}

static void make_button(lv_obj_t *parent, int index, const lv_image_dsc_t *icon, int x)
{
    s_buttons[index] = eva_player_frame_button(parent, x, icon, &s_button_icons[index]);
}

static void update_button_styles(uint8_t focus)
{
    int active = (focus < 4) ? (int)focus : 1;
    for (int i = 0; i < 4; i++) {
        eva_player_frame_button_style(s_buttons[i], s_button_icons[i], active == i, true);
    }
}

static void update_play_toggle_label(const buddy_ui_snapshot_t *snapshot)
{
    unsigned playing;
    const lv_image_dsc_t *icon;

    if (!s_button_icons[1]) {
        return;
    }
    playing = snapshot->running ? 1U : 0U;
    if (playing == s_last_toggle_running) {
        return;
    }
    s_last_toggle_running = playing;
    icon = playing ? &eva_text_btn_pause : &eva_text_btn_play;
    lv_image_set_src(s_button_icons[1], icon);
}

static uint32_t playback_elapsed_ms(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    uint32_t base = s_position_base_ms;
    if (snapshot->running && s_position_base_at_ms > 0 && elapsed_ms >= s_position_base_at_ms) {
        base += (uint32_t)(elapsed_ms - s_position_base_at_ms);
    }
    if (snapshot->total > 0) {
        uint32_t cap = snapshot->total * 1000U;
        if (base > cap) base = cap;
    }
    return base;
}

static uint32_t playback_remaining_ms(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    uint32_t elapsed = playback_elapsed_ms(snapshot, elapsed_ms);
    if (snapshot->total == 0) {
        return 0;
    }
    {
        uint32_t total_ms = snapshot->total * 1000U;
        if (elapsed >= total_ms) {
            return 0;
        }
        return total_ms - elapsed;
    }
}

static void draw_clock(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    uint32_t remaining = playback_remaining_ms(snapshot, elapsed_ms);
    if (!s_clock) return;
    if (!snapshot->running && remaining == s_last_drawn_remaining &&
        s_last_running == snapshot->running) {
        return;
    }
    lv_draw_buf_t *buf = eva_player_frame_clock_buf();
    if (eva_clock_render_a8(buf->data, buf->header.stride, remaining)) {
        s_last_drawn_remaining = remaining;
        s_last_running = snapshot->running;
        lv_obj_invalidate(s_clock);
    }
}

static void sync_position_anchor(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    (void)elapsed_ms;
    /* Music page: heartbeat `waiting` = playback position (ms). */
    s_position_base_ms = snapshot->waiting;
    s_position_base_at_ms = elapsed_ms;
    s_last_drawn_remaining = UINT32_MAX;
}

static void update_track(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    const char *title = snapshot->message[0] ? snapshot->message : "—";
    const char *artist = snapshot->entries[0][0] ? snapshot->entries[0] : "";
    char line[sizeof(s_last_title) + BUDDY_ENTRY_MAX + 4];

    if (strcmp(title, s_last_title) == 0 && strcmp(artist, s_last_artist) == 0) {
        return;
    }
    snprintf(s_last_title, sizeof(s_last_title), "%s", title);
    snprintf(s_last_artist, sizeof(s_last_artist), "%s", artist);
    if (artist[0] != '\0') {
        snprintf(line, sizeof(line), "%s\n%s", title, artist);
    } else {
        snprintf(line, sizeof(line), "%s", title);
    }
    lv_label_set_text(s_track_label, line);
    sync_position_anchor(snapshot, elapsed_ms);
}

static void set_now_playing_visible(bool visible)
{
    if (s_clock) {
        if (visible) {
            lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_track_panel) {
        if (visible) {
            lv_obj_remove_flag(s_track_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_track_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_playlist_title_panel) {
        if (visible) {
            lv_obj_add_flag(s_playlist_title_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_playlist_title_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < PLAYLIST_ROW_COUNT; i++) {
        if (!s_playlist_rows[i]) {
            continue;
        }
        if (visible) {
            lv_obj_add_flag(s_playlist_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_playlist_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_playlist_list(const buddy_ui_snapshot_t *snapshot)
{
    const char *title = snapshot->message[0] ? snapshot->message : "—";
    int i;

    if (strcmp(title, s_last_list_title) != 0) {
        snprintf(s_last_list_title, sizeof(s_last_list_title), "%s", title);
        lv_label_set_text(s_playlist_title, title);
    }

    for (i = 0; i < PLAYLIST_ROW_COUNT; i++) {
        const char *line = snapshot->entries[i];
        bool active = snapshot->music_playlist_pick ==
                      (uint16_t)(snapshot->music_playlist_window + (uint16_t)i);
        uint32_t bg = active ? COLOR_MAGENTA : COLOR_BG;
        uint32_t fg = active ? COLOR_BG : COLOR_ORANGE;

        if (strcmp(line, s_last_list_rows[i]) != 0) {
            snprintf(s_last_list_rows[i], sizeof(s_last_list_rows[i]), "%s", line);
            if (s_playlist_row_labels[i]) {
                lv_label_set_text(s_playlist_row_labels[i], line[0] ? line : " ");
            }
        }
        lv_obj_set_style_bg_color(s_playlist_rows[i], lv_color_hex(bg), 0);
        if (s_playlist_row_labels[i]) {
            lv_obj_set_style_text_color(s_playlist_row_labels[i], lv_color_hex(fg), 0);
        }
    }

    s_last_list_pick = snapshot->music_playlist_pick;
    s_last_list_window = snapshot->music_playlist_window;
}

static void update_battery(const buddy_ui_snapshot_t *snapshot)
{
    int bars;
    if (!s_battery_image) return;
    bars = snapshot->battery_available ? eva_player_frame_battery_bars(snapshot->battery_percent) : 0;
    if (bars == s_battery_bars) return;
    eva_player_frame_battery(s_battery_image, bars);
    s_battery_bars = bars;
}

static void build_screen(void)
{
    if (s_built) return;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    s_battery_image = eva_player_frame_header(s_scr, NULL, NULL);

    s_clock = lv_canvas_create(s_scr);
    lv_canvas_set_draw_buf(s_clock, eva_player_frame_clock_buf());
    lv_obj_set_pos(s_clock, 8, 106);
    lv_obj_set_style_image_recolor(s_clock, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_image_recolor_opa(s_clock, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_clock, 0, 0);
    lv_obj_set_style_border_color(s_clock, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_border_width(s_clock, 1, 0);

    s_playlist_title_panel =
        panel(s_scr, 4, PLAYLIST_TITLE_Y, TRACK_PANEL_W, PLAYLIST_TITLE_H, COLOR_GREEN, COLOR_ORANGE);
    s_playlist_title = lv_label_create(s_playlist_title_panel);
    lv_obj_set_width(s_playlist_title, TRACK_PANEL_W - 8);
    lv_label_set_long_mode(s_playlist_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_playlist_title, lv_color_hex(COLOR_ORANGE), 0);
    lv_obj_set_style_text_font(s_playlist_title, &font_zh_16, 0);
    lv_obj_set_style_text_align(s_playlist_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_playlist_title, "—");
    lv_obj_center(s_playlist_title);
    lv_obj_add_flag(s_playlist_title_panel, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < PLAYLIST_ROW_COUNT; i++) {
        int y = PLAYLIST_LIST_Y + i * PLAYLIST_ROW_H;
        s_playlist_rows[i] =
            panel(s_scr, 6, y, 228, PLAYLIST_ROW_H - 1, COLOR_BG, COLOR_ORANGE);
        lv_obj_t *lab = lv_label_create(s_playlist_rows[i]);
        s_playlist_row_labels[i] = lab;
        lv_obj_set_width(lab, 214);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(lab, lv_color_hex(COLOR_ORANGE), 0);
        lv_obj_set_style_text_font(lab, &font_zh_16, 0);
        lv_label_set_text(lab, " ");
        lv_obj_center(lab);
        lv_obj_add_flag(s_playlist_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_track_panel = panel(s_scr, 4, 206, TRACK_PANEL_W, TRACK_PANEL_H, COLOR_BG, COLOR_ORANGE);
    s_track_label = lv_label_create(s_track_panel);
    lv_obj_set_width(s_track_label, TRACK_PANEL_W - 12);
    lv_label_set_long_mode(s_track_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_line_space(s_track_label, 0, 0);
    lv_obj_set_style_text_color(s_track_label, lv_color_hex(COLOR_ORANGE), 0);
    /* Montserrat 无 CJK；曲名来自 Apple Music，统一用子集汉字点阵。 */
    lv_obj_set_style_text_font(s_track_label, &font_zh_16, 0);
    lv_obj_set_style_text_align(s_track_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_track_label, "—");
    lv_obj_center(s_track_label);

    eva_player_frame_control_bar(s_scr);
    make_button(s_scr, 0, &eva_text_btn_prev, 5);
    make_button(s_scr, 1, &eva_text_btn_play, 64);
    make_button(s_scr, 2, &eva_text_btn_next, 123);
    make_button(s_scr, 3, &eva_text_btn_menu, 182);
    panel(s_scr, 3, 313, 234, 2, COLOR_GREEN, COLOR_GREEN);

    s_built = true;
}

/* 隐藏即销毁：LVGL 内置堆放不下音乐屏 + 编译屏 + i4 屏同时常驻（会在 lv_array_resize 断言卡死 → 白屏）。 */
static void destroy_screen(void)
{
    if (!s_built) return;
    lv_obj_delete(s_scr);
    s_scr = NULL;
    s_clock = NULL;
    s_battery_image = NULL;
    memset(s_buttons, 0, sizeof(s_buttons));
    memset(s_button_icons, 0, sizeof(s_button_icons));
    s_track_panel = NULL;
    s_track_label = NULL;
    s_playlist_title_panel = NULL;
    s_playlist_title = NULL;
    memset(s_playlist_rows, 0, sizeof(s_playlist_rows));
    memset(s_playlist_row_labels, 0, sizeof(s_playlist_row_labels));
    s_last_list_pick = UINT16_MAX;
    s_last_list_window = UINT16_MAX;
    memset(s_last_list_rows, 0, sizeof(s_last_list_rows));
    s_last_list_title[0] = '\0';
    s_last_title[0] = '\0';
    s_last_artist[0] = '\0';
    s_last_drawn_remaining = UINT32_MAX;
    s_last_running = UINT32_MAX;
    s_last_toggle_running = UINT32_MAX;
    s_battery_bars = -1;
    s_built = false;
}

void buddy_eva_music_init(void)
{
    /* 按需创建：真正显示时才 build_screen()。 */
}

bool buddy_eva_music_active(void)
{
    return s_visible;
}

void buddy_eva_music_register_i4_screen(lv_obj_t *screen)
{
    s_i4_screen = screen;
}

void buddy_eva_music_set_visible(bool visible)
{
    if (visible == s_visible) return;
    s_visible = visible;
    if (visible) {
        build_screen();
        lv_screen_load(s_scr);
        eva_player_frame_log_mem("music shown");
    } else {
        if (s_i4_screen) {
            lv_screen_load(s_i4_screen);
        }
        destroy_screen();
    }
    /* 计时器缓冲与编译页共用：重新显示时必须整幅重绘。 */
    s_last_drawn_remaining = UINT32_MAX;
    s_last_running = UINT32_MAX;
}

void buddy_eva_music_update(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    static uint32_t s_last_wait;
    if (!snapshot || !s_visible || !s_built) return;

    if (snapshot->music_playlist_open) {
        set_now_playing_visible(false);
        update_playlist_list(snapshot);
    } else {
        set_now_playing_visible(true);
        memset(s_last_list_rows, 0, sizeof(s_last_list_rows));
        s_last_list_title[0] = '\0';
        s_last_list_pick = UINT16_MAX;

        if (snapshot->waiting != s_last_wait) {
            s_last_wait = snapshot->waiting;
            sync_position_anchor(snapshot, elapsed_ms);
        } else if (!snapshot->running) {
            sync_position_anchor(snapshot, elapsed_ms);
        }

        update_track(snapshot, elapsed_ms);
        draw_clock(snapshot, elapsed_ms);
    }

    update_battery(snapshot);
    update_play_toggle_label(snapshot);
    update_button_styles(snapshot->music_control_focus);
}
