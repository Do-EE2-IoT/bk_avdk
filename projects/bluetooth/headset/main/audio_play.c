// Copyright 2023-2024 Beken
//
// Licensed under the Apache License, Version 2.0 (the  "License ");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//	   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an  "AS IS " BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stdio.h"
#include <os/os.h>
#include <os/mem.h>

#include <driver/i2s.h>
#include <driver/i2s_types.h>
#include <driver/audio_ring_buff.h>
#include "audio_play.h"

#define TAG "AUD_PLAY_I2S"

float gain = 0.25f;

typedef struct
{
	RingBufferContext *i2s_tx_rb;
	bool started;
	uint8_t channels;
	uint8_t bits_per_sample;
	uint32_t sample_rate;
} audio_pcm_stream_t;

static audio_pcm_stream_t s_audio_pcm_stream = {0};
static uint32_t s_audio_pcm_log_counter = 0;

static bk_err_t audio_play_i2s_hw_start(uint32_t sample_rate, RingBufferContext **tx_rb);
static void audio_play_i2s_hw_stop(void);
static void audio_play_apply_gain(int16_t *samples, uint32_t sample_count);
static void audio_play_log_pcm_chunk(const uint8_t *data, uint32_t size, uint32_t total_written, uint32_t written);

// Convert MP3 sample rate to I2S enum
static i2s_samp_rate_t get_i2s_sample_rate(uint32_t mp3_rate)
{
	switch (mp3_rate)
	{
	case 8000:
		return I2S_SAMP_RATE_8000;
	case 11025:
		return I2S_SAMP_RATE_11025;
	case 12000:
		return I2S_SAMP_RATE_12000;
	case 16000:
		return I2S_SAMP_RATE_16000;
	case 22050:
		return I2S_SAMP_RATE_22050;
	case 24000:
		return I2S_SAMP_RATE_24000;
	case 32000:
		return I2S_SAMP_RATE_32000;
	case 44100:
		return I2S_SAMP_RATE_44100;
	case 48000:
		return I2S_SAMP_RATE_48000;
	case 88200:
		return I2S_SAMP_RATE_88200;
	case 96000:
		return I2S_SAMP_RATE_96000;
	default:
		BK_LOGW(TAG, "Unsupported sample rate %d, using 44100  n", mp3_rate);
		return I2S_SAMP_RATE_44100; // Default fallback
	}
}
bk_err_t audio_play_pcm_i2s_start(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
	bk_err_t ret;

	if ((channels != 2U) || (bits_per_sample != 16U))
	{
		BK_LOGE(TAG, "Unsupported PCM format, channels=%u bits=%u\n", channels, bits_per_sample);
		return BK_ERR_PARAM;
	}

	if (s_audio_pcm_stream.started)
	{
		if ((s_audio_pcm_stream.sample_rate == sample_rate) &&
			(s_audio_pcm_stream.channels == channels) &&
			(s_audio_pcm_stream.bits_per_sample == bits_per_sample))
		{
			return BK_OK;
		}

		audio_play_pcm_i2s_stop();
	}

	ret = audio_play_i2s_hw_start(sample_rate, &s_audio_pcm_stream.i2s_tx_rb);
	if (ret != BK_OK)
	{
		return ret;
	}

	s_audio_pcm_stream.started = true;
	s_audio_pcm_stream.sample_rate = sample_rate;
	s_audio_pcm_stream.channels = channels;
	s_audio_pcm_stream.bits_per_sample = bits_per_sample;

	BK_LOGW(TAG, "PCM I2S stream started, rate=%lu channels=%u bits=%u\n",
			(unsigned long)sample_rate, channels, bits_per_sample);

	return BK_OK;
}

bk_err_t audio_play_pcm_i2s_write(uint8_t *data, uint32_t size, uint32_t timeout_ms)
{
	uint32_t total_written = 0;
	uint32_t waited_ms = 0;

	if ((data == NULL) || (size == 0U))
	{
		return BK_ERR_NULL_PARAM;
	}

	if (!s_audio_pcm_stream.started || (s_audio_pcm_stream.i2s_tx_rb == NULL))
	{
		return BK_ERR_NOT_INIT;
	}

	if ((s_audio_pcm_stream.bits_per_sample == 16U) && ((size % 2U) == 0U))
	{
		audio_play_apply_gain((int16_t *)data, size / 2U);
	}

	// BK_LOGW(TAG, "PCM write request, size=%lu timeout=%lu rate=%lu\n",
	// 		(unsigned long)size,
	// 		(unsigned long)timeout_ms,
	// 		(unsigned long)s_audio_pcm_stream.sample_rate);

	while (total_written < size)
	{
		uint32_t written = ring_buffer_write(s_audio_pcm_stream.i2s_tx_rb,
											 data + total_written,
											 size - total_written);

		//audio_play_log_pcm_chunk(data + total_written, size, total_written, written);

		total_written += written;

		if (total_written >= size)
		{
			return BK_OK;
		}

		if (waited_ms >= timeout_ms)
		{
			BK_LOGW(TAG, "PCM write timeout, dropped %lu bytes\n",
					(unsigned long)(size - total_written));
			return BK_FAIL;
		}

		rtos_delay_milliseconds(2);
		waited_ms += 2U;
	}

	return BK_OK;
}

static void audio_play_log_pcm_chunk(const uint8_t *data, uint32_t size, uint32_t total_written, uint32_t written)
{
	uint32_t pushed_total = total_written + written;

	if ((data == NULL) || (written == 0U))
	{
		return;
	}

	if (((s_audio_pcm_log_counter++ % 32U) != 0U) && (pushed_total < size))
	{
		return;
	}

	if ((s_audio_pcm_stream.bits_per_sample == 16U) && ((written % 2U) == 0U))
	{
		const int16_t *samples = (const int16_t *)data;
		uint32_t sample_count = written / 2U;

		// BK_LOGW(TAG,
		// 		"PCM pushed total=%lu/%lu chunk=%lu samples=%d,%d,%d,%d,%d,%d,%d,%d\n",
		// 		(unsigned long)pushed_total,
		// 		(unsigned long)size,
		// 		(unsigned long)written,
		// 		(sample_count > 0U) ? samples[0] : 0,
		// 		(sample_count > 1U) ? samples[1] : 0,
		// 		(sample_count > 2U) ? samples[2] : 0,
		// 		(sample_count > 3U) ? samples[3] : 0,
		// 		(sample_count > 4U) ? samples[4] : 0,
		// 		(sample_count > 5U) ? samples[5] : 0,
		// 		(sample_count > 6U) ? samples[6] : 0,
		// 		(sample_count > 7U) ? samples[7] : 0);
		return;
	}

	// BK_LOGW(TAG,
	// 		"PCM pushed total=%lu/%lu chunk=%lu bytes=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
	// 		(unsigned long)pushed_total,
	// 		(unsigned long)size,
	// 		(unsigned long)written,
	// 		(written > 0U) ? data[0] : 0,
	// 		(written > 1U) ? data[1] : 0,
	// 		(written > 2U) ? data[2] : 0,
	// 		(written > 3U) ? data[3] : 0,
	// 		(written > 4U) ? data[4] : 0,
	// 		(written > 5U) ? data[5] : 0,
	// 		(written > 6U) ? data[6] : 0,
	// 		(written > 7U) ? data[7] : 0);
}

bk_err_t audio_play_pcm_i2s_stop(void)
{
	if (!s_audio_pcm_stream.started)
	{
		return BK_OK;
	}

	audio_play_i2s_hw_stop();
	os_memset(&s_audio_pcm_stream, 0, sizeof(s_audio_pcm_stream));
	BK_LOGI(TAG, "PCM I2S stream stopped\n");
	return BK_OK;
}

// I2S callback - called by DMA when buffer needs data
// Keep this simple - actual decoding is done in separate thread
static int i2s_tx_data_callback(uint32_t size)
{
	// Just return size - decode thread handles filling the buffer
	// os_printf("i2s tx data send, size = %d \r\n", size);
	return size;
}

static bk_err_t audio_play_i2s_hw_start(uint32_t sample_rate, RingBufferContext **tx_rb)
{
	bk_err_t ret;
	i2s_config_t i2s_config = DEFAULT_I2S_CONFIG();
	i2s_samp_rate_t i2s_rate;

	if (tx_rb == NULL)
	{
		return BK_ERR_NULL_PARAM;
	}

	ret = bk_i2s_driver_init();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_driver_init fail, ret:%d\n", ret);
		return ret;
	}

	i2s_config.role = I2S_ROLE_MASTER;
	i2s_config.work_mode = I2S_WORK_MODE_I2S;
	i2s_config.samp_rate = I2S_SAMP_RATE_44100;
	i2s_config.data_length = 16;
	i2s_config.store_mode = I2S_LRCOM_STORE_16R16L;

	ret = (I2S_GPIO_GROUP_2, &i2s_config);
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_init fail, ret:%d\n", ret);
		bk_i2s_driver_deinit();
		return ret;
	}

	ret = bk_i2s_chl_init(I2S_CHANNEL_1,
						  I2S_TXRX_TYPE_TX,
						  16384 * 4,
						  i2s_tx_data_callback,
						  tx_rb);
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_chl_init fail, ret:%d\n", ret);
		bk_i2s_deinit();
		bk_i2s_driver_deinit();
		return ret;
	}

	i2s_rate = get_i2s_sample_rate(sample_rate);
	ret = bk_i2s_set_samp_rate(i2s_rate);
	if (ret != BK_OK)
	{
		BK_LOGW(TAG, "bk_i2s_set_samp_rate fail, ret:%d\n", ret);
	}

	ret = bk_i2s_start();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_start fail, ret:%d\n", ret);
		bk_i2s_chl_deinit(I2S_CHANNEL_1, I2S_TXRX_TYPE_TX);
		bk_i2s_deinit();
		bk_i2s_driver_deinit();
		return ret;
	}

	return BK_OK;
}

static void audio_play_i2s_hw_stop(void)
{
	bk_err_t ret;

	ret = bk_i2s_stop();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_stop fail, ret:%d\n", ret);
	}

	ret = bk_i2s_chl_deinit(I2S_CHANNEL_1, I2S_TXRX_TYPE_TX);
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_chl_deinit fail, ret:%d\n", ret);
	}

	ret = bk_i2s_deinit();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_deinit fail, ret:%d\n", ret);
	}

	ret = bk_i2s_driver_deinit();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_driver_deinit fail, ret:%d\n", ret);
	}
}

static void audio_play_apply_gain(int16_t *samples, uint32_t sample_count)
{
	uint32_t index;

	if (samples == NULL)
	{
		return;
	}

	for (index = 0; index < sample_count; ++index)
	{
		int32_t scaled = (int32_t)(samples[index] * gain);

		if (scaled > 32767)
		{
			scaled = 32767;
		}
		else if (scaled < -32768)
		{
			scaled = -32768;
		}

		samples[index] = (int16_t)scaled;
	}
}
