
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	AUDIO_PLAY_IDLE = 0,
	AUDIO_PLAY_EXIT,
	AUDIO_PLAY_MAX
} audio_play_op_t;

typedef struct {
	audio_play_op_t op;
	void *param;
} audio_play_msg_t;

bk_err_t audio_play_sdcard_i2s_start(char *file_name);

bk_err_t audio_play_sdcard_i2s_stop(void);

#ifdef __cplusplus
}
#endif
