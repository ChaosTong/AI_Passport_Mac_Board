#pragma once

#include "buddy_types.h"

typedef struct {
    buddy_connection_t connection;
    buddy_character_t character;
    buddy_page_t page;
    buddy_heartbeat_t heartbeat;
    buddy_prompt_t prompt;
    buddy_settings_snapshot_t settings;
    char last_attempted_prompt_id[BUDDY_PROMPT_ID_MAX];
    char last_successful_decision_id[BUDDY_PROMPT_ID_MAX];
    char name[BUDDY_NAME_MAX];
    char owner[BUDDY_OWNER_MAX];
    char time[BUDDY_MESSAGE_MAX];
    char message[BUDDY_MESSAGE_MAX];
    char entries[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
    unsigned total;
    unsigned running;
    unsigned waiting;
    uint64_t tokens;
    uint64_t tokens_today;
    int64_t epoch_seconds;
    int32_t timezone_offset_seconds;
    uint64_t time_received_ms;
    uint64_t highest_celebrated_level;
    uint64_t last_heartbeat_ms;
    uint64_t temporary_until_ms;
    uint32_t prompt_connection_generation;
    uint32_t confirmation_connection_generation;
    uint32_t ble_connection_generation;
    buddy_character_t temporary_character;
    bool connected;
    bool heartbeat_stale;
    bool confirmation_pending;
    buddy_confirmation_t confirmation;
    bool confirmation_acknowledge;
    buddy_settings_item_t settings_selection;
    buddy_reset_item_t reset_selection;
    buddy_menu_item_t menu_selection;
    uint8_t pet_page;
    uint8_t info_page;
    bool menu_open;
    bool reset_open;
    bool transcript_enabled;
    bool screen_off;
    uint8_t brightness_level;
    uint8_t species;
    buddy_permission_delivery_t permission_delivery;
    bool approval_locked;
    bool ble_connected;
    bool ble_encrypted;
    bool battery_available;
    bool passkey_visible;
    uint32_t passkey;
    uint8_t battery_percent;
    uint16_t battery_mv;
    bool macboard_companion;
    /** MacBoard 音乐：播停仅本地维护，不用 Mac heartbeat `running` 覆盖。 */
    bool music_ui_playing;
    /** 0–3：PREV / PLAY / NEXT / MENU，UP/DOWN 移动，OK 确认。 */
    uint8_t music_control_focus;
    /** 编译页：0 = PLAY，1 = STOP；UP/DOWN 切换，OK 确认。 */
    uint8_t build_control_focus;
    bool music_playlist_open;
    uint16_t music_playlist_pick;
    uint16_t music_playlist_window;
    bool music_playlist_pick_armed;
    /* 编译结果音效（边沿触发）：build_sound_id 为已处理的编译 id；
     * armed=false 时（开机 / 重连后第一个 heartbeat）只记录基线不出声。
     * build_sound_pending 由 main 消费：0 无 / 1 成功 / 2 失败。 */
    uint64_t build_sound_id;
    bool build_sound_armed;
    uint8_t build_sound_pending;
    /* true = Mac 试听命令触发，忽略 SOUND 开关 */
    bool build_sound_force;
    /* 音量页：高亮档位；OK 确认后由 main 应用 / 持久化 / 试听 */
    uint8_t volume_pick;
    bool volume_commit_pending;
    /* Mac 推来别的页的数据时，板子上报自己当前页（限频，下次允许上报的时间） */
    uint64_t page_report_after_ms;
} buddy_state_t;

void buddy_state_init(buddy_state_t *state, const buddy_settings_snapshot_t *settings);
void buddy_state_reduce(buddy_state_t *state, const buddy_event_t *event,
                        uint64_t now_ms, buddy_action_t *action);
void buddy_state_snapshot(const buddy_state_t *state, buddy_ui_snapshot_t *snapshot);
