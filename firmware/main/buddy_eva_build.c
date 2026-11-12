/*
 * MacBoard 编译页：
 *
 *   上部   纯文本编译日志（黑底、无边框），最新一行在最下面，能放几行放几行。
 *          heartbeat 只带最近 8 条 entries，这里在本地按重叠合并成更长的滚动历史。
 *   底部   EVA 控制条 + 音乐页同款居中 PLAY 按钮：OK = 开始编译；编译中置灰。
 *
 * 显示时创建、隐藏即销毁：LVGL 内置堆只够一个 EVA 屏常驻。日志历史在静态内存，跨页面保留。
 */
#include "buddy_eva_build.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "buddy_macboard.h"
#include "eva_player_frame.h"
#include "eva_text_buttons.h"
#include "lvgl.h"

#define COLOR_BG EVA_FRAME_BG
#define COLOR_ORANGE EVA_FRAME_ORANGE
#define COLOR_GREEN EVA_FRAME_GREEN
#define COLOR_RED 0xFF3359U

/* 日志区：y=4 到控制条 y=258 之间，每行 18px（font_zh_16）。 */
#define LOG_TOP 4
#define LOG_LINE_H 18
#define LOG_ROWS ((EVA_FRAME_CTRL_TOP - LOG_TOP) / LOG_LINE_H)
#define LOG_X 6
#define LOG_W 228
/* PLAY 按钮居中 */
#define PLAY_X ((240 - EVA_FRAME_BUTTON_W) / 2)

LV_FONT_DECLARE(font_zh_16);

static lv_obj_t *s_scr;
static lv_obj_t *s_i4_screen;
static bool s_visible;
static bool s_built;

static lv_obj_t *s_play_button;
static lv_obj_t *s_play_icon;
static int s_last_play_enabled = -1;

static lv_obj_t *s_log_labels[LOG_ROWS];
static char s_last_log[LOG_ROWS][BUDDY_ENTRY_MAX];
static uint32_t s_last_log_color[LOG_ROWS];

/* 本地日志历史（静态内存，不占 LVGL 堆）。 */
static char s_history[LOG_ROWS][BUDDY_ENTRY_MAX];
static int s_history_count;

/* ---------- 文本净化：字体里没有的字形换成 '?'，避免出现豆腐块 ---------- */

static uint32_t utf8_next(const char *s, size_t *i)
{
    const unsigned char *p = (const unsigned char *)s + *i;
    uint32_t cp;
    int extra;

    if (p[0] < 0x80) { cp = p[0]; extra = 0; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else { (*i)++; return '?'; }
    for (int k = 1; k <= extra; k++) {
        if ((p[k] & 0xC0) != 0x80) { *i += (size_t)k; return '?'; }
        cp = (cp << 6) | (p[k] & 0x3F);
    }
    *i += (size_t)extra + 1U;
    return cp;
}

static void sanitize_line(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    size_t o = 0;
    lv_font_glyph_dsc_t dsc;

    while (src[i] != '\0' && o + 1 < size) {
        size_t start = i;
        uint32_t cp = utf8_next(src, &i);
        size_t len = i - start;
        if (cp == '\t') cp = ' ';
        if (cp < 0x20 || !lv_font_get_glyph_dsc(&font_zh_16, &dsc, cp, 0)) {
            dst[o++] = '?';
            continue;
        }
        if (cp < 0x80) {
            dst[o++] = (char)cp;
        } else {
            if (o + len >= size) break;
            memcpy(dst + o, src + start, len);
            o += len;
        }
    }
    dst[o] = '\0';
}

/* ---------- 日志历史合并 ---------- */

static bool history_matches(int start, const char (*entries)[BUDDY_ENTRY_MAX], int k)
{
    for (int j = 0; j < k; j++) {
        if (strcmp(s_history[start + j], entries[j]) != 0) return false;
    }
    return true;
}

static void history_push(const char *line)
{
    if (s_history_count == LOG_ROWS) {
        memmove(s_history[0], s_history[1], sizeof(s_history[0]) * (LOG_ROWS - 1));
        s_history_count--;
    }
    snprintf(s_history[s_history_count], sizeof(s_history[0]), "%s", line);
    s_history_count++;
}

/* entries = Mac 发来的最近 N 行（时间线滑动窗口 / 失败时的错误行）。
 * 找历史末尾与 entries 开头的最长重叠，只追加新行；新一轮编译（"00:00 " 开头且无重叠）清空历史。 */
static void merge_entries(const buddy_ui_snapshot_t *snapshot)
{
    char clean[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
    int n = 0;
    int overlap = 0;

    for (int i = 0; i < BUDDY_ENTRY_COUNT; i++) {
        if (snapshot->entries[i][0] == '\0') continue;
        sanitize_line(clean[n], sizeof(clean[n]), snapshot->entries[i]);
        n++;
    }
    if (n == 0) return;

    for (int k = (n < s_history_count ? n : s_history_count); k > 0; k--) {
        if (history_matches(s_history_count - k, (const char (*)[BUDDY_ENTRY_MAX])clean, k)) {
            overlap = k;
            break;
        }
    }
    if (overlap == 0 && strncmp(clean[0], "00:00 ", 6) == 0) {
        s_history_count = 0;
    }
    for (int i = overlap; i < n; i++) {
        history_push(clean[i]);
    }
}

/* ---------- 画面 ---------- */

static void set_row(int row, const char *text, uint32_t color)
{
    if (strcmp(text, s_last_log[row]) != 0) {
        snprintf(s_last_log[row], sizeof(s_last_log[row]), "%s", text);
        lv_label_set_text(s_log_labels[row], text);
    }
    if (color != s_last_log_color[row]) {
        s_last_log_color[row] = color;
        lv_obj_set_style_text_color(s_log_labels[row], lv_color_hex(color), 0);
    }
}

static void update_log(const buddy_ui_snapshot_t *snapshot)
{
    merge_entries(snapshot);

    if (s_history_count == 0) {
        /* 还没有日志：最下面一行显示状态 + 提示。 */
        char placeholder[BUDDY_ENTRY_MAX];
        const char *status = snapshot->message;
        if (buddy_macboard_build_phase(status) == BUDDY_BUILD_PHASE_IDLE &&
            (status == NULL || strcmp(status, "idle") != 0)) {
            /* 刚从音乐页切过来时 message 里可能还留着歌名，不要显示在编译页。 */
            status = "idle";
        }
        if (status == NULL || status[0] == '\0') {
            status = "idle";
        }
        size_t i = 0;
        for (; status[i] != '\0' && i + 12 < sizeof(placeholder); i++) {
            placeholder[i] = (char)toupper((unsigned char)status[i]);
        }
        snprintf(placeholder + i, sizeof(placeholder) - i, "  OK:PLAY");
        for (int row = 0; row < LOG_ROWS; row++) {
            set_row(row, row == LOG_ROWS - 1 ? placeholder : "", COLOR_ORANGE);
        }
        return;
    }

    /* 底部对齐：最新一行在最下面。 */
    for (int row = 0; row < LOG_ROWS; row++) {
        int idx = s_history_count - LOG_ROWS + row;
        const char *line = idx >= 0 ? s_history[idx] : "";
        uint32_t color = strstr(line, "error") != NULL ? COLOR_RED : COLOR_ORANGE;
        set_row(row, line, color);
    }
}

static void update_play_button(const buddy_ui_snapshot_t *snapshot)
{
    int enabled = buddy_macboard_build_phase(snapshot->message) == BUDDY_BUILD_PHASE_ACTIVE ? 0 : 1;
    if (enabled == s_last_play_enabled) return;
    s_last_play_enabled = enabled;
    /* 与音乐页焦点按钮同款：可按 = 品红底；编译中 = 黑底暗绿（不可按）。 */
    eva_player_frame_button_style(s_play_button, s_play_icon, true, enabled != 0);
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

    /* 纯文本行：label 直接挂在屏幕上，无面板 / 边框 / 底色。 */
    for (int i = 0; i < LOG_ROWS; i++) {
        lv_obj_t *lab = lv_label_create(s_scr);
        lv_obj_set_pos(lab, LOG_X, LOG_TOP + i * LOG_LINE_H);
        lv_obj_set_width(lab, LOG_W);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(lab, lv_color_hex(COLOR_ORANGE), 0);
        lv_obj_set_style_text_font(lab, &font_zh_16, 0);
        lv_label_set_text(lab, "");
        s_log_labels[i] = lab;
        s_last_log[i][0] = '\0';
        s_last_log_color[i] = COLOR_ORANGE;
    }

    /* 控制条：音乐页同款 PLAY 按钮 */
    eva_player_frame_control_bar(s_scr);
    s_play_button = eva_player_frame_button(s_scr, PLAY_X, &eva_text_btn_play, &s_play_icon);
    eva_player_frame_panel(s_scr, 3, 313, 234, 2, COLOR_GREEN, COLOR_GREEN);

    s_built = true;
}

static void destroy_screen(void)
{
    if (!s_built) return;
    lv_obj_delete(s_scr);
    s_scr = NULL;
    s_play_button = NULL;
    s_play_icon = NULL;
    s_last_play_enabled = -1;
    memset(s_log_labels, 0, sizeof(s_log_labels));
    memset(s_last_log, 0, sizeof(s_last_log));
    s_built = false;
}

void buddy_eva_build_init(void)
{
    /* 按需创建：真正显示时才 build_screen()。 */
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
    if (visible == s_visible) {
        return;
    }
    s_visible = visible;
    if (visible) {
        build_screen();
        lv_screen_load(s_scr);
        eva_player_frame_log_mem("build shown");
    } else {
        if (s_i4_screen) {
            lv_screen_load(s_i4_screen);
        }
        destroy_screen();
    }
}

void buddy_eva_build_update(const buddy_ui_snapshot_t *snapshot, uint64_t elapsed_ms)
{
    (void)elapsed_ms;
    if (!snapshot || !s_visible || !s_built) {
        return;
    }
    update_log(snapshot);
    update_play_button(snapshot);
}
