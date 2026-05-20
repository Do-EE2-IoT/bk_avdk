
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

	typedef enum
	{
		AUDIO_PLAY_IDLE = 0,
		AUDIO_PLAY_EXIT,
		AUDIO_PLAY_MAX
	} audio_play_op_t;

	typedef struct
	{
		audio_play_op_t op;
		void *param;
	} audio_play_msg_t;

	bk_err_t audio_play_pcm_i2s_start(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

	bk_err_t audio_play_pcm_i2s_write(uint8_t *data, uint32_t size, uint32_t timeout_ms);

		bk_err_t audio_play_pcm_i2s_stop(void);

		bk_err_t audio_play_i2s_direct_start(uint32_t sample_rate);

		bk_err_t audio_play_i2s_direct_stop(void);

		bk_err_t audio_play_i2s_get_tx_addr(uint32_t *i2s_data_addr);

		bk_err_t audio_play_i2s_direct_write_word(uint32_t data);

	#ifdef __cplusplus
	}
#endif
