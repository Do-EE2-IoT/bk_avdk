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
#include <driver/aud_dmic.h>
#include <driver/aud_dac.h>
#include <driver/dma.h>
#include "ff.h"
#include "diskio.h"
#include "driver/gpio.h"
#include "gpio_driver.h"

#define SPEAKER_PA_PIN GPIO_13

#define TAG "AUD_DMIC_HTTP"
#define DMIC_ENABLE_SPEAKER_LOOPBACK 0
#define DMIC_HTTP_STREAM_CHANNELS 1
#define DMIC_SOFTWARE_GAIN 4
#define DMIC_STREAM_FRAME_SAMPLES 320
#define DMIC_LOG_INTERVAL_FRAMES 10
#define DMIC_LOG_SAMPLE_COUNT 32
#define DMIC_FLUSH_IDLE_LOG_POLLS 1000
#define DMIC_INITIAL_WORD_LOGS 8
#define DMIC_DMA_BUFFER_FRAMES 2
#define DMIC_DMA_FRAME_WORDS DMIC_STREAM_FRAME_SAMPLES
#define DMIC_DMA_TOTAL_WORDS (DMIC_DMA_BUFFER_FRAMES * DMIC_DMA_FRAME_WORDS)
#define DMIC_DMA_TOTAL_BYTES (DMIC_DMA_TOTAL_WORDS * sizeof(uint32_t))

static int16_t s_dmic_stream_frame[DMIC_STREAM_FRAME_SAMPLES];
static uint32_t s_dmic_stream_samples = 0;
static uint32_t s_dmic_dma_words[DMIC_DMA_TOTAL_WORDS] __attribute__((aligned(4)));
static uint32_t s_dmic_debug_words[DMIC_INITIAL_WORD_LOGS];
static uint32_t s_dmic_stream_frames = 0;
static uint32_t s_dmic_flush_idle_polls = 0;
static uint32_t s_dmic_initial_word_logs = 0;
static uint32_t s_dmic_initial_word_printed = 0;
static beken_semaphore_t s_dmic_dma_sem = NULL;
static dma_id_t s_dmic_dma_id = DMA_ID_MAX;
static volatile uint32_t s_dmic_dma_ready_mask = 0;
static volatile uint32_t s_dmic_dma_overrun = 0;
static volatile uint32_t s_dmic_dma_half_count = 0;
static volatile uint32_t s_dmic_dma_finish_count = 0;

static void dmic_dma_deinit(void);

static int16_t dmic_apply_gain(int16_t sample)
{
    int32_t amplified = (int32_t)sample * DMIC_SOFTWARE_GAIN;

    if (amplified > 32767)
        return 32767;
    if (amplified < -32768)
        return -32768;

    return (int16_t)amplified;
}

static int16_t dmic_get_low_sample(uint32_t fifo_word)
{
    return (int16_t)(fifo_word & 0xffff);
}

static int16_t dmic_get_high_sample(uint32_t fifo_word)
{
    return (int16_t)((fifo_word >> 16) & 0xffff);
}

static int16_t dmic_get_stream_sample(uint32_t fifo_word)
{
    return dmic_get_high_sample(fifo_word);
}

static void dmic_log_frame(const int16_t *pcm, uint32_t samples)
{
    int16_t mic_min = 32767;
    int16_t mic_max = -32768;
    int64_t mic_abs_sum = 0;
    uint32_t checked = samples < DMIC_LOG_SAMPLE_COUNT ? samples : DMIC_LOG_SAMPLE_COUNT;

    for (uint32_t i = 0; i < checked; i++)
    {
        int16_t mic = pcm[i];

        if (mic < mic_min)
            mic_min = mic;
        if (mic > mic_max)
            mic_max = mic;

        mic_abs_sum += mic >= 0 ? mic : -mic;
    }

    os_printf("%s: frame=%lu samples=%lu checked=%lu "
              "dmic1_high[min=%d max=%d avg_abs=%ld]\n",
              TAG,
              (unsigned long)s_dmic_stream_frames,
              (unsigned long)samples,
              (unsigned long)checked,
              mic_min,
              mic_max,
              (long)(checked ? mic_abs_sum / checked : 0));
}

static void dmic_stream_push_sample(int16_t sample)
{
    s_dmic_stream_frame[s_dmic_stream_samples++] = dmic_apply_gain(sample);

    if (s_dmic_stream_samples >= DMIC_STREAM_FRAME_SAMPLES)
    {
        s_dmic_stream_frames++;

        if ((s_dmic_stream_frames % DMIC_LOG_INTERVAL_FRAMES) == 0)
            dmic_log_frame(s_dmic_stream_frame, DMIC_STREAM_FRAME_SAMPLES);

        audio_http_stream_push_frame((const uint8_t *)s_dmic_stream_frame,
                                     DMIC_STREAM_FRAME_SAMPLES * sizeof(int16_t));

        s_dmic_stream_samples = 0;
    }
}

static void dmic_dma_signal_frame(uint32_t frame_index)
{
    uint32_t frame_mask = (1U << frame_index);

    if (s_dmic_dma_ready_mask & frame_mask)
        s_dmic_dma_overrun++;

    s_dmic_dma_ready_mask |= frame_mask;

    if (s_dmic_dma_sem)
        rtos_set_semaphore(&s_dmic_dma_sem);
}

static void dmic_dma_half_isr(dma_id_t dma_id)
{
    (void)dma_id;
    s_dmic_dma_half_count++;
    dmic_dma_signal_frame(0);
}

static void dmic_dma_finish_isr(dma_id_t dma_id)
{
    (void)dma_id;
    s_dmic_dma_finish_count++;
    dmic_dma_signal_frame(1);
}

static bk_err_t dmic_dma_config(void)
{
    bk_err_t ret;
    dma_config_t dma_config = {0};
    uint32_t dmic_fifo_addr;

    ret = bk_dma_driver_init();
    if (ret != BK_OK)
    {
        os_printf("%s: bk_dma_driver_init fail, ret:%d\n", __func__, ret);
        return ret;
    }

    dmic_dma_deinit();

    ret = rtos_init_semaphore_ex(&s_dmic_dma_sem, 8, 0);
    if (ret != BK_OK)
    {
        os_printf("%s: rtos_init_semaphore_ex fail, ret:%d\n", __func__, ret);
        return ret;
    }

    s_dmic_dma_id = bk_dma_alloc(DMA_DEV_AUDIO_RX);
    if ((s_dmic_dma_id < DMA_ID_0) || (s_dmic_dma_id >= DMA_ID_MAX))
    {
        os_printf("%s: bk_dma_alloc fail, id:%d\n", __func__, s_dmic_dma_id);
        s_dmic_dma_id = DMA_ID_MAX;
        dmic_dma_deinit();
        return BK_FAIL;
    }

    ret = bk_aud_dmic_get_fifo_addr(&dmic_fifo_addr);
    if (ret != BK_OK)
    {
        os_printf("%s: bk_aud_dmic_get_fifo_addr fail, ret:%d\n", __func__, ret);
        dmic_dma_deinit();
        return ret;
    }

    os_memset(s_dmic_dma_words, 0, sizeof(s_dmic_dma_words));
    s_dmic_dma_ready_mask = 0;
    s_dmic_dma_overrun = 0;
    s_dmic_dma_half_count = 0;
    s_dmic_dma_finish_count = 0;
    s_dmic_initial_word_logs = 0;
    s_dmic_initial_word_printed = 0;

    dma_config.mode = DMA_WORK_MODE_REPEAT;
    dma_config.chan_prio = 1;
    dma_config.src.dev = DMA_DEV_AUDIO_RX;
    dma_config.src.width = DMA_DATA_WIDTH_32BITS;
    dma_config.src.addr_inc_en = DMA_ADDR_INC_ENABLE;
    dma_config.src.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
    dma_config.src.start_addr = dmic_fifo_addr;
    dma_config.src.end_addr = dmic_fifo_addr + 4;
    dma_config.dst.dev = DMA_DEV_DTCM;
    dma_config.dst.width = DMA_DATA_WIDTH_32BITS;
    dma_config.dst.addr_inc_en = DMA_ADDR_INC_ENABLE;
    dma_config.dst.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
    dma_config.dst.start_addr = (uint32_t)&s_dmic_dma_words[0];
    dma_config.dst.end_addr = (uint32_t)&s_dmic_dma_words[DMIC_DMA_TOTAL_WORDS];

    ret = bk_dma_init(s_dmic_dma_id, &dma_config);
    if (ret != BK_OK)
    {
        os_printf("%s: bk_dma_init fail, ret:%d\n", __func__, ret);
        dmic_dma_deinit();
        return ret;
    }

    ret = bk_dma_set_transfer_len(s_dmic_dma_id, DMIC_DMA_TOTAL_BYTES);
    if (ret != BK_OK)
    {
        os_printf("%s: bk_dma_set_transfer_len fail, ret:%d\n", __func__, ret);
        dmic_dma_deinit();
        return ret;
    }

#if (CONFIG_SPE)
    bk_dma_set_dest_sec_attr(s_dmic_dma_id, DMA_ATTR_SEC);
    bk_dma_set_src_sec_attr(s_dmic_dma_id, DMA_ATTR_SEC);
#endif

    ret = bk_dma_register_isr(s_dmic_dma_id, dmic_dma_half_isr, dmic_dma_finish_isr);
    if (ret != BK_OK)
    {
        os_printf("%s: bk_dma_register_isr fail, ret:%d\n", __func__, ret);
        dmic_dma_deinit();
        return ret;
    }

    bk_dma_enable_half_finish_interrupt(s_dmic_dma_id);
    bk_dma_enable_finish_interrupt(s_dmic_dma_id);

    ret = bk_dma_start(s_dmic_dma_id);
    if (ret != BK_OK)
    {
        os_printf("%s: bk_dma_start fail, ret:%d\n", __func__, ret);
        dmic_dma_deinit();
        return ret;
    }

    os_printf("%s: dmic dma started, id=%d frame_words=%u total=%u bytes\n",
              __func__, s_dmic_dma_id, DMIC_DMA_FRAME_WORDS, DMIC_DMA_TOTAL_BYTES);

    return BK_OK;
}

static void dmic_dma_deinit(void)
{
    if (s_dmic_dma_id != DMA_ID_MAX)
    {
        bk_dma_stop(s_dmic_dma_id);
        bk_dma_disable_half_finish_interrupt(s_dmic_dma_id);
        bk_dma_disable_finish_interrupt(s_dmic_dma_id);
        bk_dma_register_isr(s_dmic_dma_id, NULL, NULL);
        bk_dma_deinit(s_dmic_dma_id);
        bk_dma_free(DMA_DEV_AUDIO_RX, s_dmic_dma_id);
        s_dmic_dma_id = DMA_ID_MAX;
    }

    if (s_dmic_dma_sem)
    {
        rtos_deinit_semaphore(&s_dmic_dma_sem);
        s_dmic_dma_sem = NULL;
    }

    s_dmic_dma_ready_mask = 0;
}

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


bk_err_t get_fifo(uint32_t *d)
{
    bk_err_t ret;
    uint32_t ready_mask;

    (void)d;

    if (s_dmic_dma_id == DMA_ID_MAX)
        return BK_FAIL;

    ret = rtos_get_semaphore(&s_dmic_dma_sem, 10);
    if (ret != BK_OK)
    {
        s_dmic_flush_idle_polls++;
        if ((s_dmic_flush_idle_polls % DMIC_FLUSH_IDLE_LOG_POLLS) == 0)
        {
            uint32_t dmic_status = 0;
            bk_aud_dmic_get_status(&dmic_status);
            os_printf("%s: dma wait timeout status=0x%08lx polls=%lu half=%lu finish=%lu overrun=%lu\n",
                      TAG,
                      (unsigned long)dmic_status,
                      (unsigned long)s_dmic_flush_idle_polls,
                      (unsigned long)s_dmic_dma_half_count,
                      (unsigned long)s_dmic_dma_finish_count,
                      (unsigned long)s_dmic_dma_overrun);
        }
        return BK_FAIL;
    }

    s_dmic_flush_idle_polls = 0;

    GLOBAL_INT_DECLARATION();
    GLOBAL_INT_DISABLE();
    ready_mask = s_dmic_dma_ready_mask;
    s_dmic_dma_ready_mask = 0;
    GLOBAL_INT_RESTORE();

    for (uint32_t frame_index = 0; frame_index < DMIC_DMA_BUFFER_FRAMES; frame_index++)
    {
        if ((ready_mask & (1U << frame_index)) == 0)
            continue;

        uint32_t *frame_words = &s_dmic_dma_words[frame_index * DMIC_DMA_FRAME_WORDS];

        for (uint32_t i = 0; i < DMIC_DMA_FRAME_WORDS; i++)
        {
            uint32_t word = frame_words[i];

            if (s_dmic_initial_word_logs < DMIC_INITIAL_WORD_LOGS)
                s_dmic_debug_words[s_dmic_initial_word_logs++] = word;

            dmic_stream_push_sample(dmic_get_stream_sample(word));
        }
    }

    while (s_dmic_initial_word_printed < s_dmic_initial_word_logs)
    {
        uint32_t word = s_dmic_debug_words[s_dmic_initial_word_printed];

        os_printf("%s: dma_word[%lu]=0x%08lx low=%d high=%d stream=%d\n",
                  TAG,
                  (unsigned long)s_dmic_initial_word_printed,
                  (unsigned long)word,
                  dmic_get_low_sample(word),
                  dmic_get_high_sample(word),
                  dmic_get_stream_sample(word));
        s_dmic_initial_word_printed++;
    }

    return BK_OK;
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

#if DMIC_ENABLE_SPEAKER_LOOPBACK
    /* initialize DAC (speaker) */
    {
        aud_dac_config_t dac_cfg = DEFAULT_AUD_DAC_CONFIG();
        dac_cfg.samp_rate = samp_rate;
        dac_cfg.dac_chl = AUD_DAC_CHL_LR;

        ret = bk_aud_dac_init(&dac_cfg);
        if (ret != BK_OK)
        {
            os_printf("%s: bk_aud_dac_init fail, ret:%d\n", __func__, ret);
            goto fail;
        }
    }
#endif

    ret = audio_http_stream_start(samp_rate, DMIC_HTTP_STREAM_CHANNELS);
    if (ret != BK_OK)
    {
        os_printf("%s: audio_http_stream_start fail, ret:%d\n", __func__, ret);
        goto fail;
    }

    /* initialize DMIC */
    {
        aud_dmic_config_t dmic_cfg = DEFAULT_AUD_DMIC_CONFIG();
        dmic_cfg.samp_rate = samp_rate;
        dmic_cfg.dmic_chl = AUD_DMIC_CHL_LR;

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

        ret = dmic_dma_config();
        if (ret != BK_OK)
        {
            goto fail;
        }

#if DMIC_ENABLE_SPEAKER_LOOPBACK
        /* start DAC and DMIC */
        bk_aud_dac_start();
        speaker_pa_enable();
#endif
        bk_aud_dmic_start();

        os_printf("%s: DMIC started at %u Hz, streaming DMIC1 high channel\n", __func__, samp_rate);

        // bk_aud_dmic_start_loop_test();
    }

    return BK_OK;

fail:
    /* try to clean up any partial init */
    bk_aud_dmic_stop();
    bk_aud_dmic_deinit();
    dmic_dma_deinit();
#if DMIC_ENABLE_SPEAKER_LOOPBACK
    bk_aud_dac_stop();
    bk_aud_dac_deinit();
#endif
    audio_http_stream_stop();

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

#if DMIC_ENABLE_SPEAKER_LOOPBACK
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
#endif

    dmic_dma_deinit();
    audio_http_stream_stop();

    tf_unmount();

    return BK_OK;
}
