#include "digital_mic.h"

#include <stdbool.h>
#include <stdint.h>

#include <driver/aud_dac.h>
#include <driver/aud_dac_types.h>
#include <driver/aud_dmic.h>
#include <driver/aud_dmic_types.h>
#include <os/os.h>

#include "audio_play.h"

#define TAG "DMIC_I2S"

#define LOGW(...)                 \
    do                            \
    {                             \
        os_printf(TAG " WARN: "); \
        os_printf(__VA_ARGS__);   \
    } while (0)
#define LOGE(...)                  \
    do                             \
    {                              \
        os_printf(TAG " ERROR: "); \
        os_printf(__VA_ARGS__);    \
    } while (0)

#define DIGITAL_MIC_TASK_PRIORITY 4
#define DIGITAL_MIC_TASK_STACK_SIZE (4096)
#define DIGITAL_MIC_DEFAULT_FRAME_MS 20U
#define DIGITAL_MIC_DEFAULT_FRAME_COUNT 4U
#define DIGITAL_MIC_FIFO_THRESHOLD 8U
#define DIGITAL_MIC_I2S_CHANNELS 2U
#define DIGITAL_MIC_I2S_BITS_PER_SAMPLE 16U
#define DIGITAL_MIC_LOG_EVERY_WORDS 256U

typedef struct
{
    digital_mic_config_t config;
    beken_thread_t thread;
    volatile uint32_t received_words;
    volatile uint32_t empty_polls;
    volatile bool running;
    bool i2s_started;
    bool dac_initialized;
    bool dac_started;
    bool dmic_started;
} digital_mic_ctx_t;

static digital_mic_ctx_t s_dmic;

static bk_err_t digital_mic_get_fifo(uint32_t *data)
{
    uint32_t dmic_status = 0;
    bk_err_t ret;

    ret = bk_aud_dmic_get_status(&dmic_status);
    if (ret != BK_OK)
    {
        LOGW("DMIC get status failed: %d\r\n", ret);
        return ret;
    }

    if (dmic_status & (AUD_DMIC_NEAR_FULL_MASK | AUD_DMIC_FIFO_FULL_MASK))
    {
        ret = bk_aud_dmic_get_fifo_data(data);
        if (ret != BK_OK)
        {
            LOGW("DMIC get fifo data failed: %d status=0x%08lX\r\n",
                 ret,
                 (unsigned long)dmic_status);
            return ret;
        }

        return BK_OK;
    }

    return BK_FAIL;
}

static void digital_mic_log_status(const char *reason)
{
    uint32_t status = 0;
    bk_err_t ret;

    ret = bk_aud_dmic_get_status(&status);
    if (ret == BK_OK)
    {
        LOGW("DMIC status %s: fifo_status=0x%08lX received_words=%lu empty_polls=%lu\r\n",
             reason,
             (unsigned long)status,
             (unsigned long)s_dmic.received_words,
             (unsigned long)s_dmic.empty_polls);
    }
    else
    {
        LOGW("DMIC status %s: read failed=%d received_words=%lu empty_polls=%lu\r\n",
             reason,
             ret,
             (unsigned long)s_dmic.received_words,
             (unsigned long)s_dmic.empty_polls);
    }
}

static void digital_mic_cleanup(void)
{
    s_dmic.running = false;

    if (s_dmic.dmic_started)
    {
        bk_aud_dmic_stop();
        s_dmic.dmic_started = false;
    }

    bk_aud_dmic_deinit();

    if (s_dmic.dac_started)
    {
        bk_aud_dac_stop();
        s_dmic.dac_started = false;
    }

    if (s_dmic.dac_initialized)
    {
        bk_aud_dac_deinit();
        s_dmic.dac_initialized = false;
    }

    if (s_dmic.i2s_started)
    {
        audio_play_pcm_i2s_stop();
        s_dmic.i2s_started = false;
    }
}

static void digital_mic_task(void *arg)
{
    bk_err_t ret;
    aud_dac_config_t dac_config = DEFAULT_AUD_DAC_CONFIG();
    aud_dmic_config_t dmic_config = DEFAULT_AUD_DMIC_CONFIG();
    uint32_t dmic_data = 0;
    int16_t pcm_frame[2];

    (void)arg;

    LOGW("DMIC polling task start, rate=%lu threshold=%lu\r\n",
         (unsigned long)s_dmic.config.sample_rate,
         (unsigned long)DIGITAL_MIC_FIFO_THRESHOLD);

    ret = audio_play_pcm_i2s_start(s_dmic.config.sample_rate,
                                   DIGITAL_MIC_I2S_CHANNELS,
                                   DIGITAL_MIC_I2S_BITS_PER_SAMPLE);
    if (ret != BK_OK)
    {
        LOGE("%s audio_play_pcm_i2s_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.i2s_started = true;

    dac_config.samp_rate = s_dmic.config.sample_rate;
    dac_config.dac_chl = AUD_DAC_CHL_LR;

    ret = bk_aud_dac_init(&dac_config);
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dac_init failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dac_initialized = true;

    dmic_config.samp_rate = s_dmic.config.sample_rate;
    dmic_config.dmic_chl = AUD_DMIC_CHL_LR;

    ret = bk_aud_dmic_init(&dmic_config);
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_init failed: %d\r\n", __func__, ret);
        goto exit;
    }

    ret = bk_aud_dmic_set_dmic_wr_threshold(DIGITAL_MIC_FIFO_THRESHOLD);
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_set_dmic_wr_threshold failed: %d\r\n", __func__, ret);
        goto exit;
    }

    ret = bk_aud_dac_start();
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dac_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dac_started = true;

    ret = bk_aud_dmic_start();
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dmic_started = true;

    LOGW("DMIC->I2S running: GPIO_8 CLK, GPIO_9 DAT, sample_rate=%lu\r\n",
         (unsigned long)s_dmic.config.sample_rate);
    digital_mic_log_status("after start");

    while (s_dmic.running)
    {
        ret = digital_mic_get_fifo(&dmic_data);
        if (ret == BK_OK)
        {
            s_dmic.received_words++;
            pcm_frame[0] = (int16_t)(dmic_data & 0xFFFFU);
            pcm_frame[1] = (int16_t)((dmic_data >> 16) & 0xFFFFU);

            ret = audio_play_pcm_i2s_write((uint8_t *)pcm_frame,
                                           sizeof(pcm_frame),
                                           s_dmic.config.write_timeout_ms);
            if (ret != BK_OK)
            {
                LOGW("DMIC I2S write failed: %d word=0x%08lX\r\n",
                     ret,
                     (unsigned long)dmic_data);
            }
            else if ((s_dmic.received_words % DIGITAL_MIC_LOG_EVERY_WORDS) == 0U)
            {
                LOGW("DMIC FIFO data: 0x%08lX\r\n", (unsigned long)dmic_data);
            }
            continue;
        }

        s_dmic.empty_polls++;
        if ((s_dmic.empty_polls % 1000U) == 0U)
        {
            digital_mic_log_status("poll empty");
        }
        rtos_delay_milliseconds(1);
    }

exit:
    digital_mic_cleanup();
    s_dmic.thread = NULL;
    LOGW("DMIC polling task exit\r\n");
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

    s_dmic.config = *config;
    s_dmic.received_words = 0;
    s_dmic.empty_polls = 0;
    s_dmic.i2s_started = false;
    s_dmic.dac_initialized = false;
    s_dmic.dac_started = false;
    s_dmic.dmic_started = false;

    if (s_dmic.config.sample_rate == 0U)
    {
        s_dmic.config.sample_rate = DIGITAL_MIC_SAMPLE_RATE_DEFAULT;
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
        s_dmic.running = false;
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
