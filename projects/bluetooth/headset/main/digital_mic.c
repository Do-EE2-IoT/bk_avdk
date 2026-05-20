#include "digital_mic.h"

#include <stdbool.h>
#include <stdint.h>

#include <driver/aud_dmic.h>
#include <driver/aud_dmic_types.h>
#include <driver/dma.h>
#include <os/mem.h>
#include <os/os.h>

#include "audio_play.h"

#define TAG "DMIC_I2S"

#define LOGW(...)          \
    do                     \
    {                      \
        os_printf(TAG " WARN: "); \
        os_printf(__VA_ARGS__); \
    } while (0)
#define LOGE(...)          \
    do                     \
    {                      \
        os_printf(TAG " ERROR: "); \
        os_printf(__VA_ARGS__); \
    } while (0)

#define DIGITAL_MIC_TASK_PRIORITY 4
#define DIGITAL_MIC_TASK_STACK_SIZE (4096)
#define DIGITAL_MIC_DEFAULT_FRAME_MS 20U
#define DIGITAL_MIC_DEFAULT_FRAME_COUNT 4U
#define DIGITAL_MIC_MIN_FRAME_COUNT 2U
#define DIGITAL_MIC_MAX_FRAME_COUNT 8U
#define DIGITAL_MIC_DMA_USER DMA_DEV_AUDIO
#define DIGITAL_MIC_DMA_TRANSFER_BYTES 4U

typedef struct
{
    digital_mic_config_t config;
    beken_thread_t thread;
    beken_semaphore_t frame_sema;
    dma_id_t dma_id;
    uint8_t *dma_buffer;
    uint8_t *stereo_buffer;
    uint32_t frame_bytes;
    uint32_t frame_count;
    volatile uint32_t dma_frame_index;
    volatile uint32_t dma_frame_bytes;
    volatile uint32_t read_frame_index;
    volatile uint32_t ready_frames;
    volatile uint32_t received_bytes;
    volatile uint32_t dropped_frames;
    volatile bool running;
    bool dmic_started;
    bool i2s_started;
} digital_mic_ctx_t;

static digital_mic_ctx_t s_dmic = {
    .dma_id = DMA_ID_MAX,
};

static uint32_t digital_mic_align4(uint32_t value)
{
    return (value + 3U) & ~3U;
}

static uint32_t digital_mic_mono_to_stereo(uint8_t *dst, const uint8_t *src, uint32_t src_len)
{
    const int16_t *mono = (const int16_t *)src;
    int16_t *stereo = (int16_t *)dst;
    uint32_t samples = src_len / sizeof(int16_t);
    uint32_t index;

    for (index = 0; index < samples; ++index)
    {
        int16_t sample = mono[index];
        stereo[index * 2U] = sample;
        stereo[index * 2U + 1U] = sample;
    }

    return samples * 2U * sizeof(int16_t);
}

static void digital_mic_dma_finish_isr(dma_id_t dma_id)
{
    (void)dma_id;

    s_dmic.received_bytes += DIGITAL_MIC_DMA_TRANSFER_BYTES;
    s_dmic.dma_frame_bytes += DIGITAL_MIC_DMA_TRANSFER_BYTES;

    if (s_dmic.dma_frame_bytes < s_dmic.frame_bytes)
    {
        return;
    }

    s_dmic.dma_frame_bytes = 0;
    s_dmic.dma_frame_index++;
    if (s_dmic.dma_frame_index >= s_dmic.frame_count)
    {
        s_dmic.dma_frame_index = 0;
    }

    if (s_dmic.ready_frames < s_dmic.frame_count)
    {
        s_dmic.ready_frames++;
    }
    else
    {
        s_dmic.dropped_frames++;
        s_dmic.read_frame_index++;
        if (s_dmic.read_frame_index >= s_dmic.frame_count)
        {
            s_dmic.read_frame_index = 0;
        }
    }

    if (s_dmic.frame_sema)
    {
        rtos_set_semaphore(&s_dmic.frame_sema);
    }
}

static bk_err_t digital_mic_dma_init(void)
{
    bk_err_t ret;
    uint32_t dmic_fifo_addr = 0;
    dma_config_t dma_config;

    os_memset(&dma_config, 0, sizeof(dma_config));

    ret = bk_dma_driver_init();
    if (ret != BK_OK)
    {
        LOGE("%s bk_dma_driver_init failed: %d\r\n", __func__, ret);
        return ret;
    }

    ret = bk_aud_dmic_get_fifo_addr(&dmic_fifo_addr);
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_get_fifo_addr failed: %d\r\n", __func__, ret);
        return ret;
    }

    s_dmic.dma_id = bk_dma_alloc(DIGITAL_MIC_DMA_USER);
    if ((s_dmic.dma_id < DMA_ID_0) || (s_dmic.dma_id >= DMA_ID_MAX))
    {
        LOGE("%s bk_dma_alloc failed\r\n", __func__);
        s_dmic.dma_id = DMA_ID_MAX;
        return BK_FAIL;
    }

    dma_config.mode = DMA_WORK_MODE_REPEAT;
    dma_config.chan_prio = 1;
    dma_config.src.dev = DMA_DEV_AUDIO;
    dma_config.src.width = DMA_DATA_WIDTH_32BITS;
    dma_config.src.addr_inc_en = DMA_ADDR_INC_ENABLE;
    dma_config.src.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
    dma_config.src.start_addr = dmic_fifo_addr;
    dma_config.src.end_addr = dmic_fifo_addr + 4U;
    dma_config.dst.dev = DMA_DEV_DTCM;
    dma_config.dst.width = DMA_DATA_WIDTH_32BITS;
    dma_config.dst.addr_inc_en = DMA_ADDR_INC_ENABLE;
    dma_config.dst.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
    dma_config.dst.start_addr = (uint32_t)s_dmic.dma_buffer;
    dma_config.dst.end_addr = (uint32_t)s_dmic.dma_buffer + (s_dmic.frame_bytes * s_dmic.frame_count);

    ret = bk_dma_init(s_dmic.dma_id, &dma_config);
    if (ret != BK_OK)
    {
        LOGE("%s bk_dma_init failed: %d\r\n", __func__, ret);
        goto fail;
    }

    ret = bk_dma_set_transfer_len(s_dmic.dma_id, DIGITAL_MIC_DMA_TRANSFER_BYTES);
    if (ret != BK_OK)
    {
        LOGE("%s bk_dma_set_transfer_len failed: %d\r\n", __func__, ret);
        goto fail;
    }

#if (CONFIG_SPE)
    bk_dma_set_dest_sec_attr(s_dmic.dma_id, DMA_ATTR_SEC);
    bk_dma_set_src_sec_attr(s_dmic.dma_id, DMA_ATTR_SEC);
#endif

    ret = bk_dma_register_isr(s_dmic.dma_id, NULL, digital_mic_dma_finish_isr);
    if (ret != BK_OK)
    {
        LOGE("%s bk_dma_register_isr failed: %d\r\n", __func__, ret);
        goto fail;
    }

    ret = bk_dma_enable_finish_interrupt(s_dmic.dma_id);
    if (ret != BK_OK)
    {
        LOGE("%s bk_dma_enable_finish_interrupt failed: %d\r\n", __func__, ret);
        goto fail;
    }

    LOGW("DMIC DMA ready, fifo=0x%08lX buffer=0x%08lX frame=%lu frames=%lu transfer=%lu\r\n",
         (unsigned long)dmic_fifo_addr,
         (unsigned long)s_dmic.dma_buffer,
         (unsigned long)s_dmic.frame_bytes,
         (unsigned long)s_dmic.frame_count,
         (unsigned long)DIGITAL_MIC_DMA_TRANSFER_BYTES);

    return BK_OK;

fail:
    if (s_dmic.dma_id != DMA_ID_MAX)
    {
        bk_dma_deinit(s_dmic.dma_id);
        bk_dma_free(DIGITAL_MIC_DMA_USER, s_dmic.dma_id);
        s_dmic.dma_id = DMA_ID_MAX;
    }
    return ret;
}

static void digital_mic_cleanup(void)
{
    s_dmic.running = false;

    if (s_dmic.dmic_started)
    {
        bk_aud_dmic_stop();
        s_dmic.dmic_started = false;
    }

    if (s_dmic.dma_id != DMA_ID_MAX)
    {
        bk_dma_stop(s_dmic.dma_id);
        bk_dma_disable_finish_interrupt(s_dmic.dma_id);
        bk_dma_register_isr(s_dmic.dma_id, NULL, NULL);
        bk_dma_deinit(s_dmic.dma_id);
        bk_dma_free(DIGITAL_MIC_DMA_USER, s_dmic.dma_id);
        s_dmic.dma_id = DMA_ID_MAX;
    }

    bk_aud_dmic_deinit();

    if (s_dmic.i2s_started)
    {
        audio_play_pcm_i2s_stop();
        s_dmic.i2s_started = false;
    }

    if (s_dmic.frame_sema)
    {
        rtos_deinit_semaphore(&s_dmic.frame_sema);
        s_dmic.frame_sema = NULL;
    }

    if (s_dmic.dma_buffer)
    {
        os_free(s_dmic.dma_buffer);
        s_dmic.dma_buffer = NULL;
    }

    if (s_dmic.stereo_buffer)
    {
        os_free(s_dmic.stereo_buffer);
        s_dmic.stereo_buffer = NULL;
    }
}

static void digital_mic_log_status(const char *reason)
{
    bk_err_t ret;
    uint32_t status = 0;

    ret = bk_aud_dmic_get_status(&status);
    if (ret == BK_OK)
    {
        LOGW("DMIC status %s: fifo_status=0x%08lX received=%lu ready=%lu dropped=%lu dma_frame=%lu frame_bytes=%lu\r\n",
             reason,
             (unsigned long)status,
             (unsigned long)s_dmic.received_bytes,
             (unsigned long)s_dmic.ready_frames,
             (unsigned long)s_dmic.dropped_frames,
             (unsigned long)s_dmic.dma_frame_index,
             (unsigned long)s_dmic.dma_frame_bytes);
    }
    else
    {
        LOGW("DMIC status %s: read failed=%d received=%lu ready=%lu dropped=%lu\r\n",
             reason,
             ret,
             (unsigned long)s_dmic.received_bytes,
             (unsigned long)s_dmic.ready_frames,
             (unsigned long)s_dmic.dropped_frames);
    }
}

static void digital_mic_task(void *arg)
{
    bk_err_t ret;
    aud_dmic_config_t dmic_config = DEFAULT_AUD_DMIC_CONFIG();
    uint32_t log_counter = 0;

    (void)arg;

    LOGW("DMIC->I2S task start, rate=%lu frame=%lums\r\n",
         (unsigned long)s_dmic.config.sample_rate,
         (unsigned long)s_dmic.config.frame_ms);

    ret = audio_play_pcm_i2s_start(s_dmic.config.sample_rate, 2, 16);
    if (ret != BK_OK)
    {
        LOGE("%s audio_play_pcm_i2s_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.i2s_started = true;

    dmic_config.samp_rate = s_dmic.config.sample_rate;
    dmic_config.dmic_chl = AUD_DMIC_CHL_LR;
    ret = bk_aud_dmic_init(&dmic_config);
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_init failed: %d\r\n", __func__, ret);
        goto exit;
    }

    ret = digital_mic_dma_init();
    if (ret != BK_OK)
    {
        goto exit;
    }

    ret = bk_dma_start(s_dmic.dma_id);
    if (ret != BK_OK)
    {
        LOGE("%s bk_dma_start failed: %d\r\n", __func__, ret);
        goto exit;
    }

    ret = bk_aud_dmic_start();
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dmic_started = true;

    LOGW("DMIC->I2S running: DMIC GPIO_8 CLK, GPIO_9 DAT, sample_rate=%lu\r\n",
         (unsigned long)s_dmic.config.sample_rate);
    digital_mic_log_status("after start");

    while (s_dmic.running)
    {
        ret = rtos_get_semaphore(&s_dmic.frame_sema, 1000);
        if (ret != BK_OK)
        {
            LOGW("DMIC wait frame timeout, received=%lu dropped=%lu\r\n",
                 (unsigned long)s_dmic.received_bytes,
                 (unsigned long)s_dmic.dropped_frames);
            digital_mic_log_status("timeout");
            continue;
        }

        while (s_dmic.ready_frames > 0U)
        {
            uint32_t frame_index = s_dmic.read_frame_index;
            uint8_t *mono = s_dmic.dma_buffer + frame_index * s_dmic.frame_bytes;
            uint32_t stereo_len = digital_mic_mono_to_stereo(s_dmic.stereo_buffer, mono, s_dmic.frame_bytes);

            s_dmic.read_frame_index++;
            if (s_dmic.read_frame_index >= s_dmic.frame_count)
            {
                s_dmic.read_frame_index = 0;
            }
            s_dmic.ready_frames--;

            ret = audio_play_pcm_i2s_write(s_dmic.stereo_buffer, stereo_len, s_dmic.config.write_timeout_ms);
            if (ret != BK_OK)
            {
                LOGE("DMIC push I2S failed: %d, frame=%lu mono=%lu stereo=%lu\r\n",
                     ret,
                     (unsigned long)frame_index,
                     (unsigned long)s_dmic.frame_bytes,
                     (unsigned long)stereo_len);
                break;
            }

            if ((log_counter++ % 50U) == 0U)
            {
                LOGW("DMIC bytes in=%lu, pushed I2S=%lu, frame=%lu ready=%lu dropped=%lu\r\n",
                     (unsigned long)s_dmic.received_bytes,
                     (unsigned long)stereo_len,
                     (unsigned long)frame_index,
                     (unsigned long)s_dmic.ready_frames,
                     (unsigned long)s_dmic.dropped_frames);
            }
        }
    }

exit:
    digital_mic_cleanup();
    s_dmic.thread = NULL;
    LOGW("DMIC->I2S task exit\r\n");
    rtos_delete_thread(NULL);
}

void digital_mic_init_default_config(digital_mic_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config->sample_rate = DIGITAL_MIC_SAMPLE_RATE_DEFAULT;
    config->frame_ms = DIGITAL_MIC_DEFAULT_FRAME_MS;
    config->frame_count = DIGITAL_MIC_DEFAULT_FRAME_COUNT;
    config->write_timeout_ms = 200;
}

bk_err_t digital_mic_start(const digital_mic_config_t *config)
{
    bk_err_t ret;
    digital_mic_config_t local_config;

    if (s_dmic.running)
    {
        return BK_OK;
    }

    if (config == NULL)
    {
        digital_mic_init_default_config(&local_config);
        config = &local_config;
    }

    os_memset(&s_dmic, 0, sizeof(s_dmic));
    s_dmic.dma_id = DMA_ID_MAX;
    s_dmic.config = *config;

    if (s_dmic.config.sample_rate == 0U)
    {
        s_dmic.config.sample_rate = DIGITAL_MIC_SAMPLE_RATE_DEFAULT;
    }
    if (s_dmic.config.frame_ms == 0U)
    {
        s_dmic.config.frame_ms = DIGITAL_MIC_DEFAULT_FRAME_MS;
    }
    if (s_dmic.config.frame_count < DIGITAL_MIC_MIN_FRAME_COUNT)
    {
        s_dmic.config.frame_count = DIGITAL_MIC_MIN_FRAME_COUNT;
    }
    if (s_dmic.config.frame_count > DIGITAL_MIC_MAX_FRAME_COUNT)
    {
        s_dmic.config.frame_count = DIGITAL_MIC_MAX_FRAME_COUNT;
    }
    if (s_dmic.config.write_timeout_ms == 0U)
    {
        s_dmic.config.write_timeout_ms = 200;
    }

    s_dmic.frame_bytes = digital_mic_align4((s_dmic.config.sample_rate * s_dmic.config.frame_ms * sizeof(int16_t)) / 1000U);
    s_dmic.frame_count = s_dmic.config.frame_count;

    s_dmic.dma_buffer = (uint8_t *)os_malloc(s_dmic.frame_bytes * s_dmic.frame_count);
    s_dmic.stereo_buffer = (uint8_t *)os_malloc(s_dmic.frame_bytes * 2U);
    if ((s_dmic.dma_buffer == NULL) || (s_dmic.stereo_buffer == NULL))
    {
        LOGE("%s malloc failed, frame=%lu frames=%lu\r\n",
             __func__, (unsigned long)s_dmic.frame_bytes, (unsigned long)s_dmic.frame_count);
        digital_mic_cleanup();
        return BK_ERR_NO_MEM;
    }
    os_memset(s_dmic.dma_buffer, 0, s_dmic.frame_bytes * s_dmic.frame_count);
    os_memset(s_dmic.stereo_buffer, 0, s_dmic.frame_bytes * 2U);

    ret = rtos_init_semaphore(&s_dmic.frame_sema, 1);
    if (ret != BK_OK)
    {
        LOGE("%s rtos_init_semaphore failed: %d\r\n", __func__, ret);
        digital_mic_cleanup();
        return ret;
    }

    s_dmic.running = true;
    ret = rtos_create_thread(&s_dmic.thread,
                             DIGITAL_MIC_TASK_PRIORITY,
                             "digital_mic",
                             (beken_thread_function_t)digital_mic_task,
                             DIGITAL_MIC_TASK_STACK_SIZE,
                             NULL);
    if (ret != BK_OK)
    {
        LOGE("%s rtos_create_thread failed: %d\r\n", __func__, ret);
        digital_mic_cleanup();
        return ret;
    }

    return BK_OK;
}

bk_err_t digital_mic_stop(void)
{
    if (!s_dmic.running)
    {
        return BK_OK;
    }

    s_dmic.running = false;
    if (s_dmic.frame_sema)
    {
        rtos_set_semaphore(&s_dmic.frame_sema);
    }

    while (s_dmic.thread)
    {
        rtos_delay_milliseconds(20);
    }

    return BK_OK;
}

bool digital_mic_is_running(void)
{
    return s_dmic.running;
}
