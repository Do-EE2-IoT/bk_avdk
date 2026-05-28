// Copyright 2023-2024 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//	   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stdio.h"
#include <os/os.h>
#include <os/mem.h>
#include <os/str.h>
#include <modules/mp3dec.h>
#include "ff.h"
#include "diskio.h"
#include <driver/i2s.h>
#include <driver/i2s_types.h>
#include <driver/audio_ring_buff.h>
#include "audio_play.h"


#define TAG  "AUD_PLAY_SDCARD_MP3"

#define PCM_SIZE_MAX		(MAX_NSAMP * MAX_NCHAN * MAX_NGRAN)
#define I2S_TX_RING_BUFFER_SIZE (16384 * 4)
#define I2S_MP3_DECODE_STACK_SIZE (4096)


typedef struct {
    HMP3Decoder hMP3Decoder;
    MP3FrameInfo mp3FrameInfo;
    unsigned char *readBuf;
    short *pcmBuf;
    int bytesLeft;

    FIL mp3file;
    char mp3_file_name[50];
    unsigned char *g_readptr;

    bool mp3_file_is_empty;

    RingBufferContext *i2s_tx_rb;
    beken_thread_t decode_thread;
    bool decode_thread_running;
    bool i2s_started;
} audio_play_info_t;


static audio_play_info_t *audio_play_info = NULL;
static FATFS *pfs = NULL;

static bk_err_t audio_play_i2s_hw_start(uint32_t sample_rate, RingBufferContext **tx_rb);
static void audio_play_i2s_hw_stop(void);

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
		BK_LOGW(TAG, "Unsupported sample rate %lu, using 44100\n", (unsigned long)mp3_rate);
		return I2S_SAMP_RATE_44100;
	}
}


static bk_err_t tf_mount(void)
{
	FRESULT fr;

	if (pfs != NULL)
	{
		os_free(pfs);
	}

	pfs = os_malloc(sizeof(FATFS));
	if(NULL == pfs)
	{
		BK_LOGI(TAG, "f_mount malloc failed!\r\n");
		return BK_FAIL;
	}

	fr = f_mount(pfs, "1:", 1);
	if (fr != FR_OK)
	{
		BK_LOGE(TAG, "f_mount failed:%d\r\n", fr);
		os_free(pfs);
		pfs = NULL;
		return BK_FAIL;
	}
	else
	{
		BK_LOGI(TAG, "f_mount OK!\r\n");
	}

	return BK_OK;
}

static bk_err_t tf_unmount(void)
{
	FRESULT fr;
	fr = f_unmount(DISK_NUMBER_SDIO_SD, "1:", 1);
	if (fr != FR_OK)
	{
		BK_LOGE(TAG, "f_unmount failed:%d\r\n", fr);
		return BK_FAIL;
	}
	else
	{
		BK_LOGI(TAG, "f_unmount OK!\r\n");
	}

	if (pfs)
	{
		os_free(pfs);
		pfs = NULL;
	}

	return BK_OK;
}


static bk_err_t mp3_decode_handler(unsigned int size)
{
	bk_err_t ret = BK_OK;

	FRESULT fr;
	uint32 uiTemp = 0;
    static bool empty_already_flag = false;

    if (!audio_play_info) {
        return BK_FAIL;
    }

	if (audio_play_info->mp3_file_is_empty) {
        if (empty_already_flag == false) {
            empty_already_flag = true;
		    BK_LOGW(TAG, "==========================================================\n");
		    BK_LOGW(TAG, "%s playback is over, please input the stop command!\n", audio_play_info->mp3_file_name);
		    BK_LOGW(TAG, "==========================================================\n");
        }
		return BK_FAIL;
	}

    empty_already_flag = false;

	if (audio_play_info->bytesLeft < MAINBUF_SIZE) {
		os_memmove(audio_play_info->readBuf, audio_play_info->g_readptr, audio_play_info->bytesLeft);
		fr = f_read(&audio_play_info->mp3file, (void *)(audio_play_info->readBuf + audio_play_info->bytesLeft), MAINBUF_SIZE - audio_play_info->bytesLeft, &uiTemp);
		if (fr != FR_OK) {
			BK_LOGE(TAG, "read %s failed\n", audio_play_info->mp3_file_name);
			return fr;
		}

		if ((uiTemp == 0) && (audio_play_info->bytesLeft == 0)) {
			BK_LOGI(TAG, "uiTemp = 0 and bytesLeft = 0\n");
			audio_play_info->mp3_file_is_empty = true;
			BK_LOGI(TAG, "the %s is empty\n", audio_play_info->mp3_file_name);
			return ret;
		}

		audio_play_info->bytesLeft = audio_play_info->bytesLeft + uiTemp;
		audio_play_info->g_readptr = audio_play_info->readBuf;
	}

	int offset = MP3FindSyncWord(audio_play_info->g_readptr, audio_play_info->bytesLeft);

	if (offset < 0) {
		BK_LOGE(TAG, "MP3FindSyncWord not find\n");
		audio_play_info->bytesLeft = 0;
	} else {
		audio_play_info->g_readptr += offset;
		audio_play_info->bytesLeft -= offset;
		
		ret = MP3Decode(audio_play_info->hMP3Decoder, &audio_play_info->g_readptr, &audio_play_info->bytesLeft, audio_play_info->pcmBuf, 0);
		if (ret != ERR_MP3_NONE) {
			BK_LOGE(TAG, "MP3Decode failed, code is %d\n", ret);
			return ret;
		}

		MP3GetLastFrameInfo(audio_play_info->hMP3Decoder, &audio_play_info->mp3FrameInfo);
//		os_printf("Bitrate: %d kb/s, Samprate: %d\r\n", (mp3FrameInfo.bitrate) / 1000, mp3FrameInfo.samprate);
//		os_printf("Channel: %d, Version: %d, Layer: %d\r\n", mp3FrameInfo.nChans, mp3FrameInfo.version, mp3FrameInfo.layer);
//		os_printf("OutputSamps: %d\r\n", mp3FrameInfo.outputSamps);

		uint32_t pcm_size = audio_play_info->mp3FrameInfo.outputSamps * 2;
		uint8_t *write_ptr = (uint8_t *)audio_play_info->pcmBuf;
		uint32_t total_written = 0;

		while (total_written < pcm_size) {
			uint32_t written = ring_buffer_write(audio_play_info->i2s_tx_rb,
												 write_ptr + total_written,
												 pcm_size - total_written);

			total_written += written;
			if (total_written < pcm_size) {
				rtos_delay_milliseconds(2);
			}
		}
	}

	return ret;
}

bk_err_t audio_play_sdcard_mp3_music_stop(void)
{
    if (!audio_play_info) {
		if (pfs) {
			tf_unmount();
		}
        return BK_OK;
    }

	if (audio_play_info->decode_thread_running) {
		int wait_count = 0;

		audio_play_info->decode_thread_running = false;
		while (audio_play_info->decode_thread != NULL && wait_count < 200) {
			rtos_delay_milliseconds(10);
			wait_count++;
		}

		if (audio_play_info->decode_thread != NULL) {
			rtos_delete_thread(&audio_play_info->decode_thread);
			audio_play_info->decode_thread = NULL;
		}
	}

	if (audio_play_info->i2s_started) {
		audio_play_i2s_hw_stop();
		audio_play_info->i2s_started = false;
	}

	audio_play_info->bytesLeft = 0;
	audio_play_info->mp3_file_is_empty = false;

	f_close(&audio_play_info->mp3file);

    if (audio_play_info->hMP3Decoder) {
	    MP3FreeDecoder(audio_play_info->hMP3Decoder);
        audio_play_info->hMP3Decoder = NULL;
    }

    if (audio_play_info->readBuf) {
        os_free(audio_play_info->readBuf);
        audio_play_info->readBuf = NULL;
    }

    if (audio_play_info->pcmBuf) {
        os_free(audio_play_info->pcmBuf);
        audio_play_info->pcmBuf = NULL;
    }

    if (audio_play_info) {
        os_free(audio_play_info);
        audio_play_info = NULL;
    }

    tf_unmount();

    return BK_OK;
}

static int i2s_tx_data_callback(uint32_t size)
{
	return size;
}

static bk_err_t audio_play_i2s_hw_start(uint32_t sample_rate, RingBufferContext **tx_rb)
{
	bk_err_t ret;
	i2s_config_t i2s_config = DEFAULT_I2S_CONFIG();

	if (tx_rb == NULL) {
		return BK_ERR_NULL_PARAM;
	}

	ret = bk_i2s_driver_init();
	if (ret != BK_OK) {
		BK_LOGE(TAG, "bk_i2s_driver_init fail, ret:%d\n", ret);
		return ret;
	}

	i2s_config.role = I2S_ROLE_MASTER;
	i2s_config.work_mode = I2S_WORK_MODE_I2S;
	i2s_config.samp_rate = I2S_SAMP_RATE_48000;
	i2s_config.data_length = 16;
	i2s_config.store_mode = I2S_LRCOM_STORE_16R16L;

	ret = bk_i2s_init(I2S_GPIO_GROUP_2, &i2s_config);
	if (ret != BK_OK) {
		BK_LOGE(TAG, "bk_i2s_init group2 fail, ret:%d\n", ret);
		bk_i2s_driver_deinit();
		return ret;
	}

	ret = bk_i2s_chl_init(I2S_CHANNEL_1,
						  I2S_TXRX_TYPE_TX,
						  I2S_TX_RING_BUFFER_SIZE,
						  i2s_tx_data_callback,
						  tx_rb);
	if (ret != BK_OK) {
		BK_LOGE(TAG, "bk_i2s_chl_init fail, ret:%d\n", ret);
		bk_i2s_deinit();
		bk_i2s_driver_deinit();
		return ret;
	}

	ret = bk_i2s_set_samp_rate(get_i2s_sample_rate(sample_rate));
	if (ret != BK_OK) {
		BK_LOGW(TAG, "bk_i2s_set_samp_rate fail, ret:%d\n", ret);
	}

	ret = bk_i2s_start();
	if (ret != BK_OK) {
		BK_LOGE(TAG, "bk_i2s_start fail, ret:%d\n", ret);
		bk_i2s_chl_deinit(I2S_CHANNEL_1, I2S_TXRX_TYPE_TX);
		bk_i2s_deinit();
		bk_i2s_driver_deinit();
		return ret;
	}

	if (audio_play_info) {
		audio_play_info->i2s_started = true;
	}

	return BK_OK;
}

static void audio_play_i2s_hw_stop(void)
{
	bk_i2s_stop();
	bk_i2s_chl_deinit(I2S_CHANNEL_1, I2S_TXRX_TYPE_TX);
	bk_i2s_deinit();
	bk_i2s_driver_deinit();
}

static void audio_decode_thread(void *arg)
{
	BK_LOGI(TAG, "MP3 decode thread started\n");

	while (audio_play_info && audio_play_info->decode_thread_running) {
		if (audio_play_info->mp3_file_is_empty) {
			break;
		}

		if (mp3_decode_handler(0) != BK_OK) {
			rtos_delay_milliseconds(5);
			continue;
		}

		rtos_delay_milliseconds(2);
	}

	if (audio_play_info) {
		audio_play_info->decode_thread_running = false;
		audio_play_info->decode_thread = NULL;
	}

	BK_LOGI(TAG, "MP3 decode thread exit\n");
	rtos_delete_thread(NULL);
}

bk_err_t audio_play_sdcard_mp3_music_start(char *file_name)
{
	bk_err_t ret = BK_OK;
    uint32 uiTemp = 0;
	char tag_header[10];
	int tag_size = 0;

	if (!file_name) {
		BK_LOGE(TAG, "file_name is NULL\n");
		return BK_FAIL;
	}

	if (audio_play_info) {
		audio_play_sdcard_mp3_music_stop();
	}

    ret = tf_mount();
    if (ret != BK_OK) {
        BK_LOGE(TAG, "mount sdcard fail\n");
        return BK_FAIL;
    }

    audio_play_info = (audio_play_info_t *)os_malloc(sizeof(audio_play_info_t));
    if (!audio_play_info) {
        BK_LOGE(TAG, "mount sdcard fail\n");
        goto fail;
    }

    os_memset(audio_play_info, 0, sizeof(audio_play_info_t));

	audio_play_info->readBuf = os_malloc(MAINBUF_SIZE);
	if (audio_play_info->readBuf == NULL) {
		BK_LOGE(TAG, "readBuf malloc fail\n");
		goto fail;
	}
    os_memset(audio_play_info->readBuf, 0, MAINBUF_SIZE);

	audio_play_info->pcmBuf = os_malloc(PCM_SIZE_MAX * 2);
	if (audio_play_info->pcmBuf == NULL) {
		BK_LOGE(TAG, "pcmBuf malloc fail\n");
		goto fail;
	}
    os_memset(audio_play_info->pcmBuf, 0, PCM_SIZE_MAX * 2);

	audio_play_info->hMP3Decoder = MP3InitDecoder();
	if (audio_play_info->hMP3Decoder == NULL) {
		BK_LOGE(TAG, "MP3Decoder init fail\n");
		goto fail;
	}

	BK_LOGI(TAG, "audio mp3 play decode init complete\n");

	/*open file to read mp3 data */
    os_memset(audio_play_info->mp3_file_name, 0, sizeof(audio_play_info->mp3_file_name)/sizeof(audio_play_info->mp3_file_name[0]));
	sprintf(audio_play_info->mp3_file_name, "%d:/%s", DISK_NUMBER_SDIO_SD, file_name);
	FRESULT fr = f_open(&audio_play_info->mp3file, audio_play_info->mp3_file_name, FA_OPEN_EXISTING | FA_READ);
	if (fr != FR_OK) {
		BK_LOGE(TAG, "open %s fail\n", audio_play_info->mp3_file_name);
		goto fail;
	}
	BK_LOGI(TAG, "mp3 file: %s open successful\n", audio_play_info->mp3_file_name);

    fr = f_read(&audio_play_info->mp3file, (void *)tag_header, 10, &uiTemp);
    if (fr != FR_OK)
    {
        BK_LOGE(TAG, "read %s fail\n", audio_play_info->mp3_file_name);
        goto fail;
    }

    if (os_memcmp(tag_header, "ID3", 3) == 0)
    {
        tag_size = ((tag_header[6] & 0x7F) << 21) | ((tag_header[7] & 0x7F) << 14) | ((tag_header[8] & 0x7F) << 7) | (tag_header[9] & 0x7F);
        BK_LOGI(TAG, "tag_size = %d\n", tag_size);
        f_lseek(&audio_play_info->mp3file, tag_size + 10);
        BK_LOGI(TAG, "tag_header has found\n");
    }
    else
    {
        BK_LOGI(TAG, "tag_header not found\n");
        f_lseek(&audio_play_info->mp3file, 0);
    }

	ret = audio_play_i2s_hw_start(48000U, &audio_play_info->i2s_tx_rb);
	if (ret != BK_OK) {
		goto fail;
	}

	audio_play_info->g_readptr = audio_play_info->readBuf;
	ret = mp3_decode_handler(audio_play_info->mp3FrameInfo.outputSamps * 2);
    if (ret < 0) {
        BK_LOGE(TAG, "mp3_decode_handler fail, ret:%d\n", ret);
        goto fail;
    }

	ret = bk_i2s_set_samp_rate(I2S_SAMP_RATE_48000);
	if (ret != BK_OK) {
		BK_LOGW(TAG, "bk_i2s_set_samp_rate fail, ret:%d\n", ret);
	}

	BK_LOGI(TAG, "I2S group2 MP3 playback started, i2s_rate=48000 mp3_rate=%d channels=%d\n",
			audio_play_info->mp3FrameInfo.samprate,
			audio_play_info->mp3FrameInfo.nChans);

	audio_play_info->decode_thread_running = true;
	audio_play_info->decode_thread = NULL;
	ret = rtos_create_thread(&audio_play_info->decode_thread,
							 BEKEN_DEFAULT_WORKER_PRIORITY,
							 "mp3_i2s_decode",
							 (beken_thread_function_t)audio_decode_thread,
							 I2S_MP3_DECODE_STACK_SIZE,
							 NULL);
	if (ret != BK_OK) {
		BK_LOGE(TAG, "create decode thread fail, ret:%d\n", ret);
		audio_play_info->decode_thread_running = false;
		goto fail;
	}

    return BK_OK;

fail:

    audio_play_sdcard_mp3_music_stop();

	return BK_FAIL;
}

