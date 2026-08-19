#pragma once

#include <common/bk_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bk_err_t audio_http_stream_start(uint32_t sample_rate, uint32_t channels);
bk_err_t audio_http_stream_stop(void);
bk_err_t audio_http_stream_push_frame(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
