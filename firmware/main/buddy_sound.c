#include "buddy_sound.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "buddy_sound";
static bool s_ready;

static void buddy_sound_tone(uint16_t hz, uint16_t ms, uint16_t amplitude)
{
    enum { RATE = 16000 };
    int16_t buf[256];
    uint32_t total = ((uint32_t)RATE * (uint32_t)ms) / 1000U;
    uint32_t i = 0;

    if (!s_ready || hz == 0 || ms == 0) {
        return;
    }
    while (i < total) {
        uint32_t n = total - i;
        uint32_t k;
        if (n > (uint32_t)(sizeof(buf) / sizeof(buf[0]))) {
            n = (uint32_t)(sizeof(buf) / sizeof(buf[0]));
        }
        for (k = 0; k < n; ++k) {
            float t = (float)(i + k) / (float)RATE;
            float sample = sinf(2.0f * (float)M_PI * (float)hz * t);
            /* Short fade in/out to avoid clicks. */
            float env = 1.0f;
            if (i + k < 40U) {
                env = (float)(i + k) / 40.0f;
            } else if (i + k + 40U > total) {
                env = (float)(total - (i + k)) / 40.0f;
            }
            buf[k] = (int16_t)(sample * env * (float)amplitude);
        }
        if (bsp_audio_write(buf, n * sizeof(int16_t)) != ESP_OK) {
            return;
        }
        i += n;
    }
}

esp_err_t buddy_sound_init(void)
{
    esp_err_t err;

    if (s_ready) {
        return ESP_OK;
    }
    err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = bsp_audio_set_format(16000, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio format failed: %s", esp_err_to_name(err));
        return err;
    }
    bsp_audio_set_volume(72);
    s_ready = true;
    ESP_LOGI(TAG, "sound cues ready");
    return ESP_OK;
}

void buddy_sound_play_attention(void)
{
    if (!s_ready) {
        return;
    }
    buddy_sound_tone(880, 90, 12000);
    {
        int16_t zeros[280];
        memset(zeros, 0, sizeof(zeros));
        bsp_audio_write(zeros, sizeof(zeros));
    }
    buddy_sound_tone(1175, 110, 14000);
}

void buddy_sound_play_click(void)
{
    if (!s_ready) {
        return;
    }
    buddy_sound_tone(1400, 45, 9000);
}
