#pragma once

#include "lvgl.h"

#define EVA_FRAME_BG 0x000000
#define EVA_FRAME_ORANGE 0xFF8800
#define EVA_FRAME_GREEN 0x5B7337
#define EVA_FRAME_MAGENTA 0xE9004D

lv_obj_t *eva_player_frame_panel(lv_obj_t *parent, int x, int y, int w, int h,
    uint32_t bg, uint32_t border);
lv_obj_t *eva_player_frame_header(lv_obj_t *parent, const char *mode, const lv_font_t *font);
void eva_player_frame_battery(lv_obj_t *image, int bars);

/* 底部控制条（音乐页 / 编译页共用）：y=258 橄榄底 + y=313 细线。 */
#define EVA_FRAME_CTRL_TOP 258
#define EVA_FRAME_BUTTON_Y 266
#define EVA_FRAME_BUTTON_W 52
#define EVA_FRAME_BUTTON_H 30
void eva_player_frame_control_bar(lv_obj_t *parent);
/* 控制条按钮：返回按钮面板，*icon_out 为图标对象（可为 NULL）。 */
lv_obj_t *eva_player_frame_button(lv_obj_t *parent, int x, const lv_image_dsc_t *icon,
    lv_obj_t **icon_out);
/* focused：品红底（音乐页焦点样式）；enabled=false：黑底 + 暗绿图标（不可按）。 */
void eva_player_frame_button_style(lv_obj_t *button, lv_obj_t *icon, bool focused, bool enabled);
int eva_player_frame_battery_bars(unsigned percent);
/* EVA 七段计时器画布的 A8 静态缓冲（音乐页使用）。重新显示后调用方须整幅重绘。 */
lv_draw_buf_t *eva_player_frame_clock_buf(void);
/* LVGL 内置堆只有 CONFIG_LV_MEM_SIZE_KILOBYTES（24KB）：EVA 页按需创建、隐藏即销毁，
 * 同一时刻只保留一个 EVA 屏。这里打印 LVGL 堆余量便于排查。 */
void eva_player_frame_log_mem(const char *what);
