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
#include <driver/aud_dmic.h>
#include <driver/aud_dac.h>
#include "ff.h"
#include "diskio.h"


#define TAG  "AUD_RECORD_SDCARD"

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
    // Forward mic data to SD (or other storage) and also forward to speaker for immediate playback
    // SD write is currently stubbed out; keep logging and forward to speaker
    os_printf("%s: data: %p, len: %u\n", __func__, data, len);

    return len;
}

static void audio_dmic_isr(void)
{
    uint32_t dmic_data;

    /* Read several samples from DMIC FIFO and forward to DAC */
    for (uint8_t i = 0; i < 16; i++) {
        if (bk_aud_dmic_get_fifo_data(&dmic_data) == BK_OK) {
            bk_aud_dac_write(dmic_data);
        } 
        else {
            break;
        }
    }
}

bk_err_t audio_record_to_sdcard_start(char *file_name, uint32_t samp_rate)
{
    bk_err_t ret = BK_OK;

    ret = tf_mount();
    if (ret != BK_OK) {
        os_printf("%s: tfcard mount fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    /* initialize DAC (speaker) */
    {
        aud_dac_config_t dac_cfg = DEFAULT_AUD_DAC_CONFIG();
        dac_cfg.samp_rate = samp_rate;

        ret = bk_aud_dac_init(&dac_cfg);
        if (ret != BK_OK) {
            os_printf("%s: bk_aud_dac_init fail, ret:%d\n", __func__, ret);
            goto fail;
        }
    }

    /* initialize DMIC */
    {
        aud_dmic_config_t dmic_cfg = DEFAULT_AUD_DMIC_CONFIG();
        dmic_cfg.samp_rate = samp_rate;

        ret = bk_aud_dmic_init(&dmic_cfg);
        if (ret != BK_OK) {
            os_printf("%s: bk_aud_dmic_init fail, ret:%d\n", __func__, ret);
            goto fail;
        }

        /* register ISR and enable interrupt (dmic-specific register) */
        ret = bk_aud_dmic_register_isr(audio_dmic_isr);
        if (ret != BK_OK) {
            os_printf("%s: register dmic isr fail, ret:%d\n", __func__, ret);
            goto fail;
        }

        bk_aud_dmic_set_dmic_wr_threshold(8);
        bk_aud_dmic_enable_int();

        /* start DAC and DMIC */
        bk_aud_dac_start();
        bk_aud_dmic_start();
    }

    return BK_OK;

fail:
    /* try to clean up any partial init */
    bk_aud_dmic_stop();
    bk_aud_dmic_deinit();
    bk_aud_dac_stop();
    bk_aud_dac_deinit();

    return BK_FAIL;
}

bk_err_t audio_record_to_sdcard_stop(void)
{
	bk_err_t ret;

    /* disable dmic interrupt and unregister handler */
    bk_aud_dmic_disable_int();
    bk_aud_dmic_register_isr(NULL);

    /* stop and deinit DMIC and DAC */
    ret = bk_aud_dmic_stop();
    if (ret != BK_OK) {
        os_printf("%s: bk_aud_dmic_stop fail, ret:%d\n", __func__, ret);
    }
    ret = bk_aud_dmic_deinit();
    if (ret != BK_OK) {
        os_printf("%s: bk_aud_dmic_deinit fail, ret:%d\n", __func__, ret);
    }

    ret = bk_aud_dac_stop();
    if (ret != BK_OK) {
        os_printf("%s: bk_aud_dac_stop fail, ret:%d\n", __func__, ret);
    }
    ret = bk_aud_dac_deinit();
    if (ret != BK_OK) {
        os_printf("%s: bk_aud_dac_deinit fail, ret:%d\n", __func__, ret);
    }

    tf_unmount();

    return BK_OK;
}

