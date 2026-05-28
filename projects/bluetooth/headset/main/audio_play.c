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
#include "tas_5805.h"

#define TAG "AUD_PLAY_I2S"
#define AUDIO_PLAY_I2S_STORE_MODE I2S_LRCOM_STORE_16R16L
#define AUDIO_PLAY_I2S_REF_LOG_INTERVAL 64U

float gain = 1.0f;

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
static uint32_t s_audio_i2s_ref_log_counter = 0;
static bool s_audio_i2s_direct_started = false;

static bk_err_t audio_play_i2s_hw_start(uint32_t sample_rate, RingBufferContext **tx_rb);
static void audio_play_i2s_hw_stop(void);
static void audio_play_apply_gain(int16_t *samples, uint32_t sample_count);
static void audio_play_log_pcm_chunk(const uint8_t *data, uint32_t size, uint32_t total_written, uint32_t written);
static void audio_play_log_i2s_ref(const uint8_t *data,
								   uint32_t size,
								   uint32_t written,
								   uint32_t timeout_ms,
								   uint32_t rb_free_before,
								   uint32_t rb_fill_before,
								   uint32_t rb_capacity,
								   uint32_t rb_free_after,
								   uint32_t rb_fill_after,
								   bool force_log);

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
	s_audio_i2s_ref_log_counter = 0;

	BK_LOGW(TAG, "PCM I2S stream started, rate=%lu channels=%u bits=%u\n",
			(unsigned long)sample_rate, channels, bits_per_sample);

	return BK_OK;
}

bk_err_t audio_play_pcm_i2s_write(uint8_t *data, uint32_t size, uint32_t timeout_ms)
{
	uint32_t total_written = 0;
	uint32_t waited_ms = 0;
	uint32_t rb_free_before;
	uint32_t rb_fill_before;
	uint32_t rb_capacity;

	if ((data == NULL) || (size == 0U))
	{
		return BK_ERR_NULL_PARAM;
	}

	if (!s_audio_pcm_stream.started || (s_audio_pcm_stream.i2s_tx_rb == NULL))
	{
		return BK_ERR_NOT_INIT;
	}

	rb_free_before = ring_buffer_get_free_size(s_audio_pcm_stream.i2s_tx_rb);
	rb_fill_before = ring_buffer_get_fill_size(s_audio_pcm_stream.i2s_tx_rb);
	rb_capacity = s_audio_pcm_stream.i2s_tx_rb->capacity;

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
			audio_play_log_i2s_ref(data,
								   size,
								   total_written,
								   timeout_ms,
								   rb_free_before,
								   rb_fill_before,
								   rb_capacity,
								   ring_buffer_get_free_size(s_audio_pcm_stream.i2s_tx_rb),
								   ring_buffer_get_fill_size(s_audio_pcm_stream.i2s_tx_rb),
								   false);
			return BK_OK;
		}

		if (waited_ms >= timeout_ms)
		{
			BK_LOGW(TAG, "PCM write timeout, dropped %lu bytes\n",
					(unsigned long)(size - total_written));
			audio_play_log_i2s_ref(data,
								   size,
								   total_written,
								   timeout_ms,
								   rb_free_before,
								   rb_fill_before,
								   rb_capacity,
								   ring_buffer_get_free_size(s_audio_pcm_stream.i2s_tx_rb),
								   ring_buffer_get_fill_size(s_audio_pcm_stream.i2s_tx_rb),
								   true);
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

static uint32_t audio_play_abs_i32(int32_t value)
{
	if (value < 0)
	{
		return (uint32_t)(-value);
	}

	return (uint32_t)value;
}

static uint32_t audio_play_u32_from_le_bytes(const uint8_t *data)
{
	return ((uint32_t)data[0]) |
		   ((uint32_t)data[1] << 8) |
		   ((uint32_t)data[2] << 16) |
		   ((uint32_t)data[3] << 24);
}

static void audio_play_log_i2s_ref(const uint8_t *data,
								   uint32_t size,
								   uint32_t written,
								   uint32_t timeout_ms,
								   uint32_t rb_free_before,
								   uint32_t rb_fill_before,
								   uint32_t rb_capacity,
								   uint32_t rb_free_after,
								   uint32_t rb_fill_after,
								   bool force_log)
{
	const int16_t *samples;
	uint32_t sample_count;
	uint32_t word_count;
	uint32_t index;
	int16_t first_i16[8] = {0};
	uint32_t first_u32[4] = {0};
	int32_t min = 0;
	int32_t max = 0;
	uint64_t sum_abs = 0;
	uint32_t avg_abs = 0;
	uint32_t peak = 0;
	uint32_t even_count = 0;
	uint32_t odd_count = 0;
	int32_t even_min = 0;
	int32_t even_max = 0;
	int32_t odd_min = 0;
	int32_t odd_max = 0;
	uint64_t even_sum_abs = 0;
	uint64_t odd_sum_abs = 0;
	uint32_t even_avg_abs = 0;
	uint32_t odd_avg_abs = 0;
	uint32_t even_peak = 0;
	uint32_t odd_peak = 0;

	if (data == NULL)
	{
		return;
	}

	if (!force_log)
	{
		s_audio_i2s_ref_log_counter++;
		if ((s_audio_i2s_ref_log_counter % AUDIO_PLAY_I2S_REF_LOG_INTERVAL) != 0U)
		{
			return;
		}
	}

	sample_count = size / 2U;
	word_count = size / 4U;
	samples = (const int16_t *)data;

	for (index = 0; (index < sample_count) && (index < 8U); index++)
	{
		first_i16[index] = samples[index];
	}

	for (index = 0; (index < word_count) && (index < 4U); index++)
	{
		first_u32[index] = audio_play_u32_from_le_bytes(data + (index * 4U));
	}

	for (index = 0; index < sample_count; index++)
	{
		int32_t value = samples[index];
		uint32_t abs_value = audio_play_abs_i32(value);

		if (index == 0U)
		{
			min = value;
			max = value;
		}
		else
		{
			if (value < min)
			{
				min = value;
			}
			if (value > max)
			{
				max = value;
			}
		}

		sum_abs += abs_value;

		if ((index & 1U) == 0U)
		{
			if (even_count == 0U)
			{
				even_min = value;
				even_max = value;
			}
			else
			{
				if (value < even_min)
				{
					even_min = value;
				}
				if (value > even_max)
				{
					even_max = value;
				}
			}
			even_sum_abs += abs_value;
			even_count++;
		}
		else
		{
			if (odd_count == 0U)
			{
				odd_min = value;
				odd_max = value;
			}
			else
			{
				if (value < odd_min)
				{
					odd_min = value;
				}
				if (value > odd_max)
				{
					odd_max = value;
				}
			}
			odd_sum_abs += abs_value;
			odd_count++;
		}
	}

	if (sample_count > 0U)
	{
		avg_abs = (uint32_t)(sum_abs / sample_count);
		peak = (audio_play_abs_i32(min) > audio_play_abs_i32(max)) ? audio_play_abs_i32(min) : audio_play_abs_i32(max);
	}

	if (even_count > 0U)
	{
		even_avg_abs = (uint32_t)(even_sum_abs / even_count);
		even_peak = (audio_play_abs_i32(even_min) > audio_play_abs_i32(even_max)) ? audio_play_abs_i32(even_min) : audio_play_abs_i32(even_max);
	}

	if (odd_count > 0U)
	{
		odd_avg_abs = (uint32_t)(odd_sum_abs / odd_count);
		odd_peak = (audio_play_abs_i32(odd_min) > audio_play_abs_i32(odd_max)) ? audio_play_abs_i32(odd_min) : audio_play_abs_i32(odd_max);
	}

	BK_LOGW(TAG,
			"[I2S_REF] size=%lu sample_count=%lu sample_rate=%lu channels=%u bits_per_sample=%u store_mode=%lu rb_free_before=%lu rb_fill_before=%lu rb_capacity=%lu timeout_ms=%lu written=%lu rb_free_after=%lu rb_fill_after=%lu first_i16=[%d,%d,%d,%d,%d,%d,%d,%d] first_u32=[0x%08lX,0x%08lX,0x%08lX,0x%08lX] all[min,max,avg_abs,peak]=[%ld,%ld,%lu,%lu] even[count,min,max,avg_abs,peak]=[%lu,%ld,%ld,%lu,%lu] odd[count,min,max,avg_abs,peak]=[%lu,%ld,%ld,%lu,%lu]\n",
			(unsigned long)size,
			(unsigned long)sample_count,
			(unsigned long)s_audio_pcm_stream.sample_rate,
			s_audio_pcm_stream.channels,
			s_audio_pcm_stream.bits_per_sample,
			(unsigned long)AUDIO_PLAY_I2S_STORE_MODE,
			(unsigned long)rb_free_before,
			(unsigned long)rb_fill_before,
			(unsigned long)rb_capacity,
			(unsigned long)timeout_ms,
			(unsigned long)written,
			(unsigned long)rb_free_after,
			(unsigned long)rb_fill_after,
			first_i16[0],
			first_i16[1],
			first_i16[2],
			first_i16[3],
			first_i16[4],
			first_i16[5],
			first_i16[6],
			first_i16[7],
			(unsigned long)first_u32[0],
			(unsigned long)first_u32[1],
			(unsigned long)first_u32[2],
			(unsigned long)first_u32[3],
			(long)min,
			(long)max,
			(unsigned long)avg_abs,
			(unsigned long)peak,
			(unsigned long)even_count,
			(long)even_min,
			(long)even_max,
			(unsigned long)even_avg_abs,
			(unsigned long)even_peak,
			(unsigned long)odd_count,
			(long)odd_min,
			(long)odd_max,
			(unsigned long)odd_avg_abs,
			(unsigned long)odd_peak);
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

bk_err_t audio_play_i2s_direct_start(uint32_t sample_rate)
{
	bk_err_t ret;
	i2s_config_t i2s_config = DEFAULT_I2S_CONFIG();
	i2s_samp_rate_t i2s_rate;

	if (s_audio_pcm_stream.started)
	{
		return BK_ERR_BUSY;
	}

	if (s_audio_i2s_direct_started)
	{
		return BK_OK;
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
	i2s_config.store_mode = AUDIO_PLAY_I2S_STORE_MODE;

	ret = bk_i2s_init(I2S_GPIO_GROUP_2, &i2s_config);
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_init fail, ret:%d\n", ret);
		bk_i2s_driver_deinit();
		return ret;
	}

	i2s_rate = get_i2s_sample_rate(sample_rate);
	ret = bk_i2s_set_samp_rate(i2s_rate);
	if (ret != BK_OK)
	{
		BK_LOGW(TAG, "bk_i2s_set_samp_rate fail, ret:%d\n", ret);
	}

	ret = bk_i2s_clear_txfifo();
	if (ret != BK_OK)
	{
		BK_LOGW(TAG, "bk_i2s_clear_txfifo fail, ret:%d\n", ret);
	}

	ret = bk_i2s_start();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_start fail, ret:%d\n", ret);
		bk_i2s_deinit();
		bk_i2s_driver_deinit();
		return ret;
	}

	ret = tas5805m_start(sample_rate);
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "tas5805m_start fail, ret:%d\n", ret);
		bk_i2s_stop();
		bk_i2s_deinit();
		bk_i2s_driver_deinit();
		return ret;
	}

	s_audio_i2s_direct_started = true;
	BK_LOGW(TAG, "direct I2S stream started, rate=%lu\n", (unsigned long)sample_rate);
	return BK_OK;
}

bk_err_t audio_play_i2s_direct_stop(void)
{
	bk_err_t ret;

	if (!s_audio_i2s_direct_started)
	{
		return BK_OK;
	}

	ret = tas5805m_stop();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "tas5805m_stop fail, ret:%d\n", ret);
	}

	ret = bk_i2s_stop();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "bk_i2s_stop fail, ret:%d\n", ret);
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

	s_audio_i2s_direct_started = false;
	BK_LOGW(TAG, "direct I2S stream stopped\n");
	return BK_OK;
}

bk_err_t audio_play_i2s_get_tx_addr(uint32_t *i2s_data_addr)
{
	if (i2s_data_addr == NULL)
	{
		return BK_ERR_NULL_PARAM;
	}

	if (!s_audio_i2s_direct_started)
	{
		return BK_ERR_NOT_INIT;
	}

	return bk_i2s_get_data_addr(I2S_CHANNEL_1, i2s_data_addr);
}

bk_err_t audio_play_i2s_direct_write_word(uint32_t data)
{
	uint32_t write_ready = 0;
	uint32_t wait_count = 0;

	if (!s_audio_i2s_direct_started)
	{
		return BK_ERR_NOT_INIT;
	}

	do
	{
		if (bk_i2s_get_write_ready(&write_ready) != BK_OK)
		{
			return BK_FAIL;
		}
		wait_count++;
	} while ((write_ready == 0U) && (wait_count < 100000U));

	if (write_ready == 0U)
	{
		return BK_FAIL;
	}

	return bk_i2s_write_data(I2S_CHANNEL_1, &data, 1);
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
	i2s_config.store_mode = AUDIO_PLAY_I2S_STORE_MODE;

	ret = bk_i2s_init(I2S_GPIO_GROUP_2, &i2s_config);
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

	ret = tas5805m_start(sample_rate);
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "tas5805m_start fail, ret:%d\n", ret);
		bk_i2s_stop();
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

	ret = tas5805m_stop();
	if (ret != BK_OK)
	{
		BK_LOGE(TAG, "tas5805m_stop fail, ret:%d\n", ret);
	}

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
