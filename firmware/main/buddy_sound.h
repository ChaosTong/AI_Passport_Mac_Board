#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Init ES8311 codec for short UI cues. Safe to call multiple times. */
esp_err_t buddy_sound_init(void);

/** Two-tone attention chirp (blocking ~200ms). No-op if init failed. */
void buddy_sound_play_attention(void);

/** Soft click when toggling sound on. */
void buddy_sound_play_click(void);
