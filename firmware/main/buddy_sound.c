#include "buddy_sound.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "bsp_audio.h"
#include "build_sounds.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "buddy_sound";
static bool s_ready;
/* 音量档位 0 / 50 / 100 → ES8311 codec 输出音量：0 = 静音（连播放都跳过），
 * 50 = 72（原来固定的响度），100 = 100（codec 满量程）。 */
static volatile uint8_t s_volume = 50;

static uint8_t buddy_sound_codec_volume(uint8_t level)
{
    if (level == 0U) {
        return 0U;
    }
    return level >= 100U ? 100U : (uint8_t)(level <= 50U ? (level * 72U) / 50U
                                                         : 72U + ((level - 50U) * 28U) / 50U);
}

/* 所有提示音都交给独立的低优先级任务播放：bsp_audio_write 阻塞，不能卡住 UI / BLE 主循环。 */
typedef enum {
    BUDDY_CUE_ATTENTION,
    BUDDY_CUE_CLICK,
    BUDDY_CUE_BUILD_SUCCESS,
    BUDDY_CUE_BUILD_FAILURE,
} buddy_cue_t;

static QueueHandle_t s_cue_queue;
static void buddy_sound_task(void *arg);

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
    bsp_audio_set_volume(buddy_sound_codec_volume(s_volume));
    s_cue_queue = xQueueCreate(4, sizeof(buddy_cue_t));
    if (s_cue_queue == NULL ||
        xTaskCreate(buddy_sound_task, "buddy_sound", 3072, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "sound task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "sound cues ready");
    return ESP_OK;
}

static void buddy_sound_attention_now(void)
{
    buddy_sound_tone(880, 90, 12000);
    {
        int16_t zeros[280];
        memset(zeros, 0, sizeof(zeros));
        bsp_audio_write(zeros, sizeof(zeros));
    }
    buddy_sound_tone(1175, 110, 14000);
}

static void buddy_sound_click_now(void)
{
    buddy_sound_tone(1400, 45, 9000);
}

/* IMA-ADPCM（16 kHz 单声道，低 nibble 在前）流式解码播放，见 sounds/convert_sounds.py。 */
static const int16_t s_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t s_ima_index[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static void buddy_sound_play_adpcm(const uint8_t *data, uint32_t samples)
{
    int16_t buf[256];
    int32_t pred = 0;
    int index = 0;
    uint32_t i = 0;
    uint32_t n = 0;

    while (i < samples) {
        const uint8_t code = (uint8_t)((data[i >> 1] >> ((i & 1U) * 4U)) & 0x0FU);
        const int32_t step = s_ima_step[index];
        int32_t delta = step >> 3;

        if (code & 4U) {
            delta += step;
        }
        if (code & 2U) {
            delta += step >> 1;
        }
        if (code & 1U) {
            delta += step >> 2;
        }
        pred += (code & 8U) ? -delta : delta;
        if (pred > 32767) {
            pred = 32767;
        } else if (pred < -32768) {
            pred = -32768;
        }
        index += s_ima_index[code & 7U];
        if (index < 0) {
            index = 0;
        } else if (index > 88) {
            index = 88;
        }
        buf[n++] = (int16_t)pred;
        ++i;
        if (n == sizeof(buf) / sizeof(buf[0]) || i == samples) {
            if (bsp_audio_write(buf, n * sizeof(int16_t)) != ESP_OK) {
                return;
            }
            n = 0;
        }
    }
    {
        /* 尾部补静音，避免 DMA 里残留最后一段被重复播放 */
        int16_t zeros[256];

        memset(zeros, 0, sizeof(zeros));
        bsp_audio_write(zeros, sizeof(zeros));
    }
}

static void buddy_sound_task(void *arg)
{
    buddy_cue_t cue;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_cue_queue, &cue, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cue) {
        case BUDDY_CUE_ATTENTION:
            buddy_sound_attention_now();
            break;
        case BUDDY_CUE_CLICK:
            buddy_sound_click_now();
            break;
        case BUDDY_CUE_BUILD_SUCCESS:
            ESP_LOGI(TAG, "play build_success clip (%u samples)", (unsigned)build_success_samples);
            buddy_sound_play_adpcm(build_success_adpcm, build_success_samples);
            ESP_LOGI(TAG, "build_success clip done");
            break;
        case BUDDY_CUE_BUILD_FAILURE:
            ESP_LOGI(TAG, "play build_failure clip (%u samples)", (unsigned)build_failure_samples);
            buddy_sound_play_adpcm(build_failure_adpcm, build_failure_samples);
            ESP_LOGI(TAG, "build_failure clip done");
            break;
        }
    }
}

void buddy_sound_set_volume(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    s_volume = percent;
    if (s_ready) {
        bsp_audio_set_volume(buddy_sound_codec_volume(percent));
    }
}

uint8_t buddy_sound_volume(void)
{
    return s_volume;
}

static void buddy_sound_post(buddy_cue_t cue)
{
    if (!s_ready || s_cue_queue == NULL || s_volume == 0U) {
        return;
    }
    if (xQueueSend(s_cue_queue, &cue, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cue %d dropped (queue full)", (int)cue);
    }
}

void buddy_sound_play_attention(void)
{
    buddy_sound_post(BUDDY_CUE_ATTENTION);
}

void buddy_sound_play_click(void)
{
    buddy_sound_post(BUDDY_CUE_CLICK);
}

void buddy_sound_play_build_success(void)
{
    buddy_sound_post(BUDDY_CUE_BUILD_SUCCESS);
}

void buddy_sound_play_build_failure(void)
{
    buddy_sound_post(BUDDY_CUE_BUILD_FAILURE);
}
