#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <common/bk_err.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DIGITAL_MIC_SAMPLE_RATE_DEFAULT 44100U

typedef struct
{
    uint32_t sample_rate;
    uint32_t frame_ms;
    uint32_t frame_count;
    uint32_t write_timeout_ms;
} digital_mic_config_t;

void digital_mic_init_default_config(digital_mic_config_t *config);
bk_err_t digital_mic_start(const digital_mic_config_t *config);
bk_err_t digital_mic_stop(void);
bool digital_mic_is_running(void);

#ifdef __cplusplus
}
#endif
