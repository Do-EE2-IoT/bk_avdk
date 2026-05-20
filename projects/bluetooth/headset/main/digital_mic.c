#include "digital_mic.h"

#include <stdbool.h>
#include <stdint.h>

#include <driver/aud_dac.h>
#include <driver/aud_dac_types.h>
#include <driver/aud_dmic.h>
#include <driver/aud_dmic_types.h>
#include <driver/dma.h>
#include <os/mem.h>
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
#define DIGITAL_MIC_FIFO_BURST_WORDS 16U
#define DIGITAL_MIC_PCM_BLOCK_WORDS 320U
#define DIGITAL_MIC_DMA_WORDS (DIGITAL_MIC_PCM_BLOCK_WORDS * 2U)
#define DIGITAL_MIC_DMA_START_TIMEOUT_POLLS 1000U
#define DIGITAL_MIC_DIRECT_DMA_TO_I2S 0U
#define DIGITAL_MIC_DIRECT_CPU_TO_I2S 1U
#define DIGITAL_MIC_USE_DMA 0U
#define DIGITAL_MIC_I2S_CHANNELS 2U
#define DIGITAL_MIC_I2S_BITS_PER_SAMPLE 16U
#define DIGITAL_MIC_PLAYBACK_RATE_MULTIPLIER 1U
#define DIGITAL_MIC_CAPTURE_RATE_MULTIPLIER 1U
#define DIGITAL_MIC_FILTER_ENABLE 0U
#define DIGITAL_MIC_STATS_ENABLE 1U
#define DIGITAL_MIC_STATS_DECIMATE_MASK 0x0FU
#define DIGITAL_MIC_FAST_MONO_ENABLE 1U
#define DIGITAL_MIC_FIFO_EMPTY_YIELD_INTERVAL 64U
#define DIGITAL_MIC_HPF_ALPHA_Q15 31130
#define DIGITAL_MIC_FAST_HPF_ALPHA_Q15 30500
#define DIGITAL_MIC_LPF_ALPHA_Q15 32767
#define DIGITAL_MIC_NOISE_GATE_THRESHOLD 0
#define DIGITAL_MIC_OUTPUT_GAIN_Q8 (8 * 256)
#define DIGITAL_MIC_OUTPUT_CHANNEL 1U
#define DIGITAL_MIC_OUTPUT_DC_OFFSET 0

typedef struct
{
    digital_mic_config_t config;
    beken_thread_t thread;
    dma_id_t dma_id;
    volatile uint32_t received_words;
    volatile uint32_t empty_polls;
    volatile uint32_t dropped_blocks;
    volatile uint32_t dma_half_count;
    volatile uint32_t dma_finish_count;
    volatile bool running;
    bool i2s_started;
    bool direct_i2s_started;
    bool dac_initialized;
    bool dma_initialized;
    bool dma_started;
    bool dac_started;
    bool dmic_started;
} digital_mic_ctx_t;

typedef struct
{
    int32_t prev_input;
    int32_t hpf_output;
    int32_t lpf_output;
} digital_mic_filter_state_t;

typedef struct
{
    uint32_t count;
    int32_t min[2];
    int32_t max[2];
    int64_t sum[2];
    uint64_t sum_sq[2];
    uint32_t zero[2];
    uint32_t clip[2];
} digital_mic_audio_stats_t;

static digital_mic_ctx_t s_dmic;
static digital_mic_filter_state_t s_filter_state[2];
static digital_mic_filter_state_t s_fast_filter_state;
static digital_mic_audio_stats_t s_raw_stats;
static digital_mic_audio_stats_t s_out_stats;
static uint32_t s_raw_stats_decimate;
static uint32_t s_out_stats_decimate;
static uint32_t s_pcm_block[DIGITAL_MIC_PCM_BLOCK_WORDS];
static uint32_t s_dmic_dma_buffer[DIGITAL_MIC_DMA_WORDS];

static void digital_mic_dma_half_isr(dma_id_t dma_id)
{
    (void)dma_id;
    s_dmic.dma_half_count++;
}

static void digital_mic_dma_finish_isr(dma_id_t dma_id)
{
    (void)dma_id;
    s_dmic.dma_finish_count++;
}

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

static uint32_t digital_mic_isqrt64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t result = 0;

    while (bit > value)
    {
        bit >>= 2;
    }

    while (bit != 0)
    {
        if (value >= result + bit)
        {
            value -= result + bit;
            result = (result >> 1) + bit;
        }
        else
        {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

static void digital_mic_stats_reset(digital_mic_audio_stats_t *stats)
{
#if DIGITAL_MIC_STATS_ENABLE
    stats->count = 0;
    stats->min[0] = 32767;
    stats->min[1] = 32767;
    stats->max[0] = -32768;
    stats->max[1] = -32768;
    stats->sum[0] = 0;
    stats->sum[1] = 0;
    stats->sum_sq[0] = 0;
    stats->sum_sq[1] = 0;
    stats->zero[0] = 0;
    stats->zero[1] = 0;
    stats->clip[0] = 0;
    stats->clip[1] = 0;
    if (stats == &s_raw_stats)
    {
        s_raw_stats_decimate = 0;
    }
    else if (stats == &s_out_stats)
    {
        s_out_stats_decimate = 0;
    }
#else
    (void)stats;
#endif
}

static void digital_mic_stats_add_sample(digital_mic_audio_stats_t *stats, uint32_t channel, int16_t sample)
{
#if DIGITAL_MIC_STATS_ENABLE
    int32_t value = sample;

    if (value < stats->min[channel])
    {
        stats->min[channel] = value;
    }
    if (value > stats->max[channel])
    {
        stats->max[channel] = value;
    }

    stats->sum[channel] += value;
    stats->sum_sq[channel] += (uint64_t)((int64_t)value * value);

    if (value == 0)
    {
        stats->zero[channel]++;
    }
    if ((value >= 32760) || (value <= -32760))
    {
        stats->clip[channel]++;
    }
#else
    (void)stats;
    (void)channel;
    (void)sample;
#endif
}

static void digital_mic_stats_add_word(digital_mic_audio_stats_t *stats, uint32_t word)
{
#if DIGITAL_MIC_STATS_ENABLE
    uint32_t *decimate = (stats == &s_raw_stats) ? &s_raw_stats_decimate : &s_out_stats_decimate;
    int16_t low = (int16_t)(word & 0xFFFFU);
    int16_t high = (int16_t)((word >> 16) & 0xFFFFU);

    if (((*decimate)++ & DIGITAL_MIC_STATS_DECIMATE_MASK) != 0U)
    {
        return;
    }

    digital_mic_stats_add_sample(stats, 0, low);
    digital_mic_stats_add_sample(stats, 1, high);
    stats->count++;
#else
    (void)stats;
    (void)word;
#endif
}

static void digital_mic_stats_log_and_reset(const char *name, digital_mic_audio_stats_t *stats)
{
#if DIGITAL_MIC_STATS_ENABLE
    uint32_t rms_l = 0;
    uint32_t rms_r = 0;
    int32_t dc_l = 0;
    int32_t dc_r = 0;

    if (stats->count == 0U)
    {
        LOGW("DMIC %s stats: empty\r\n", name);
        return;
    }

    rms_l = digital_mic_isqrt64(stats->sum_sq[0] / stats->count);
    rms_r = digital_mic_isqrt64(stats->sum_sq[1] / stats->count);
    dc_l = (int32_t)(stats->sum[0] / (int64_t)stats->count);
    dc_r = (int32_t)(stats->sum[1] / (int64_t)stats->count);

    LOGW("DMIC %s stats: count=%lu L[min,max,rms,dc]=[%ld,%ld,%lu,%ld] R[min,max,rms,dc]=[%ld,%ld,%lu,%ld] zero[L,R]=[%lu,%lu] clip[L,R]=[%lu,%lu]\r\n",
         name,
         (unsigned long)stats->count,
         (long)stats->min[0],
         (long)stats->max[0],
         (unsigned long)rms_l,
         (long)dc_l,
         (long)stats->min[1],
         (long)stats->max[1],
         (unsigned long)rms_r,
         (long)dc_r,
         (unsigned long)stats->zero[0],
         (unsigned long)stats->zero[1],
         (unsigned long)stats->clip[0],
         (unsigned long)stats->clip[1]);

    digital_mic_stats_reset(stats);
#else
    (void)name;
    (void)stats;
#endif
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

    return digital_mic_clip_sample((lpf * DIGITAL_MIC_OUTPUT_GAIN_Q8) >> 8);
#else
    (void)state;
    return sample;
#endif
}

static uint32_t digital_mic_filter_word(uint32_t word)
{
#if DIGITAL_MIC_FAST_MONO_ENABLE
    int16_t low = (int16_t)(word & 0xFFFFU);
    int16_t high = (int16_t)((word >> 16) & 0xFFFFU);
    int32_t input = (DIGITAL_MIC_OUTPUT_CHANNEL == 0U) ? low : high;
    int32_t selected;

    input -= DIGITAL_MIC_OUTPUT_DC_OFFSET;
    selected = ((DIGITAL_MIC_FAST_HPF_ALPHA_Q15 *
                 (s_fast_filter_state.hpf_output + input - s_fast_filter_state.prev_input)) >>
                15);
    s_fast_filter_state.prev_input = input;
    s_fast_filter_state.hpf_output = selected;
    selected = (selected * DIGITAL_MIC_OUTPUT_GAIN_Q8) >> 8;

    selected = digital_mic_clip_sample(selected);
    return (((uint32_t)(uint16_t)selected) << 16) | (uint16_t)selected;
#elif DIGITAL_MIC_FILTER_ENABLE
    int16_t low = (int16_t)(word & 0xFFFFU);
    int16_t high = (int16_t)((word >> 16) & 0xFFFFU);
    int16_t selected;

    low = digital_mic_filter_sample(&s_filter_state[0], low);
    high = digital_mic_filter_sample(&s_filter_state[1], high);

    selected = (DIGITAL_MIC_OUTPUT_CHANNEL == 0U) ? low : high;

    return (((uint32_t)(uint16_t)selected) << 16) | (uint16_t)selected;
#else
    return word;
#endif
}

static uint32_t digital_mic_get_capture_rate(uint32_t playback_rate)
{
    uint32_t capture_rate = playback_rate * DIGITAL_MIC_CAPTURE_RATE_MULTIPLIER;

    switch (capture_rate)
    {
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return capture_rate;
    default:
        return playback_rate;
    }
}

static uint32_t digital_mic_get_i2s_rate(uint32_t playback_rate)
{
    uint32_t i2s_rate = playback_rate * DIGITAL_MIC_PLAYBACK_RATE_MULTIPLIER;

    switch (i2s_rate)
    {
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return i2s_rate;
    default:
        return playback_rate;
    }
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

static bk_err_t digital_mic_write_block(const uint32_t *words, uint32_t word_count)
{
    uint32_t index;

    if ((words == NULL) || (word_count > DIGITAL_MIC_PCM_BLOCK_WORDS))
    {
        return BK_ERR_PARAM;
    }

    for (index = 0; index < word_count; index++)
    {
        digital_mic_stats_add_word(&s_raw_stats, words[index]);
        s_pcm_block[index] = digital_mic_filter_word(words[index]);
        digital_mic_stats_add_word(&s_out_stats, s_pcm_block[index]);
    }

    return audio_play_pcm_i2s_write((uint8_t *)s_pcm_block,
                                    word_count * sizeof(uint32_t),
                                    s_dmic.config.write_timeout_ms);
}

static uint32_t digital_mic_poll_fifo_block(uint32_t *block_words, uint32_t *last_sample)
{
    bk_err_t ret;
    uint32_t status = 0;
    uint32_t dmic_data = 0;
    uint32_t read_count = 0;

    while (*block_words < DIGITAL_MIC_PCM_BLOCK_WORDS)
    {
        ret = bk_aud_dmic_get_status(&status);
        if (ret != BK_OK)
        {
            LOGW("DMIC poll get status failed: %d\r\n", ret);
            break;
        }

        if (status & AUD_DMIC_FIFO_EMPTY_MASK)
        {
            break;
        }

        for (uint32_t burst = 0; (burst < DIGITAL_MIC_FIFO_BURST_WORDS) && (*block_words < DIGITAL_MIC_PCM_BLOCK_WORDS); burst++)
        {
            ret = bk_aud_dmic_get_fifo_data(&dmic_data);
            if (ret != BK_OK)
            {
                LOGW("DMIC poll get fifo failed: %d status=0x%08lX\r\n",
                     ret,
                     (unsigned long)status);
                break;
            }

            s_dmic_dma_buffer[(*block_words)++] = dmic_data;
            read_count++;
        }
    }

    if (*block_words == DIGITAL_MIC_PCM_BLOCK_WORDS)
    {
        ret = digital_mic_write_block(s_dmic_dma_buffer, *block_words);
        if (ret != BK_OK)
        {
            LOGW("DMIC poll I2S write failed: %d words=%lu\r\n",
                 ret,
                 (unsigned long)s_dmic.received_words);
            *block_words = 0;
            return 0;
        }

        s_dmic.received_words += *block_words;
        *last_sample = s_pcm_block[*block_words - 1U];
        *block_words = 0;
    }

    return read_count;
}

static uint32_t digital_mic_poll_fifo_direct_i2s(uint32_t *last_sample)
{
    bk_err_t ret;
    uint32_t status = 0;
    uint32_t dmic_data = 0;
    uint32_t read_count = 0;

    ret = bk_aud_dmic_get_status(&status);
    if (ret != BK_OK)
    {
        LOGW("DMIC direct get status failed: %d\r\n", ret);
        return 0;
    }

    if (status & AUD_DMIC_FIFO_EMPTY_MASK)
    {
        return 0;
    }

    for (uint32_t burst = 0; burst < DIGITAL_MIC_FIFO_BURST_WORDS; burst++)
    {
        ret = bk_aud_dmic_get_fifo_data(&dmic_data);
        if (ret != BK_OK)
        {
            LOGW("DMIC direct get fifo failed: %d status=0x%08lX\r\n",
                 ret,
                 (unsigned long)status);
            break;
        }

        ret = audio_play_i2s_direct_write_word(dmic_data);
        if (ret != BK_OK)
        {
            LOGW("DMIC direct I2S write failed: %d words=%lu\r\n",
                 ret,
                 (unsigned long)s_dmic.received_words);
            break;
        }

        digital_mic_stats_add_word(&s_raw_stats, dmic_data);
        digital_mic_stats_add_word(&s_out_stats, dmic_data);
        s_dmic.received_words++;
        *last_sample = dmic_data;
        read_count++;
    }

    return read_count;
}

static void digital_mic_cleanup(void)
{
    s_dmic.running = false;

    if (s_dmic.dmic_started)
    {
        bk_aud_dmic_stop();
        s_dmic.dmic_started = false;
    }

    if (s_dmic.dma_started)
    {
        bk_dma_stop(s_dmic.dma_id);
        s_dmic.dma_started = false;
    }

    if (s_dmic.dma_initialized)
    {
        bk_dma_deinit(s_dmic.dma_id);
        bk_dma_free(DMA_DEV_AUDIO, s_dmic.dma_id);
        s_dmic.dma_initialized = false;
        s_dmic.dma_id = DMA_ID_MAX;
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

    if (s_dmic.direct_i2s_started)
    {
        audio_play_i2s_direct_stop();
        s_dmic.direct_i2s_started = false;
    }
}

static void digital_mic_task(void *arg)
{
    bk_err_t ret;
    aud_dac_config_t dac_config = DEFAULT_AUD_DAC_CONFIG();
    aud_dmic_config_t dmic_config = DEFAULT_AUD_DMIC_CONFIG();
    dma_config_t dma_config;
    uint32_t status = 0;
    uint32_t dmic_fifo_addr = 0;
    uint32_t i2s_data_addr = 0;
    uint32_t last_logged_words = 0;
    uint32_t last_sample = 0;
    uint32_t handled_half = 0;
    uint32_t handled_finish = 0;
    uint32_t poll_block_words = 0;
    uint32_t direct_log_ticks = 0;
    uint32_t playback_rate;
    uint32_t capture_rate;
    uint32_t i2s_rate;
    bool use_dma = (DIGITAL_MIC_USE_DMA != 0U);
    bool direct_dma_to_i2s = (DIGITAL_MIC_DIRECT_DMA_TO_I2S != 0U);
    bool direct_cpu_to_i2s = (DIGITAL_MIC_DIRECT_CPU_TO_I2S != 0U);

    (void)arg;
    playback_rate = s_dmic.config.sample_rate;
    capture_rate = digital_mic_get_capture_rate(playback_rate);
    i2s_rate = digital_mic_get_i2s_rate(playback_rate);

    LOGW("DMIC DMA task start, playback_rate=%lu i2s_rate=%lu capture_rate=%lu threshold=%lu\r\n",
         (unsigned long)playback_rate,
         (unsigned long)i2s_rate,
         (unsigned long)capture_rate,
         (unsigned long)DIGITAL_MIC_FIFO_THRESHOLD);

    if (direct_dma_to_i2s || direct_cpu_to_i2s)
    {
        ret = audio_play_i2s_direct_start(i2s_rate);
        if (ret != BK_OK)
        {
            LOGE("%s audio_play_i2s_direct_start failed: %d\r\n", __func__, ret);
            goto exit;
        }
        s_dmic.direct_i2s_started = true;

        if (direct_dma_to_i2s)
        {
            ret = audio_play_i2s_get_tx_addr(&i2s_data_addr);
            if (ret != BK_OK)
            {
                LOGE("%s audio_play_i2s_get_tx_addr failed: %d\r\n", __func__, ret);
                goto exit;
            }
        }
    }
    else
    {
        ret = audio_play_pcm_i2s_start(i2s_rate,
                                       DIGITAL_MIC_I2S_CHANNELS,
                                       DIGITAL_MIC_I2S_BITS_PER_SAMPLE);
        if (ret != BK_OK)
        {
            LOGE("%s audio_play_pcm_i2s_start failed: %d\r\n", __func__, ret);
            goto exit;
        }
        s_dmic.i2s_started = true;

    }

    dac_config.samp_rate = capture_rate;
    dac_config.dac_chl = AUD_DAC_CHL_LR;

    ret = bk_aud_dac_init(&dac_config);
    if (ret != BK_OK)
    {
        LOGE("%s bk_aud_dac_init failed: %d\r\n", __func__, ret);
        goto exit;
    }
    s_dmic.dac_initialized = true;

    dmic_config.samp_rate = capture_rate;
    dmic_config.dmic_chl = AUD_DMIC_CHL_LR;
    s_filter_state[0] = (digital_mic_filter_state_t){0};
    s_filter_state[1] = (digital_mic_filter_state_t){0};
    s_fast_filter_state = (digital_mic_filter_state_t){0};
    digital_mic_stats_reset(&s_raw_stats);
    digital_mic_stats_reset(&s_out_stats);

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

    if (use_dma || direct_dma_to_i2s)
    {
        ret = bk_dma_driver_init();
        if (ret != BK_OK)
        {
            LOGE("%s bk_dma_driver_init failed: %d\r\n", __func__, ret);
            goto exit;
        }

        ret = bk_aud_dmic_get_fifo_addr(&dmic_fifo_addr);
        if (ret != BK_OK)
        {
            LOGE("%s bk_aud_dmic_get_fifo_addr failed: %d\r\n", __func__, ret);
            goto exit;
        }

        os_memset(&dma_config, 0, sizeof(dma_config));
        os_memset(s_dmic_dma_buffer, 0, sizeof(s_dmic_dma_buffer));

        dma_config.mode = DMA_WORK_MODE_REPEAT;
        dma_config.chan_prio = 1;
        dma_config.src.dev = DMA_DEV_AUDIO;
        dma_config.src.width = DMA_DATA_WIDTH_32BITS;
        dma_config.src.addr_inc_en = DMA_ADDR_INC_ENABLE;
        dma_config.src.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
        dma_config.src.start_addr = dmic_fifo_addr;
        dma_config.src.end_addr = dmic_fifo_addr + sizeof(uint32_t);
        dma_config.dst.width = DMA_DATA_WIDTH_32BITS;
        dma_config.dst.addr_inc_en = DMA_ADDR_INC_ENABLE;
        dma_config.dst.addr_loop_en = DMA_ADDR_LOOP_ENABLE;
        if (direct_dma_to_i2s)
        {
            dma_config.dst.dev = DMA_DEV_I2S;
            dma_config.dst.start_addr = i2s_data_addr;
            dma_config.dst.end_addr = i2s_data_addr + sizeof(uint32_t);
        }
        else
        {
            dma_config.dst.dev = DMA_DEV_DTCM;
            dma_config.dst.start_addr = (uint32_t)s_dmic_dma_buffer;
            dma_config.dst.end_addr = (uint32_t)s_dmic_dma_buffer + sizeof(s_dmic_dma_buffer);
        }

        s_dmic.dma_id = bk_dma_alloc(DMA_DEV_AUDIO);
        if ((s_dmic.dma_id < DMA_ID_0) || (s_dmic.dma_id >= DMA_ID_MAX))
        {
            LOGE("%s bk_dma_alloc failed\r\n", __func__);
            s_dmic.dma_id = DMA_ID_MAX;
            goto exit;
        }

        ret = bk_dma_init(s_dmic.dma_id, &dma_config);
        if (ret != BK_OK)
        {
            LOGE("%s bk_dma_init failed: %d\r\n", __func__, ret);
            goto exit;
        }
        s_dmic.dma_initialized = true;

        if (direct_dma_to_i2s)
        {
            bk_dma_set_transfer_len(s_dmic.dma_id, sizeof(uint32_t));
        }
        else
        {
            bk_dma_set_transfer_len(s_dmic.dma_id, sizeof(s_dmic_dma_buffer));
            bk_dma_register_isr(s_dmic.dma_id, digital_mic_dma_half_isr, digital_mic_dma_finish_isr);
            bk_dma_enable_half_finish_interrupt(s_dmic.dma_id);
            bk_dma_enable_finish_interrupt(s_dmic.dma_id);
        }
#if (CONFIG_SPE)
        bk_dma_set_dest_sec_attr(s_dmic.dma_id, DMA_ATTR_SEC);
        bk_dma_set_src_sec_attr(s_dmic.dma_id, DMA_ATTR_SEC);
#endif

        ret = bk_dma_start(s_dmic.dma_id);
        if (ret != BK_OK)
        {
            LOGE("%s bk_dma_start failed: %d\r\n", __func__, ret);
            goto exit;
        }
        s_dmic.dma_started = true;
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

    LOGW("DMIC %s->I2S running: GPIO_8 CLK, GPIO_9 DAT, playback_rate=%lu i2s_rate=%lu capture_rate=%lu block_words=%u filter=%u gate=%d gain_q8=%u out_ch=%u dmic_fifo=0x%08lX i2s_data=0x%08lX\r\n",
         direct_dma_to_i2s ? "direct DMA" : (direct_cpu_to_i2s ? "direct CPU" : (use_dma ? "DMA ringbuffer" : "FIFO ringbuffer")),
         (unsigned long)playback_rate,
         (unsigned long)i2s_rate,
         (unsigned long)capture_rate,
         (unsigned int)DIGITAL_MIC_PCM_BLOCK_WORDS,
         (unsigned int)DIGITAL_MIC_FILTER_ENABLE,
         DIGITAL_MIC_NOISE_GATE_THRESHOLD,
         (unsigned int)DIGITAL_MIC_OUTPUT_GAIN_Q8,
         (unsigned int)DIGITAL_MIC_OUTPUT_CHANNEL,
         (unsigned long)dmic_fifo_addr,
         (unsigned long)i2s_data_addr);
    digital_mic_log_status("after start");

    while (s_dmic.running)
    {
        bool processed = false;
        uint32_t half_count = s_dmic.dma_half_count;
        uint32_t finish_count = s_dmic.dma_finish_count;

        if (direct_dma_to_i2s)
        {
            processed = true;
            rtos_delay_milliseconds(100);
            direct_log_ticks++;
            if (direct_log_ticks >= 10U)
            {
                direct_log_ticks = 0;
                s_dmic.received_words += playback_rate;
            }
        }

        if (!processed && direct_cpu_to_i2s)
        {
            processed = (digital_mic_poll_fifo_direct_i2s(&last_sample) != 0U);
        }

        if (use_dma && (handled_half != half_count))
        {
            if ((half_count - handled_half) > 1U)
            {
                s_dmic.dropped_blocks += (half_count - handled_half - 1U);
            }
            handled_half = half_count;

            ret = digital_mic_write_block(&s_dmic_dma_buffer[0], DIGITAL_MIC_PCM_BLOCK_WORDS);
            if (ret != BK_OK)
            {
                LOGW("DMIC I2S half write failed: %d words=%lu\r\n",
                     ret,
                     (unsigned long)s_dmic.received_words);
            }
            else
            {
                s_dmic.received_words += DIGITAL_MIC_PCM_BLOCK_WORDS;
                last_sample = s_pcm_block[DIGITAL_MIC_PCM_BLOCK_WORDS - 1U];
            }
            processed = true;
        }

        if (use_dma && (handled_finish != finish_count))
        {
            if ((finish_count - handled_finish) > 1U)
            {
                s_dmic.dropped_blocks += (finish_count - handled_finish - 1U);
            }
            handled_finish = finish_count;

            ret = digital_mic_write_block(&s_dmic_dma_buffer[DIGITAL_MIC_PCM_BLOCK_WORDS], DIGITAL_MIC_PCM_BLOCK_WORDS);
            if (ret != BK_OK)
            {
                LOGW("DMIC I2S finish write failed: %d words=%lu\r\n",
                     ret,
                     (unsigned long)s_dmic.received_words);
            }
            else
            {
                s_dmic.received_words += DIGITAL_MIC_PCM_BLOCK_WORDS;
                last_sample = s_pcm_block[DIGITAL_MIC_PCM_BLOCK_WORDS - 1U];
            }
            processed = true;
        }

        if (!processed && !use_dma && !direct_cpu_to_i2s)
        {
            processed = (digital_mic_poll_fifo_block(&poll_block_words, &last_sample) != 0U);
        }

        if (!processed)
        {
            s_dmic.empty_polls++;
            if (direct_cpu_to_i2s)
            {
                if ((s_dmic.empty_polls % 1000U) == 0U)
                {
                    bk_aud_dmic_get_status(&status);
                    LOGW("DMIC direct CPU empty: fifo_status=0x%08lX received_words=%lu empty_polls=%lu\r\n",
                         (unsigned long)status,
                         (unsigned long)s_dmic.received_words,
                         (unsigned long)s_dmic.empty_polls);
                }
                if ((s_dmic.empty_polls % DIGITAL_MIC_FIFO_EMPTY_YIELD_INTERVAL) == 0U)
                {
                    rtos_delay_milliseconds(1);
                }
            }
            else if (!use_dma)
            {
                if ((s_dmic.empty_polls % DIGITAL_MIC_FIFO_EMPTY_YIELD_INTERVAL) == 0U)
                {
                    rtos_delay_milliseconds(1);
                }
            }
            else if ((s_dmic.received_words == 0U) &&
                     (s_dmic.empty_polls >= DIGITAL_MIC_DMA_START_TIMEOUT_POLLS))
            {
                bk_aud_dmic_get_status(&status);
                LOGW("DMIC DMA inactive, fallback to FIFO polling: empty_polls=%lu half=%lu finish=%lu status=0x%08lX\r\n",
                     (unsigned long)s_dmic.empty_polls,
                     (unsigned long)s_dmic.dma_half_count,
                     (unsigned long)s_dmic.dma_finish_count,
                     (unsigned long)status);

                if (s_dmic.dma_started)
                {
                    bk_dma_stop(s_dmic.dma_id);
                    s_dmic.dma_started = false;
                }
                if (s_dmic.dma_initialized)
                {
                    bk_dma_deinit(s_dmic.dma_id);
                    bk_dma_free(DMA_DEV_AUDIO, s_dmic.dma_id);
                    s_dmic.dma_initialized = false;
                    s_dmic.dma_id = DMA_ID_MAX;
                }

                use_dma = false;
                rtos_delay_milliseconds(1);
            }
            else
            {
                rtos_delay_milliseconds(1);
            }
        }

        if ((s_dmic.received_words - last_logged_words) >= playback_rate)
        {
            last_logged_words = s_dmic.received_words;
            bk_aud_dmic_get_status(&status);
            LOGW("DMIC %s->I2S words=%lu dropped=%lu empty_polls=%lu half=%lu finish=%lu last=0x%08lX status=0x%08lX\r\n",
                 direct_dma_to_i2s ? "direct DMA" : (direct_cpu_to_i2s ? "direct CPU" : (use_dma ? "DMA" : "FIFO")),
                 (unsigned long)s_dmic.received_words,
                 (unsigned long)s_dmic.dropped_blocks,
                 (unsigned long)s_dmic.empty_polls,
                 (unsigned long)s_dmic.dma_half_count,
                 (unsigned long)s_dmic.dma_finish_count,
                 (unsigned long)last_sample,
                 (unsigned long)status);
            digital_mic_stats_log_and_reset("raw", &s_raw_stats);
            digital_mic_stats_log_and_reset("out", &s_out_stats);
        }
    }

exit:
    digital_mic_cleanup();
    s_dmic.thread = NULL;
    LOGW("DMIC DMA task exit\r\n");
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
    s_dmic.dma_id = DMA_ID_MAX;
    s_dmic.received_words = 0;
    s_dmic.empty_polls = 0;
    s_dmic.dropped_blocks = 0;
    s_dmic.dma_half_count = 0;
    s_dmic.dma_finish_count = 0;
    s_dmic.i2s_started = false;
    s_dmic.direct_i2s_started = false;
    s_dmic.dac_initialized = false;
    s_dmic.dma_initialized = false;
    s_dmic.dma_started = false;
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
