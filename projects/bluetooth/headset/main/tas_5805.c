#include "tas_5805.h"

#include <os/mem.h>
#include <os/os.h>

#include "gpio_driver.h"

#define TAS5805M_TAG "TAS5805M"
#define TAS5805M_DEFAULT_DELAY_COUNT 25U
#define TAS5805M_DEFAULT_PDN_LOW_DELAY_MS 120U
#define TAS5805M_DEFAULT_PDN_HIGH_DELAY_MS 15U
#define TAS5805M_I2C_RETRY_COUNT 3U
#define TAS5805M_CLOCK_STABLE_DELAY_MS 6U
#define TAS5805M_CFG_META_DELAY 0xFE
#define TAS5805M_CFG_META_BURST 0xFD
#define TAS5805M_CFG_ASCII_TEXT 0xF0

typedef struct
{
    tas5805m_config_t config;
    bool initialized;
    bool started;
    bool muted;
    uint32_t sample_rate;
} tas5805m_context_t;

static tas5805m_context_t s_tas5805m = {0};

static const uint8_t s_tas5805m_minimal_cfg[] = {
    TAS5805M_REG_PAGE, 0x00,
    TAS5805M_REG_BOOK, 0x00,
    TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_DEVICE_CTRL_2_HIZ,
    TAS5805M_REG_RESET_CTRL, 0x10,
    TAS5805M_CFG_META_DELAY, 0x05,
    TAS5805M_REG_AGAIN, 0x03,
    TAS5805M_REG_FAULT, 0x80,
};

typedef struct
{
    uint8_t reg;
    const char *name;
} tas5805m_dump_reg_t;

static const tas5805m_dump_reg_t s_tas5805m_dump_regs[] = {
    {TAS5805M_REG_DEVICE_CTRL_2, "DEVICE_CTRL_2"},
    {TAS5805M_REG_FS_MON, "FS_MON"},
    {TAS5805M_REG_BCK_MON, "BCK_MON"},
    {TAS5805M_REG_CLKDET_STATUS, "CLKDET_STATUS"},
    {TAS5805M_REG_AGAIN, "AGAIN"},
    {TAS5805M_REG_CHAN_FAULT, "CHAN_FAULT"},
    {TAS5805M_REG_GLOBAL_FAULT1, "GLOBAL_FAULT1"},
    {TAS5805M_REG_GLOBAL_FAULT2, "GLOBAL_FAULT2"},
    {TAS5805M_REG_FAULT, "FAULT"},
};

static inline bool tas5805m_gpio_is_used(gpio_id_t gpio)
{
    return (gpio >= GPIO_0) && (gpio < GPIO_NUM);
}

static inline void tas5805m_delay(void)
{
    volatile uint32_t index;
    uint32_t loops = s_tas5805m.config.delay_count;

    if (loops == 0U)
    {
        loops = TAS5805M_DEFAULT_DELAY_COUNT;
    }

    for (index = 0; index < loops; ++index)
    {
    }
}

static void tas5805m_sda_high(void)
{
    bk_gpio_disable_input(s_tas5805m.config.sda_gpio);
    bk_gpio_enable_output(s_tas5805m.config.sda_gpio);
    bk_gpio_set_output_high(s_tas5805m.config.sda_gpio);
}

static void tas5805m_sda_low(void)
{
    bk_gpio_disable_input(s_tas5805m.config.sda_gpio);
    bk_gpio_enable_output(s_tas5805m.config.sda_gpio);
    bk_gpio_set_output_low(s_tas5805m.config.sda_gpio);
}

static void tas5805m_sda_release(void)
{
    bk_gpio_disable_output(s_tas5805m.config.sda_gpio);
    bk_gpio_enable_input(s_tas5805m.config.sda_gpio);
}

static bool tas5805m_read_sda(void)
{
    tas5805m_sda_release();
    return bk_gpio_get_input(s_tas5805m.config.sda_gpio);
}

static void tas5805m_scl_high(void)
{
    bk_gpio_disable_input(s_tas5805m.config.scl_gpio);
    bk_gpio_enable_output(s_tas5805m.config.scl_gpio);
    bk_gpio_set_output_high(s_tas5805m.config.scl_gpio);
}

static void tas5805m_scl_low(void)
{
    bk_gpio_disable_input(s_tas5805m.config.scl_gpio);
    bk_gpio_enable_output(s_tas5805m.config.scl_gpio);
    bk_gpio_set_output_low(s_tas5805m.config.scl_gpio);
}

static void tas5805m_i2c_start(void)
{
    tas5805m_sda_high();
    tas5805m_scl_high();
    tas5805m_delay();
    tas5805m_sda_low();
    tas5805m_delay();
    tas5805m_scl_low();
    tas5805m_delay();
}

static void tas5805m_i2c_stop(void)
{
    tas5805m_scl_low();
    tas5805m_sda_low();
    tas5805m_delay();
    tas5805m_scl_high();
    tas5805m_delay();
    tas5805m_sda_high();
    tas5805m_delay();
    tas5805m_sda_release();
}

static bk_err_t tas5805m_i2c_write_byte(uint8_t value)
{
    uint8_t mask;

    for (mask = 0x80; mask != 0U; mask >>= 1)
    {
        if ((value & mask) != 0U)
        {
            tas5805m_sda_high();
        }
        else
        {
            tas5805m_sda_low();
        }

        tas5805m_delay();
        tas5805m_scl_high();
        tas5805m_delay();
        tas5805m_scl_low();
        tas5805m_delay();
    }

    tas5805m_sda_release();
    tas5805m_delay();
    tas5805m_scl_high();
    tas5805m_delay();

    if (tas5805m_read_sda())
    {
        tas5805m_scl_low();
        tas5805m_delay();
        return BK_FAIL;
    }

    tas5805m_scl_low();
    tas5805m_delay();
    return BK_OK;
}

static uint8_t tas5805m_i2c_read_byte(bool ack)
{
    uint8_t value = 0;
    uint8_t bit_index;

    tas5805m_sda_release();

    for (bit_index = 0; bit_index < 8U; ++bit_index)
    {
        value <<= 1;
        tas5805m_delay();
        tas5805m_scl_high();
        tas5805m_delay();
        if (tas5805m_read_sda())
        {
            value |= 0x01;
        }
        tas5805m_scl_low();
        tas5805m_delay();
    }

    if (ack)
    {
        tas5805m_sda_low();
    }
    else
    {
        tas5805m_sda_high();
    }

    tas5805m_delay();
    tas5805m_scl_high();
    tas5805m_delay();
    tas5805m_scl_low();
    tas5805m_delay();
    tas5805m_sda_release();

    return value;
}

static bk_err_t tas5805m_validate_state(void)
{
    if (!s_tas5805m.initialized)
    {
        return BK_ERR_NOT_INIT;
    }

    if (!tas5805m_gpio_is_used(s_tas5805m.config.sda_gpio) ||
        !tas5805m_gpio_is_used(s_tas5805m.config.scl_gpio))
    {
        return BK_ERR_PARAM;
    }

    return BK_OK;
}

static void tas5805m_configure_output_gpio(gpio_id_t gpio)
{
    gpio_dev_unmap(gpio);
    bk_gpio_disable_input(gpio);
    bk_gpio_enable_output(gpio);
}

static void tas5805m_configure_i2c_gpio(void)
{
    gpio_dev_unmap(s_tas5805m.config.sda_gpio);
    gpio_dev_unmap(s_tas5805m.config.scl_gpio);
    bk_gpio_pull_up(s_tas5805m.config.sda_gpio);
    bk_gpio_pull_up(s_tas5805m.config.scl_gpio);
}

static void tas5805m_i2c_bus_idle(void)
{
    tas5805m_sda_high();
    tas5805m_scl_high();
    tas5805m_delay();
    tas5805m_sda_release();
}

static bool tas5805m_i2c_probe_address(uint8_t address)
{
    bk_err_t ret;

    tas5805m_i2c_start();
    ret = tas5805m_i2c_write_byte((uint8_t)((address << 1) | 0U));
    tas5805m_i2c_stop();

    return (ret == BK_OK);
}

static bk_err_t tas5805m_scan_i2c_bus(void)
{
    uint32_t found_count = 0;
    bool expected_found = false;
    uint32_t address;

    BK_LOGW(TAS5805M_TAG, "I2C scan start, expect TAS5805M at 0x%02X\r\n",
            s_tas5805m.config.i2c_address);

    for (address = 0x03; address <= 0x77U; ++address)
    {
        if (tas5805m_i2c_probe_address((uint8_t)address))
        {
            BK_LOGW(TAS5805M_TAG, "I2C device found at 0x%02X\r\n", (unsigned int)address);
            ++found_count;

            if (address == s_tas5805m.config.i2c_address)
            {
                expected_found = true;
            }
        }
    }

    BK_LOGW(TAS5805M_TAG, "I2C scan done, found %lu device(s)\r\n", (unsigned long)found_count);

    if (!expected_found)
    {
        BK_LOGE(TAS5805M_TAG, "Expected TAS5805M I2C address 0x%02X not found\r\n",
                s_tas5805m.config.i2c_address);
        return BK_FAIL;
    }

    return BK_OK;
}

static void tas5805m_init_failed_cleanup(void)
{
    if (tas5805m_gpio_is_used(s_tas5805m.config.pdn_gpio))
    {
        bk_gpio_set_output_low(s_tas5805m.config.pdn_gpio);
    }

    s_tas5805m.initialized = false;
    s_tas5805m.started = false;
}

static bk_err_t tas5805m_write_bytes(uint8_t reg, const uint8_t *data, uint32_t size)
{
    bk_err_t ret;
    uint32_t index;
    uint32_t attempt;

    if ((data == NULL) || (size == 0U))
    {
        return BK_ERR_NULL_PARAM;
    }

    ret = tas5805m_validate_state();
    if (ret != BK_OK)
    {
        return ret;
    }

    for (attempt = 0; attempt < TAS5805M_I2C_RETRY_COUNT; ++attempt)
    {
        tas5805m_i2c_start();

        ret = tas5805m_i2c_write_byte((uint8_t)((s_tas5805m.config.i2c_address << 1) | 0U));
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        ret = tas5805m_i2c_write_byte(reg);
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        for (index = 0; index < size; ++index)
        {
            ret = tas5805m_i2c_write_byte(data[index]);
            if (ret != BK_OK)
            {
                goto stop_and_retry;
            }
        }

        tas5805m_i2c_stop();
        return BK_OK;

    stop_and_retry:
        tas5805m_i2c_stop();
        rtos_delay_milliseconds(1);
    }

    BK_LOGE(TAS5805M_TAG, "I2C write failed, reg=0x%02X size=%lu\r\n", reg, (unsigned long)size);
    return BK_FAIL;
}

static bk_err_t tas5805m_read_bytes(uint8_t reg, uint8_t *data, uint32_t size)
{
    bk_err_t ret;
    uint32_t index;
    uint32_t attempt;

    if ((data == NULL) || (size == 0U))
    {
        return BK_ERR_NULL_PARAM;
    }

    ret = tas5805m_validate_state();
    if (ret != BK_OK)
    {
        return ret;
    }

    for (attempt = 0; attempt < TAS5805M_I2C_RETRY_COUNT; ++attempt)
    {
        tas5805m_i2c_start();

        ret = tas5805m_i2c_write_byte((uint8_t)((s_tas5805m.config.i2c_address << 1) | 0U));
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        ret = tas5805m_i2c_write_byte(reg);
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        tas5805m_i2c_start();
        ret = tas5805m_i2c_write_byte((uint8_t)((s_tas5805m.config.i2c_address << 1) | 1U));
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        for (index = 0; index < size; ++index)
        {
            data[index] = tas5805m_i2c_read_byte(index + 1U < size);
        }

        tas5805m_i2c_stop();
        return BK_OK;

    stop_and_retry:
        tas5805m_i2c_stop();
        rtos_delay_milliseconds(1);
    }

    BK_LOGE(TAS5805M_TAG, "I2C read failed, reg=0x%02X size=%lu\r\n", reg, (unsigned long)size);
    return BK_FAIL;
}

void tas5805m_init_default_config(tas5805m_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config->sda_gpio = GPIO_1;
    config->scl_gpio = GPIO_0;
    config->pdn_gpio = TAS5805M_GPIO_UNUSED;
    config->adr_gpio = TAS5805M_GPIO_UNUSED;
    config->i2c_address = TAS5805M_I2C_ADDRESS_DEFAULT;
    config->delay_count = TAS5805M_DEFAULT_DELAY_COUNT;
    config->pdn_low_delay_ms = TAS5805M_DEFAULT_PDN_LOW_DELAY_MS;
    config->pdn_high_delay_ms = TAS5805M_DEFAULT_PDN_HIGH_DELAY_MS;
    config->analog_gain = 0x03;
    config->start_muted = false;
    config->scan_bus = true;
    config->adr_high = false;
}

const tas5805m_config_t *tas5805m_get_config(void)
{
    if (!s_tas5805m.initialized)
    {
        return NULL;
    }

    return &s_tas5805m.config;
}

bool tas5805m_is_ready(void)
{
    return s_tas5805m.initialized;
}

bool tas5805m_is_started(void)
{
    return s_tas5805m.started;
}

bk_err_t tas5805m_init(const tas5805m_config_t *config)
{
    bk_err_t ret;
    uint32_t low_delay_ms;
    uint32_t high_delay_ms;

    if (config == NULL)
    {
        return BK_ERR_NULL_PARAM;
    }

    if (!tas5805m_gpio_is_used(config->sda_gpio) || !tas5805m_gpio_is_used(config->scl_gpio))
    {
        return BK_ERR_PARAM;
    }

    if (s_tas5805m.initialized)
    {
        return BK_OK;
    }

    s_tas5805m.config = *config;
    s_tas5805m.initialized = true;
    s_tas5805m.started = false;
    s_tas5805m.muted = config->start_muted;
    s_tas5805m.sample_rate = 0;

    if (tas5805m_gpio_is_used(s_tas5805m.config.adr_gpio))
    {
        tas5805m_configure_output_gpio(s_tas5805m.config.adr_gpio);
        if (s_tas5805m.config.adr_high)
        {
            bk_gpio_set_output_high(s_tas5805m.config.adr_gpio);
        }
        else
        {
            bk_gpio_set_output_low(s_tas5805m.config.adr_gpio);
        }
    }

    if (tas5805m_gpio_is_used(s_tas5805m.config.pdn_gpio))
    {
        low_delay_ms = s_tas5805m.config.pdn_low_delay_ms;
        high_delay_ms = s_tas5805m.config.pdn_high_delay_ms;

        if (low_delay_ms == 0U)
        {
            low_delay_ms = TAS5805M_DEFAULT_PDN_LOW_DELAY_MS;
        }
        if (high_delay_ms == 0U)
        {
            high_delay_ms = TAS5805M_DEFAULT_PDN_HIGH_DELAY_MS;
        }

        tas5805m_configure_output_gpio(s_tas5805m.config.pdn_gpio);
        bk_gpio_set_output_low(s_tas5805m.config.pdn_gpio);
        rtos_delay_milliseconds(low_delay_ms);
        bk_gpio_set_output_high(s_tas5805m.config.pdn_gpio);
        rtos_delay_milliseconds(high_delay_ms);
    }

    tas5805m_configure_i2c_gpio();
    tas5805m_i2c_bus_idle();

    if (s_tas5805m.config.scan_bus)
    {
        ret = tas5805m_scan_i2c_bus();
        if (ret != BK_OK)
        {
            goto init_failed;
        }
    }

    ret = tas5805m_write_register(TAS5805M_REG_PAGE, 0x00);
    if (ret != BK_OK)
    {
        goto init_failed;
    }

    ret = tas5805m_write_register(TAS5805M_REG_BOOK, 0x00);
    if (ret != BK_OK)
    {
        goto init_failed;
    }

    ret = tas5805m_write_register(TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_DEVICE_CTRL_2_HIZ | TAS5805M_DEVICE_CTRL_2_MUTE);
    if (ret != BK_OK)
    {
        goto init_failed;
    }

    BK_LOGI(TAS5805M_TAG, "TAS5805M powered, addr=0x%02X\r\n", s_tas5805m.config.i2c_address);
    return BK_OK;

init_failed:
    tas5805m_init_failed_cleanup();
    return ret;
}

bk_err_t tas5805m_deinit(void)
{
    if (!s_tas5805m.initialized)
    {
        return BK_OK;
    }

    tas5805m_stop();

    if (tas5805m_gpio_is_used(s_tas5805m.config.pdn_gpio))
    {
        bk_gpio_set_output_low(s_tas5805m.config.pdn_gpio);
    }

    os_memset(&s_tas5805m, 0, sizeof(s_tas5805m));
    return BK_OK;
}

bk_err_t tas5805m_start(uint32_t sample_rate)
{
    bk_err_t ret;
    uint8_t mode = TAS5805M_DEVICE_CTRL_2_PLAY;

    ret = tas5805m_validate_state();
    if (ret != BK_OK)
    {
        return ret;
    }

    rtos_delay_milliseconds(TAS5805M_CLOCK_STABLE_DELAY_MS);

    ret = tas5805m_send_cfg(s_tas5805m_minimal_cfg, sizeof(s_tas5805m_minimal_cfg));
    if (ret != BK_OK)
    {
        return ret;
    }

    if (s_tas5805m.config.analog_gain != 0U)
    {
        ret = tas5805m_write_register(TAS5805M_REG_AGAIN, s_tas5805m.config.analog_gain);
        if (ret != BK_OK)
        {
            return ret;
        }
    }

    ret = tas5805m_write_register(TAS5805M_REG_FAULT, 0x80);
    if (ret != BK_OK)
    {
        return ret;
    }

    if (s_tas5805m.muted)
    {
        mode |= TAS5805M_DEVICE_CTRL_2_MUTE;
    }

    ret = tas5805m_write_register(TAS5805M_REG_DEVICE_CTRL_2, mode);
    if (ret != BK_OK)
    {
        return ret;
    }

    s_tas5805m.started = true;
    s_tas5805m.sample_rate = sample_rate;
    BK_LOGI(TAS5805M_TAG, "TAS5805M start, sample_rate=%lu mute=%u\r\n",
            (unsigned long)sample_rate, (unsigned int)s_tas5805m.muted);
    return BK_OK;
}

bk_err_t tas5805m_stop(void)
{
    bk_err_t ret;

    if (!s_tas5805m.initialized)
    {
        return BK_OK;
    }

    ret = tas5805m_write_register(TAS5805M_REG_PAGE, 0x00);
    if (ret != BK_OK)
    {
        return ret;
    }

    ret = tas5805m_write_register(TAS5805M_REG_BOOK, 0x00);
    if (ret != BK_OK)
    {
        return ret;
    }

    ret = tas5805m_write_register(TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_DEVICE_CTRL_2_HIZ | TAS5805M_DEVICE_CTRL_2_MUTE);
    if (ret != BK_OK)
    {
        return ret;
    }

    s_tas5805m.started = false;
    s_tas5805m.sample_rate = 0;
    return BK_OK;
}

bk_err_t tas5805m_set_mute(bool mute)
{
    bk_err_t ret;
    uint8_t value = TAS5805M_DEVICE_CTRL_2_PLAY;

    ret = tas5805m_validate_state();
    if (ret != BK_OK)
    {
        return ret;
    }

    s_tas5805m.muted = mute;
    if (mute)
    {
        value |= TAS5805M_DEVICE_CTRL_2_MUTE;
    }

    if (!s_tas5805m.started)
    {
        return BK_OK;
    }

    return tas5805m_write_register(TAS5805M_REG_DEVICE_CTRL_2, value);
}

bk_err_t tas5805m_set_volume(uint8_t left, uint8_t right)
{
    bk_err_t ret;

    ret = tas5805m_write_register(TAS5805M_REG_DIG_VOL_LEFT, left);
    if (ret != BK_OK)
    {
        return ret;
    }

    return tas5805m_write_register(TAS5805M_REG_DIG_VOL_RIGHT, right);
}

bk_err_t tas5805m_write_register(uint8_t reg, uint8_t value)
{
    return tas5805m_write_bytes(reg, &value, 1);
}

bk_err_t tas5805m_read_register(uint8_t reg, uint8_t *value)
{
    return tas5805m_read_bytes(reg, value, 1);
}

bk_err_t tas5805m_write_block(uint8_t reg, const uint8_t *data, uint32_t size)
{
    return tas5805m_write_bytes(reg, data, size);
}

bk_err_t tas5805m_send_cfg(const uint8_t *cfg, uint32_t size)
{
    uint32_t index = 0;
    bk_err_t ret;

    if ((cfg == NULL) || (size == 0U))
    {
        return BK_ERR_NULL_PARAM;
    }

    while (index < size)
    {
        uint8_t cmd = cfg[index];

        if (cmd == TAS5805M_CFG_META_DELAY)
        {
            if (index + 1U >= size)
            {
                return BK_ERR_PARAM;
            }
            rtos_delay_milliseconds(cfg[index + 1U]);
            index += 2U;
        }
        else if (cmd == TAS5805M_CFG_META_BURST)
        {
            uint8_t burst_len;
            uint8_t reg;

            if (index + 2U >= size)
            {
                return BK_ERR_PARAM;
            }

            burst_len = cfg[index + 1U];
            if ((burst_len == 0U) || (index + 2U + burst_len > size))
            {
                return BK_ERR_PARAM;
            }

            reg = cfg[index + 2U];
            ret = tas5805m_write_block(reg, &cfg[index + 3U], burst_len - 1U);
            if (ret != BK_OK)
            {
                return ret;
            }
            index += 2U + burst_len;
        }
        else if (cmd == TAS5805M_CFG_ASCII_TEXT)
        {
            if (index + 1U >= size)
            {
                return BK_ERR_PARAM;
            }
            index += (uint32_t)cfg[index + 1U] + 1U;
        }
        else
        {
            if (index + 1U >= size)
            {
                return BK_ERR_PARAM;
            }
            ret = tas5805m_write_register(cmd, cfg[index + 1U]);
            if (ret != BK_OK)
            {
                return ret;
            }
            index += 2U;
        }
    }

    return BK_OK;
}

bk_err_t tas5805m_dump_status(void)
{
    uint32_t index;

    for (index = 0; index < (sizeof(s_tas5805m_dump_regs) / sizeof(s_tas5805m_dump_regs[0])); ++index)
    {
        uint8_t value = 0;
        bk_err_t ret = tas5805m_read_register(s_tas5805m_dump_regs[index].reg, &value);

        if (ret != BK_OK)
        {
            BK_LOGW(TAS5805M_TAG, "dump %s(0x%02X) read failed: %d\r\n",
                    s_tas5805m_dump_regs[index].name,
                    s_tas5805m_dump_regs[index].reg,
                    ret);
            return ret;
        }

        BK_LOGW(TAS5805M_TAG, "dump %s(0x%02X)=0x%02X\r\n",
                s_tas5805m_dump_regs[index].name,
                s_tas5805m_dump_regs[index].reg,
                value);
    }

    return BK_OK;
}
