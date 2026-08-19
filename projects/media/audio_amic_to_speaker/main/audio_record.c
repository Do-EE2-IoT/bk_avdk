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

#include <os/os.h>
#include <os/mem.h>
#include "audio_record.h"
#include "audio_http_stream.h"
#include "aud_intf.h"
#include "aud_intf_types.h"
#include "ff.h"
#include "diskio.h"
#include "gpio_driver.h"

#define TAG "AUD_MIC_DEBUG"
#define MIC_LOG_INTERVAL_FRAMES 100
#define MIC_LOG_SAMPLE_PAIRS 32
#define MIC_DEBUG_ENABLE_SPEAKER_LOOPBACK 0
#define MIC_HTTP_STREAM_CHANNELS 1
#define MIC_SOFTWARE_GAIN 4
#define MIC_STREAM_SOURCE 2

static int16_t mic_apply_gain(int16_t sample)
{
    int32_t amplified = (int32_t)sample * MIC_SOFTWARE_GAIN;

    if (amplified > 32767)
        return 32767;
    if (amplified < -32768)
        return -32768;

    return (int16_t)amplified;
}

// static FIL mic_file;
// static char mic_file_name[50];

// static FATFS *pfs = NULL;

static bk_err_t tf_mount(void)
{
    // FRESULT fr;

    // if (pfs != NULL)
    // {
    // 	os_free(pfs);
    // }

    // pfs = os_malloc(sizeof(FATFS));
    // if(NULL == pfs)
    // {
    //     os_printf("%s: malloc failed!\n", __func__);
    // 	return BK_FAIL;
    // }

    // fr = f_mount(pfs, "1:", 1);
    // if (fr != FR_OK)
    // {
    //     os_printf("%s: f_mount failed:%d\n", __func__, fr);
    // 	return BK_FAIL;
    // }
    // else
    // {
    //     os_printf("%s: f_mount OK!\n", __func__);
    // }

    os_printf("%s: tfcard mount successful!\n", __func__);

    return BK_OK;
}

static bk_err_t tf_unmount(void)
{
    // FRESULT fr;
    // fr = f_unmount(DISK_NUMBER_SDIO_SD, "1:", 1);
    // if (fr != FR_OK)
    // {
    //     os_printf("%s: f_unmount failed:%d\n", __func__, fr);
    // 	return BK_FAIL;
    // }
    // else
    // {
    //     os_printf("%s: f_unmount OK!\n", __func__);
    // }

    // if (pfs)
    // {
    // 	os_free(pfs);
    // 	pfs = NULL;
    // }

    os_printf("%s: tfcard unmount successful!\n", __func__);

    return BK_OK;
}

static int send_mic_data_to_sd(uint8_t *data, unsigned int len)
{
    int16_t *pcm = (int16_t *)data;
    unsigned int samples = len / 2;
    unsigned int stream_samples = samples / 2;
    static uint32_t frame_count = 0;

    frame_count++;

    for (unsigned int i = 0; i < stream_samples; i++)
    {
        pcm[i] = mic_apply_gain(pcm[i * 2 + 1]);
    }

    if ((frame_count % MIC_LOG_INTERVAL_FRAMES) == 0)
    {
        int16_t mic_min = 32767;
        int16_t mic_max = -32768;
        int64_t mic_abs_sum = 0;
        unsigned int logged_samples = stream_samples < MIC_LOG_SAMPLE_PAIRS ? stream_samples : MIC_LOG_SAMPLE_PAIRS;

        for (unsigned int i = 0; i < logged_samples; i++)
        {
            int16_t mic = pcm[i];

            if (mic < mic_min)
                mic_min = mic;
            if (mic > mic_max)
                mic_max = mic;

            mic_abs_sum += mic >= 0 ? mic : -mic;
        }

        os_printf("%s: frame=%lu len=%u samples=%u checked=%u "
                  "mic%d[min=%d max=%d avg_abs=%ld]\n",
                  TAG,
                  (unsigned long)frame_count,
                  len,
                  stream_samples,
                  logged_samples,
                  MIC_STREAM_SOURCE,
                  mic_min,
                  mic_max,
                  (long)(logged_samples ? mic_abs_sum / logged_samples : 0));
    }

#if MIC_DEBUG_ENABLE_SPEAKER_LOOPBACK
    bk_aud_intf_write_spk_data(data, len);
#endif
    audio_http_stream_push_frame(data, stream_samples * 2);
    return len;
}

bk_err_t audio_record_to_sdcard_start(char *file_name, uint32_t samp_rate)
{
    bk_err_t ret = BK_OK;
    // FRESULT fr;

    aud_intf_drv_setup_t aud_intf_drv_setup = DEFAULT_AUD_INTF_DRV_SETUP_CONFIG();
    aud_intf_mic_setup_t aud_intf_mic_setup = DEFAULT_AUD_INTF_MIC_SETUP_CONFIG();

    ret = tf_mount();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: tfcard mount fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    /*open file to save pcm data */
    // sprintf(mic_file_name, "1:/%s", file_name);
    // fr = f_open(&mic_file, mic_file_name, FA_CREATE_ALWAYS | FA_WRITE);
    // if (fr != FR_OK) {
    //     os_printf("%s: open %s fail\n", __func__, mic_file_name);
    //     goto fail;
    // }

    aud_intf_drv_setup.aud_intf_tx_mic_data = send_mic_data_to_sd;
    ret = bk_aud_intf_drv_init(&aud_intf_drv_setup);
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_drv_init fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    ret = bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_GENERAL);
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_set_mode fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    aud_intf_mic_setup.mic_chl = AUD_INTF_MIC_CHL_DUAL;
    aud_intf_mic_setup.samp_rate = samp_rate;
    // aud_intf_mic_setup.mic_type = AUD_INTF_MIC_TYPE_UAC;
    aud_intf_mic_setup.frame_size = 640;
    aud_intf_mic_setup.mic_gain = 0x2d;

    ret = audio_http_stream_start(samp_rate, MIC_HTTP_STREAM_CHANNELS);
    if (ret != BK_OK)
    {
        os_printf("%s: audio_http_stream_start fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    ret = bk_aud_intf_mic_init(&aud_intf_mic_setup);
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_mic_init fail, ret:%d\n", __func__, ret);
        goto fail;
    }

#if MIC_DEBUG_ENABLE_SPEAKER_LOOPBACK
    /* initialize speaker so we can forward mic PCM to spk */
    {
        aud_intf_spk_setup_t aud_intf_spk_setup = DEFAULT_AUD_INTF_SPK_SETUP_CONFIG();
        aud_intf_spk_setup.samp_rate = samp_rate;
        aud_intf_spk_setup.frame_size = aud_intf_mic_setup.frame_size;
        aud_intf_spk_setup.spk_gain = 0x2d;

        ret = bk_aud_intf_spk_init(&aud_intf_spk_setup);
        if (ret != BK_ERR_AUD_INTF_OK)
        {
            os_printf("%s: bk_aud_intf_spk_init fail, ret:%d\n", __func__, ret);
            goto fail;
        }

        ret = bk_aud_intf_spk_start();
        if (ret != BK_ERR_AUD_INTF_OK)
        {
            os_printf("%s: bk_aud_intf_spk_start fail, ret:%d\n", __func__, ret);
            goto fail;
        }
    }
#endif

    ret = bk_aud_intf_mic_start();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_mic_start fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    return BK_OK;

fail:

    bk_aud_intf_mic_stop();
    bk_aud_intf_mic_deinit();
#if MIC_DEBUG_ENABLE_SPEAKER_LOOPBACK
    bk_aud_intf_spk_stop();
    bk_aud_intf_spk_deinit();
#endif
    bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
    bk_aud_intf_drv_deinit();
    audio_http_stream_stop();

    /* close mic file */
    // f_close(&mic_file);

    return BK_FAIL;
}

bk_err_t audio_record_to_sdcard_stop(void)
{
    bk_err_t ret;
    ret = bk_aud_intf_mic_stop();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_mic_stop fail, ret:%d\n", __func__, ret);
    }

    ret = bk_aud_intf_mic_deinit();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_mic_deinit fail, ret:%d\n", __func__, ret);
    }

#if MIC_DEBUG_ENABLE_SPEAKER_LOOPBACK
    /* stop and deinit speaker if it was started */
    ret = bk_aud_intf_spk_stop();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_spk_stop fail, ret:%d\n", __func__, ret);
    }

    ret = bk_aud_intf_spk_deinit();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_spk_deinit fail, ret:%d\n", __func__, ret);
    }
#endif

    ret = bk_aud_intf_set_mode(AUD_INTF_WORK_MODE_NULL);
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_set_mode fail, ret:%d\n", __func__, ret);
    }

    ret = bk_aud_intf_drv_deinit();
    if (ret != BK_ERR_AUD_INTF_OK)
    {
        os_printf("%s: bk_aud_intf_drv_deinit fail, ret:%d\n", __func__, ret);
    }

    audio_http_stream_stop();

    /* close mic file */
    // f_close(&mic_file);

    tf_unmount();

    return BK_OK;
}
