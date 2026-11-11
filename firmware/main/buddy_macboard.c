#include "buddy_macboard.h"

#include <stdio.h>
#include <string.h>

bool buddy_macboard_owner_is_companion(const char *owner)
{
    return owner != NULL && strcmp(owner, "MacBoard") == 0;
}

void buddy_macboard_apply_page_wire(buddy_state_t *state, const char *page_wire)
{
    if (state == NULL || page_wire == NULL) {
        return;
    }
    if (strcmp(page_wire, "music") == 0) {
        state->page = BUDDY_PAGE_MUSIC;
        if (!state->music_playlist_open) {
            state->music_playlist_pick_armed = false;
        }
    } else if (strcmp(page_wire, "build") == 0) {
        state->page = BUDDY_PAGE_BUILD;
    } else if (strcmp(page_wire, "playlist") == 0) {
        state->page = BUDDY_PAGE_MUSIC;
        state->music_playlist_open = true;
    }
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
    if (state->menu_open || state->page == BUDDY_PAGE_SETTINGS) {
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
    if (state->page == BUDDY_PAGE_BUILD) {
        if (key == BUDDY_KEY_OK) {
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
