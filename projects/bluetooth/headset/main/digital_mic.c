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
#define DIGITAL_MIC_MAX_WORDS_PER_POLL 128U
#define DIGITAL_MIC_PCM_BLOCK_WORDS 320U
#define DIGITAL_MIC_I2S_CHANNELS 2U
#define DIGITAL_MIC_I2S_BITS_PER_SAMPLE 16U
#define DIGITAL_MIC_FILTER_ENABLE 1U
#define DIGITAL_MIC_HPF_ALPHA_Q15 31130
#define DIGITAL_MIC_LPF_ALPHA_Q15 24576
#define DIGITAL_MIC_NOISE_GATE_THRESHOLD 24

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

typedef struct
{
    int32_t prev_input;
    int32_t hpf_output;
    int32_t lpf_output;
} digital_mic_filter_state_t;

static digital_mic_ctx_t s_dmic;
static digital_mic_filter_state_t s_filter_state[2];
static uint32_t s_pcm_block[DIGITAL_MIC_PCM_BLOCK_WORDS];

static int16_t digital_mic_clip_sample(int32_t sample)
{
    if (sample > 32767)
    {
        return 32767;
    }
    if (sample < -32768)
    {
        return -32768;
    }

    return (int16_t)sample;
}

static int32_t digital_mic_abs32(int32_t value)
{
    return (value < 0) ? -value : value;
}

static int16_t digital_mic_filter_sample(digital_mic_filter_state_t *state, int16_t sample)
{
#if DIGITAL_MIC_FILTER_ENABLE
    int32_t input = sample;
    int32_t hpf = input - state->prev_input +
                  ((DIGITAL_MIC_HPF_ALPHA_Q15 * state->hpf_output) >> 15);
    int32_t lpf = state->lpf_output +
                  ((DIGITAL_MIC_LPF_ALPHA_Q15 * (hpf - state->lpf_output)) >> 15);
    int32_t level = digital_mic_abs32(lpf);

    state->prev_input = input;
    state->hpf_output = hpf;
    state->lpf_output = lpf;

    if (level <= DIGITAL_MIC_NOISE_GATE_THRESHOLD)
    {
        return 0;
    }

    if (lpf > 0)
    {
        lpf -= DIGITAL_MIC_NOISE_GATE_THRESHOLD;
    }
    else
    {
        lpf += DIGITAL_MIC_NOISE_GATE_THRESHOLD;
    }

    return digital_mic_clip_sample(lpf);
#else
    (void)state;
    return sample;
#endif
}

static uint32_t digital_mic_filter_word(uint32_t word)
{
    int16_t low = (int16_t)(word & 0xFFFFU);
    int16_t high = (int16_t)((word >> 16) & 0xFFFFU);

    low = digital_mic_filter_sample(&s_filter_state[0], low);
    high = digital_mic_filter_sample(&s_filter_state[1], high);

    return (((uint32_t)(uint16_t)high) << 16) | (uint16_t)low;
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
    uint32_t status = 0;
    uint32_t dmic_data = 0;
    uint32_t poll_words = 0;
    uint32_t block_words = 0;
    uint32_t last_logged_words = 0;
    uint32_t last_sample = 0;

    (void)arg;

    LOGW("DMIC direct FIFO task start, rate=%lu threshold=%lu\r\n",
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
    s_filter_state[0] = (digital_mic_filter_state_t){0};
    s_filter_state[1] = (digital_mic_filter_state_t){0};

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

    ret = bk_aud_dmic_start();
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dmic_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dmic_started = true;

    ret = bk_aud_dac_start();
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dac_start failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dac_started = true;

    LOGW("DMIC FIFO->I2S ringbuffer running: GPIO_8 CLK, GPIO_9 DAT, sample_rate=%lu block_words=%u filter=%u gate=%d\r\n",
         (unsigned long)s_dmic.config.sample_rate,
         (unsigned int)DIGITAL_MIC_PCM_BLOCK_WORDS,
         (unsigned int)DIGITAL_MIC_FILTER_ENABLE,
         DIGITAL_MIC_NOISE_GATE_THRESHOLD);
    digital_mic_log_status("after start");

    while (s_dmic.running)
    {
        poll_words = 0;

        while (poll_words < DIGITAL_MIC_MAX_WORDS_PER_POLL)
        {
            ret = bk_aud_dmic_get_status(&status);
            if (ret != BK_OK)
            {
                LOGW("DMIC get status failed: %d\r\n", ret);
                break;
            }

            if (status & AUD_DMIC_FIFO_EMPTY_MASK)
            {
                break;
            }

            ret = bk_aud_dmic_get_fifo_data(&dmic_data);
            if (ret != BK_OK)
            {
                LOGW("DMIC get fifo data failed: %d status=0x%08lX\r\n",
                     ret,
                     (unsigned long)status);
                break;
            }

            s_pcm_block[block_words++] = digital_mic_filter_word(dmic_data);
            if (block_words >= DIGITAL_MIC_PCM_BLOCK_WORDS)
            {
                ret = audio_play_pcm_i2s_write((uint8_t *)s_pcm_block,
                                               sizeof(s_pcm_block),
                                               s_dmic.config.write_timeout_ms);
                if (ret != BK_OK)
                {
                    LOGW("DMIC I2S block write failed: %d words=%lu\r\n",
                         ret,
                         (unsigned long)s_dmic.received_words);
                    break;
                }
                block_words = 0;
            }

            s_dmic.received_words++;
            poll_words++;
            last_sample = s_pcm_block[(block_words == 0U) ? (DIGITAL_MIC_PCM_BLOCK_WORDS - 1U) : (block_words - 1U)];
        }

        if (poll_words == 0U)
        {
            s_dmic.empty_polls++;
            rtos_delay_milliseconds(1);
        }

        if ((s_dmic.received_words - last_logged_words) >= s_dmic.config.sample_rate)
        {
            last_logged_words = s_dmic.received_words;
            LOGW("DMIC FIFO->I2S words=%lu empty_polls=%lu last=0x%08lX status=0x%08lX\r\n",
                 (unsigned long)s_dmic.received_words,
                 (unsigned long)s_dmic.empty_polls,
                 (unsigned long)last_sample,
                 (unsigned long)status);
        }
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
