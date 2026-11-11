#pragma once

#include "buddy_state.h"

#ifdef __cplusplus
extern "C" {
#endif

bool buddy_macboard_owner_is_companion(const char *owner);
/* heartbeat 的 page 是否属于板子当前页（music 页兼收 playlist 并打开播放列表）；从不切页 */
bool buddy_macboard_heartbeat_for_page(buddy_state_t *state, const char *page_wire);
buddy_page_t buddy_macboard_page_from_setting(uint8_t value);
uint8_t buddy_macboard_page_to_setting(buddy_page_t page);
/* 开机 / 认领为 MacBoard 时回到 NVS 记住的页（默认音乐） */
void buddy_macboard_restore_page(buddy_state_t *state);
bool buddy_macboard_handle_key(buddy_state_t *state, buddy_key_t key, buddy_action_t *action);

/* 编译页：Mac heartbeat `msg` 状态归类（idle/no-xcode/unknown → IDLE）。 */
typedef enum {
    BUDDY_BUILD_PHASE_IDLE = 0,
    BUDDY_BUILD_PHASE_ACTIVE,  /* starting / busy */
    BUDDY_BUILD_PHASE_SUCCESS,
    BUDDY_BUILD_PHASE_FAILURE, /* failure / no-xcode */
} buddy_build_phase_t;

buddy_build_phase_t buddy_macboard_build_phase(const char *status);

/* 长按 DOWN：音乐 → 编译 → 音量 → 音乐 循环切页，并上行 {"cmd":"page","action":"<page>"}
 * 让 Mac 跟随；处理了返回 true。 */
#define BUDDY_MACBOARD_PAGE_REPORT_MS 2000ULL
bool buddy_macboard_handle_long_key(buddy_state_t *state, buddy_key_t key, uint64_t now_ms,
                                    buddy_action_t *action);
const char *buddy_macboard_page_wire(buddy_page_t page);
/* 上行 {"cmd":"page","action":"<当前页>"}（每 2s 最多一次）：Mac 推错页的数据时让它跟上 */
void buddy_macboard_report_page(buddy_state_t *state, uint64_t now_ms, buddy_action_t *action);

/* 编译结果音效（heartbeat 的 bres / bid，任何页面都带）。 */
enum {
    BUDDY_BUILD_SOUND_NONE = 0,
    BUDDY_BUILD_SOUND_SUCCESS = 1,
    BUDDY_BUILD_SOUND_FAILURE = 2,
};

/* 边沿触发：bid 变化且已 armed 时置 build_sound_pending；开机 / 重连后首个 heartbeat 只记基线。 */
void buddy_macboard_track_build_result(buddy_state_t *state, const buddy_heartbeat_t *heartbeat);

#ifdef __cplusplus
}
#endif
