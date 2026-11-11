#include "buddy_macboard.h"

#include <stdio.h>
#include <string.h>

bool buddy_macboard_owner_is_companion(const char *owner)
{
    return owner != NULL && strcmp(owner, "MacBoard") == 0;
}

/* 板子是当前页的唯一来源：Mac heartbeat 的 page 只用来判断这份数据是不是给当前页的，
 * 从不切页。返回 true = 数据属于当前页（可以显示）。 */
bool buddy_macboard_heartbeat_for_page(buddy_state_t *state, const char *page_wire)
{
    if (state == NULL || page_wire == NULL) {
        return false;
    }
    if (state->page == BUDDY_PAGE_MUSIC) {
        if (strcmp(page_wire, "playlist") == 0) {
            state->music_playlist_open = true;
            return true;
        }
        if (strcmp(page_wire, "music") == 0) {
            if (!state->music_playlist_open) {
                state->music_playlist_pick_armed = false;
            }
            return true;
        }
        return false;
    }
    if (state->page == BUDDY_PAGE_BUILD) {
        return strcmp(page_wire, "build") == 0;
    }
    if (state->page == BUDDY_PAGE_VOLUME) {
        return strcmp(page_wire, "volume") == 0;
    }
    return false;
}

buddy_page_t buddy_macboard_page_from_setting(uint8_t value)
{
    if (value == BUDDY_MACBOARD_PAGE_BUILD) return BUDDY_PAGE_BUILD;
    if (value == BUDDY_MACBOARD_PAGE_VOLUME) return BUDDY_PAGE_VOLUME;
    return BUDDY_PAGE_MUSIC;
}

uint8_t buddy_macboard_page_to_setting(buddy_page_t page)
{
    if (page == BUDDY_PAGE_BUILD) return BUDDY_MACBOARD_PAGE_BUILD;
    if (page == BUDDY_PAGE_VOLUME) return BUDDY_MACBOARD_PAGE_VOLUME;
    return BUDDY_MACBOARD_PAGE_MUSIC;
}

void buddy_macboard_restore_page(buddy_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->page = buddy_macboard_page_from_setting(state->settings.macboard_page);
    state->volume_pick = buddy_volume_snap(state->settings.volume);
}

static void buddy_macboard_queue_uplink(buddy_action_t *action, const char *cmd,
                                        const char *uplink_action)
{
    if (action == NULL) {
        return;
    }
    action->type = BUDDY_ACTION_MACBOARD_UPLINK;
    strncpy(action->macboard_cmd, cmd, sizeof(action->macboard_cmd) - 1U);
    strncpy(action->macboard_action, uplink_action, sizeof(action->macboard_action) - 1U);
    action->macboard_cmd[sizeof(action->macboard_cmd) - 1U] = '\0';
    action->macboard_action[sizeof(action->macboard_action) - 1U] = '\0';
}

static void buddy_macboard_music_focus_step(buddy_state_t *state, int delta)
{
    int next = (int)state->music_control_focus + delta;
    if (next < 0) {
        next = 3;
    } else if (next > 3) {
        next = 0;
    }
    state->music_control_focus = (uint8_t)next;
}

static uint16_t buddy_macboard_playlist_count(const buddy_state_t *state)
{
    if (state == NULL || state->total == 0U) {
        return 0U;
    }
    return (uint16_t)state->total;
}

static void buddy_macboard_playlist_window_uplink(buddy_state_t *state, buddy_action_t *action,
                                                uint16_t start)
{
    char wire[32];

    if (state == NULL || action == NULL) {
        return;
    }
    snprintf(wire, sizeof(wire), "window:%u", (unsigned)start);
    buddy_macboard_queue_uplink(action, "media", wire);
    state->music_playlist_window = start;
}

static void buddy_macboard_playlist_sync_window(buddy_state_t *state, buddy_action_t *action)
{
    uint16_t count = buddy_macboard_playlist_count(state);
    uint16_t start = state->music_playlist_window;
    uint16_t pick = state->music_playlist_pick;

    if (count == 0U) {
        return;
    }
    if (pick >= count) {
        pick = (uint16_t)(count - 1U);
        state->music_playlist_pick = pick;
    }
    if (pick < start) {
        start = pick;
    } else if (pick >= start + BUDDY_ENTRY_COUNT) {
        if (pick + 1U >= BUDDY_ENTRY_COUNT) {
            start = (uint16_t)(pick + 1U - BUDDY_ENTRY_COUNT);
        } else {
            start = 0U;
        }
    }
    if (start != state->music_playlist_window) {
        buddy_macboard_playlist_window_uplink(state, action, start);
    }
}

static void buddy_macboard_playlist_open(buddy_state_t *state, buddy_action_t *action)
{
    state->music_playlist_open = true;
    state->music_playlist_pick_armed = false;
    state->music_playlist_window = 0U;
    /* 从 MENU 进入，保持 MENU 高亮，便于再按 OK 直接退出列表。 */
    state->music_control_focus = 3U;
    buddy_macboard_queue_uplink(action, "media", "playlist_open");
}

static void buddy_macboard_playlist_close(buddy_state_t *state, buddy_action_t *action)
{
    state->music_playlist_open = false;
    state->music_playlist_pick_armed = false;
    buddy_macboard_queue_uplink(action, "media", "playlist_close");
}

static void buddy_macboard_playlist_pick(buddy_state_t *state, buddy_action_t *action)
{
    char wire[32];

    snprintf(wire, sizeof(wire), "pick:%u", (unsigned)state->music_playlist_pick);
    buddy_macboard_queue_uplink(action, "media", wire);
    state->music_playlist_open = false;
    state->music_playlist_pick_armed = false;
}

static bool buddy_macboard_playlist_key(buddy_state_t *state, buddy_key_t key,
                                        buddy_action_t *action)
{
    if (key == BUDDY_KEY_UP) {
        if (state->music_playlist_pick > 0U) {
            state->music_playlist_pick--;
            state->music_control_focus = 1U;
            buddy_macboard_playlist_sync_window(state, action);
        }
        return true;
    }
    if (key == BUDDY_KEY_DOWN) {
        uint16_t count = buddy_macboard_playlist_count(state);
        if (count > 0U && state->music_playlist_pick + 1U < count) {
            state->music_playlist_pick++;
            state->music_control_focus = 1U;
            buddy_macboard_playlist_sync_window(state, action);
        }
        return true;
    }
    if (key == BUDDY_KEY_OK) {
        if (action == NULL) {
            return false;
        }
        if (state->music_control_focus == 3U) {
            buddy_macboard_playlist_close(state, action);
            return action->type == BUDDY_ACTION_MACBOARD_UPLINK;
        }
        buddy_macboard_playlist_pick(state, action);
        return action->type == BUDDY_ACTION_MACBOARD_UPLINK;
    }
    return false;
}

static void buddy_macboard_music_ok(buddy_state_t *state, buddy_action_t *action)
{
    switch (state->music_control_focus) {
    case 0:
        buddy_macboard_queue_uplink(action, "media", "previous");
        break;
    case 1:
        state->music_ui_playing = !state->music_ui_playing;
        buddy_macboard_queue_uplink(action, "media", "toggle");
        break;
    case 2:
        buddy_macboard_queue_uplink(action, "media", "next");
        break;
    case 3:
        if (state->music_playlist_open) {
            buddy_macboard_playlist_close(state, action);
        } else {
            buddy_macboard_playlist_open(state, action);
        }
        break;
    default:
        break;
    }
}

bool buddy_macboard_handle_key(buddy_state_t *state, buddy_key_t key, buddy_action_t *action)
{
    if (state == NULL || !state->macboard_companion) {
        return false;
    }
    if (state->page == BUDDY_PAGE_MUSIC) {
        if (state->music_playlist_open) {
            return buddy_macboard_playlist_key(state, key, action);
        }
        if (key == BUDDY_KEY_UP) {
            buddy_macboard_music_focus_step(state, -1);
            return true;
        }
        if (key == BUDDY_KEY_DOWN) {
            buddy_macboard_music_focus_step(state, 1);
            return true;
        }
        if (key == BUDDY_KEY_OK) {
            if (action == NULL) {
                return false;
            }
            buddy_macboard_music_ok(state, action);
            return action->type == BUDDY_ACTION_MACBOARD_UPLINK;
        }
        return false;
    }
    if (state->page == BUDDY_PAGE_VOLUME) {
        if (key == BUDDY_KEY_UP) {
            state->volume_pick = state->volume_pick >= 100U ? 0U : (uint8_t)(state->volume_pick + 50U);
            return true;
        }
        if (key == BUDDY_KEY_DOWN) {
            state->volume_pick = state->volume_pick == 0U ? 100U : (uint8_t)(state->volume_pick - 50U);
            return true;
        }
        if (key == BUDDY_KEY_OK) {
            /* 确认：main 负责应用 codec、值变化才写 NVS、按新音量试听；停留在音量页 */
            state->settings.volume = state->volume_pick;
            state->volume_commit_pending = true;
            return true;
        }
        return false;
    }
    if (state->page == BUDDY_PAGE_BUILD) {
        if (key == BUDDY_KEY_OK) {
            /* OK = EVA PLAY 按钮。编译进行中（且 Mac 心跳未过期）不重复触发 ⌘B。 */
            if (state->running != 0U && !state->heartbeat_stale &&
                buddy_macboard_build_phase(state->message) == BUDDY_BUILD_PHASE_ACTIVE) {
                return true;
            }
            buddy_macboard_queue_uplink(action, "build", "start");
            return true;
        }
        if (key == BUDDY_KEY_UP || key == BUDDY_KEY_DOWN) {
            if (action != NULL) {
                action->type = BUDDY_ACTION_UI_SCROLL;
                action->scroll_delta = key == BUDDY_KEY_DOWN ? 24 : -24;
            }
            return true;
        }
    } else {
        state->page = BUDDY_PAGE_MUSIC;
        return true;
    }
    return false;
}

buddy_build_phase_t buddy_macboard_build_phase(const char *status)
{
    if (status == NULL || status[0] == '\0') {
        return BUDDY_BUILD_PHASE_IDLE;
    }
    if (strcmp(status, "busy") == 0 || strcmp(status, "starting") == 0) {
        return BUDDY_BUILD_PHASE_ACTIVE;
    }
    if (strcmp(status, "success") == 0) {
        return BUDDY_BUILD_PHASE_SUCCESS;
    }
    if (strcmp(status, "failure") == 0 || strcmp(status, "no-xcode") == 0) {
        return BUDDY_BUILD_PHASE_FAILURE;
    }
    return BUDDY_BUILD_PHASE_IDLE;
}

void buddy_macboard_track_build_result(buddy_state_t *state, const buddy_heartbeat_t *heartbeat)
{
    uint8_t sound = BUDDY_BUILD_SOUND_NONE;

    if (state == NULL || heartbeat == NULL) {
        return;
    }
    if (strcmp(heartbeat->build_result, "success") == 0) {
        sound = BUDDY_BUILD_SOUND_SUCCESS;
    } else if (strcmp(heartbeat->build_result, "failure") == 0) {
        sound = BUDDY_BUILD_SOUND_FAILURE;
    }
    if (sound == BUDDY_BUILD_SOUND_NONE || heartbeat->build_id == 0U) {
        /* 没有已结束的编译（Mac 刚启动 / 编译中）：之后出现的第一个结果就是新结果 */
        state->build_sound_armed = true;
        return;
    }
    if (state->build_sound_armed && heartbeat->build_id != state->build_sound_id) {
        state->build_sound_pending = sound;
    }
    state->build_sound_id = heartbeat->build_id;
    state->build_sound_armed = true;
}

void buddy_macboard_report_page(buddy_state_t *state, uint64_t now_ms, buddy_action_t *action)
{
    if (state == NULL || now_ms < state->page_report_after_ms) {
        return;
    }
    state->page_report_after_ms = now_ms + BUDDY_MACBOARD_PAGE_REPORT_MS;
    buddy_macboard_queue_uplink(action, "page", buddy_macboard_page_wire(state->page));
}

const char *buddy_macboard_page_wire(buddy_page_t page)
{
    switch (page) {
    case BUDDY_PAGE_BUILD:
        return "build";
    case BUDDY_PAGE_VOLUME:
        return "volume";
    default:
        return "music";
    }
}

bool buddy_macboard_handle_long_key(buddy_state_t *state, buddy_key_t key, uint64_t now_ms,
                                    buddy_action_t *action)
{
    buddy_page_t next;

    if (state == NULL || !state->macboard_companion || state->menu_open ||
        key != BUDDY_KEY_DOWN) {
        return false;
    }
    /* 长按 DOWN：音乐 → 编译 → 音量 → 音乐 循环（播放列表打开时不切） */
    if (state->page == BUDDY_PAGE_MUSIC && !state->music_playlist_open) {
        next = BUDDY_PAGE_BUILD;
    } else if (state->page == BUDDY_PAGE_BUILD) {
        next = BUDDY_PAGE_VOLUME;
    } else if (state->page == BUDDY_PAGE_VOLUME) {
        next = BUDDY_PAGE_MUSIC;
    } else {
        return false;
    }
    if (next == BUDDY_PAGE_VOLUME) {
        state->volume_pick = buddy_volume_snap(state->settings.volume);
    }
    state->page = next;
    /* 告诉 Mac 板子选了哪页（Mac 跟随，之后推送的就是这一页的数据）；main 负责写 NVS */
    state->page_report_after_ms = now_ms + BUDDY_MACBOARD_PAGE_REPORT_MS;
    buddy_macboard_queue_uplink(action, "page", buddy_macboard_page_wire(next));
    return true;
}
