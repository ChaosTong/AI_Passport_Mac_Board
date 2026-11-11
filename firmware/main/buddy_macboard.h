#pragma once

#include "buddy_state.h"

#ifdef __cplusplus
extern "C" {
#endif

bool buddy_macboard_owner_is_companion(const char *owner);
void buddy_macboard_apply_page_wire(buddy_state_t *state, const char *page_wire);
bool buddy_macboard_handle_key(buddy_state_t *state, buddy_key_t key, buddy_action_t *action);

#ifdef __cplusplus
}
#endif
