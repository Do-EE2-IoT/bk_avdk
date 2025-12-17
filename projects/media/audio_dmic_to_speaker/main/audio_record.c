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
#include "driver/gpio.h"
#include "gpio_driver.h"

#define SPEAKER_PA_PIN GPIO_13

#define TAG "AUD_RECORD_SDCARD"

void speaker_pa_enable(void)
{
    // 1. Unmap chân nếu nó đang được dùng cho chức năng khác (VD: JTAG/UART)
    gpio_dev_unmap(SPEAKER_PA_PIN);

    // 2. Disable chức năng Input (để tránh nhiễu)
    bk_gpio_disable_input(SPEAKER_PA_PIN);

    // 3. Enable chức năng Output
    bk_gpio_enable_output(SPEAKER_PA_PIN);

    // 4. Set mức logic để bật Loa (Ví dụ: Active High)
    bk_gpio_set_output_high(SPEAKER_PA_PIN);

    // Nếu mạch là Active Low thì dùng: bk_gpio_set_output_low(SPEAKER_PA_PIN);

    os_printf("Speaker PA Enabled on GPIO %d\n", SPEAKER_PA_PIN);
}

void speaker_pa_disable(void)
{
    // Tắt loa (Ngược lại với lúc bật)
    bk_gpio_set_output_low(SPEAKER_PA_PIN);
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
    // Forward mic data to SD (or other storage) and also forward to speaker for immediate playback
    // SD write is currently stubbed out; keep logging and forward to speaker
    os_printf("%s: data: %p, len: %u\n", __func__, data, len);

    return len;
}

// static void audio_dmic_isr(void)
// {
//     uint32_t dmic_data;
//     os_printf("%s: DMIC ISR triggered\n", __func__);
//     /* Read several samples from DMIC FIFO and forward to DAC */
//     for (uint8_t i = 0; i < 16; i++)
//     {
//         if (bk_aud_dmic_get_fifo_data(&dmic_data) == BK_OK)
//         {
//             os_printf("%s: dmic_data: 0x%08X\n", __func__, dmic_data);
//             bk_aud_dac_write(dmic_data);
//         }
//         else
//         {
//             break;
//         }
//     }
// }

bk_err_t get_fifo(uint32_t *d)
{
    if (bk_aud_dmic_get_fifo_data(d) == BK_OK)
    {
        os_printf("dmic=%08X\n", *d);
        bk_aud_dac_write(*d);
        return BK_OK;
    }
    else
    {
        os_printf("empty\n");
        return BK_FAIL;
    }
}

bk_err_t audio_record_to_sdcard_start(char *file_name, uint32_t samp_rate)
{
    bk_err_t ret = BK_OK;

    ret = tf_mount();
    if (ret != BK_OK)
    {
        os_printf("%s: tfcard mount fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    /* initialize DAC (speaker) */
    {
        aud_dac_config_t dac_cfg = DEFAULT_AUD_DAC_CONFIG();
        dac_cfg.samp_rate = samp_rate;
        dac_cfg.dac_chl = AUD_DAC_CHL_L;

        ret = bk_aud_dac_init(&dac_cfg);
        if (ret != BK_OK)
        {
            os_printf("%s: bk_aud_dac_init fail, ret:%d\n", __func__, ret);
            goto fail;
        }
    }

    /* initialize DMIC */
    {
        aud_dmic_config_t dmic_cfg = DEFAULT_AUD_DMIC_CONFIG();
        dmic_cfg.samp_rate = samp_rate;
        dmic_cfg.dmic_chl = AUD_DMIC_CHL_L;

        ret = bk_aud_dmic_init(&dmic_cfg);
        if (ret != BK_OK)
        {
            os_printf("%s: bk_aud_dmic_init fail, ret:%d\n", __func__, ret);
            goto fail;
        }

        // /* register ISR and enable interrupt (dmic-specific register) */
        // ret = bk_aud_dmic_register_isr(audio_dmic_isr);
        // if (ret != BK_OK)
        // {
        //     os_printf("%s: register dmic isr fail, ret:%d\n", __func__, ret);
        //     goto fail;
        // }

        bk_aud_dmic_set_dmic_wr_threshold(8);
        // bk_aud_dmic_enable_int();

        /* start DAC and DMIC */
        bk_aud_dac_start();
        bk_aud_dmic_start();

        os_printf("%s: DMIC and DAC started at %u Hz\n", __func__, samp_rate);
        speaker_pa_enable();

        // bk_aud_dmic_start_loop_test();
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

bk_err_t dmic_cli_init(void)
{
#if (CLI_CFG_AUD == 1)
    os_printf("%s: cli_aud_init called\n", __func__);
    cli_aud_init();

#endif
    return BK_OK;
}
bk_err_t audio_record_to_sdcard_stop(void)
{
    bk_err_t ret;

    /* disable dmic interrupt and unregister handler */
    bk_aud_dmic_disable_int();
    bk_aud_dmic_register_isr(NULL);

    /* stop and deinit DMIC and DAC */
    ret = bk_aud_dmic_stop();
    if (ret != BK_OK)
    {
        os_printf("%s: bk_aud_dmic_stop fail, ret:%d\n", __func__, ret);
    }
    ret = bk_aud_dmic_deinit();
    if (ret != BK_OK)
    {
        os_printf("%s: bk_aud_dmic_deinit fail, ret:%d\n", __func__, ret);
    }

    ret = bk_aud_dac_stop();
    if (ret != BK_OK)
    {
        os_printf("%s: bk_aud_dac_stop fail, ret:%d\n", __func__, ret);
    }
    ret = bk_aud_dac_deinit();
    if (ret != BK_OK)
    {
        os_printf("%s: bk_aud_dac_deinit fail, ret:%d\n", __func__, ret);
    }

    tf_unmount();

    return BK_OK;
}
