#pragma once

#include <stdint.h>

/* MacBoard 编译结果音效：16 kHz 单声道 IMA-ADPCM（见 convert_sounds.py）。 */
extern const uint32_t build_success_samples;
extern const uint8_t build_success_adpcm[];
extern const uint32_t build_failure_samples;
extern const uint8_t build_failure_adpcm[];
