#include "tas_5711.h"

#include <os/os.h>

#include "gpio_driver.h"

#define TAS5711_TAG "TAS5711"
#define TAS5711_DEFAULT_DELAY_COUNT 25U
#define TAS5711_DEFAULT_RESET_DELAY_MS 50U
#define TAS5711_I2C_TIMEOUT_RETRY 2U

typedef struct
{
    tas5711_config_t config;
    bool initialized;
} tas5711_context_t;

static tas5711_context_t s_tas5711 = {0};

static const tas5711_reg_default_t s_tas5711_defaults[] = {
    {TAS571X_SDI_REG, 0x05},
    {TAS571X_SYS_CTRL_2_REG, 0x40},
    {TAS571X_SOFT_MUTE_REG, 0x00},
    {TAS571X_MVOL_REG, 0xFF},
    {TAS571X_CH1_VOL_REG, 0x30},
    {TAS571X_CH2_VOL_REG, 0x30},
    {TAS571X_OSC_TRIM_REG, 0x82},
};

static inline bool tas5711_gpio_is_used(gpio_id_t gpio)
{
    return (gpio >= GPIO_0) && (gpio < GPIO_NUM);
}

static inline void tas5711_delay(void)
{
    volatile uint32_t index;
    uint32_t loops = s_tas5711.config.delay_count;

    if (loops == 0)
    {
        loops = TAS5711_DEFAULT_DELAY_COUNT;
    }

    for (index = 0; index < loops; ++index)
    {
    }
}

static void tas5711_sda_high(void)
{
    bk_gpio_disable_input(s_tas5711.config.sda_gpio);
    bk_gpio_enable_output(s_tas5711.config.sda_gpio);
    bk_gpio_set_output_high(s_tas5711.config.sda_gpio);
}

static void tas5711_sda_low(void)
{
    bk_gpio_disable_input(s_tas5711.config.sda_gpio);
    bk_gpio_enable_output(s_tas5711.config.sda_gpio);
    bk_gpio_set_output_low(s_tas5711.config.sda_gpio);
}

static void tas5711_sda_release(void)
{
    bk_gpio_disable_output(s_tas5711.config.sda_gpio);
    bk_gpio_enable_input(s_tas5711.config.sda_gpio);
}

static bool tas5711_read_sda(void)
{
    tas5711_sda_release();
    return bk_gpio_get_input(s_tas5711.config.sda_gpio);
}

static void tas5711_scl_high(void)
{
    bk_gpio_disable_input(s_tas5711.config.scl_gpio);
    bk_gpio_enable_output(s_tas5711.config.scl_gpio);
    bk_gpio_set_output_high(s_tas5711.config.scl_gpio);
}

static void tas5711_scl_low(void)
{
    bk_gpio_disable_input(s_tas5711.config.scl_gpio);
    bk_gpio_enable_output(s_tas5711.config.scl_gpio);
    bk_gpio_set_output_low(s_tas5711.config.scl_gpio);
}

static void tas5711_i2c_start(void)
{
    tas5711_sda_high();
    tas5711_scl_high();
    tas5711_delay();
    tas5711_sda_low();
    tas5711_delay();
    tas5711_scl_low();
    tas5711_delay();
}

static void tas5711_i2c_stop(void)
{
    tas5711_scl_low();
    tas5711_sda_low();
    tas5711_delay();
    tas5711_scl_high();
    tas5711_delay();
    tas5711_sda_high();
    tas5711_delay();
    tas5711_sda_release();
}

static bk_err_t tas5711_i2c_write_byte(uint8_t value)
{
    uint8_t mask;

    for (mask = 0x80; mask != 0; mask >>= 1)
    {
        if ((value & mask) != 0U)
        {
            tas5711_sda_high();
        }
        else
        {
            tas5711_sda_low();
        }

        tas5711_delay();
        tas5711_scl_high();
        tas5711_delay();
        tas5711_scl_low();
        tas5711_delay();
    }

    tas5711_sda_release();
    tas5711_delay();
    tas5711_scl_high();
    tas5711_delay();

    if (tas5711_read_sda())
    {
        tas5711_scl_low();
        tas5711_delay();
        return BK_FAIL;
    }

    tas5711_scl_low();
    tas5711_delay();
    return BK_OK;
}

static uint8_t tas5711_i2c_read_byte(bool ack)
{
    uint8_t value = 0;
    uint8_t bit_index;

    tas5711_sda_release();

    for (bit_index = 0; bit_index < 8; ++bit_index)
    {
        value <<= 1;
        tas5711_delay();
        tas5711_scl_high();
        tas5711_delay();
        if (tas5711_read_sda())
        {
            value |= 0x01;
        }
        tas5711_scl_low();
        tas5711_delay();
    }

    if (ack)
    {
        tas5711_sda_low();
    }
    else
    {
        tas5711_sda_high();
    }

    tas5711_delay();
    tas5711_scl_high();
    tas5711_delay();
    tas5711_scl_low();
    tas5711_delay();
    tas5711_sda_release();

    return value;
}

static uint32_t tas5711_register_size(uint8_t reg)
{
    switch (reg)
    {
    case TAS571X_MVOL_REG:
    case TAS571X_CH1_VOL_REG:
    case TAS571X_CH2_VOL_REG:
    case TAS571X_SDI_REG:
    case TAS571X_SYS_CTRL_2_REG:
    case TAS571X_SOFT_MUTE_REG:
    case TAS571X_VOL_CFG_REG:
    case TAS571X_MODULATION_LIMIT_REG:
    case TAS571X_IC_DELAY_CH1_REG:
    case TAS571X_IC_DELAY_CH2_REG:
    case TAS571X_IC_DELAY_CH3_REG:
    case TAS571X_IC_DELAY_CH4_REG:
    case TAS571X_PWM_CH_SDN_GROUP_REG:
    case TAS571X_START_STOP_PERIOD_REG:
    case TAS571X_OSC_TRIM_REG:
    case TAS571X_BKND_ERR_REG:
        return 1;
    case TAS571X_INPUT_MUX_REG:
    case TAS571X_CH4_SRC_SELECT_REG:
    case TAS571X_PWM_MUX_REG:
        return 4;
    case TAS5707_CH1_BQ0_REG:
    case TAS5707_CH1_BQ1_REG:
    case TAS5707_CH1_BQ2_REG:
    case TAS5707_CH1_BQ3_REG:
    case TAS5707_CH1_BQ4_REG:
    case TAS5707_CH1_BQ5_REG:
    case TAS5707_CH1_BQ6_REG:
    case TAS5707_CH2_BQ0_REG:
    case TAS5707_CH2_BQ1_REG:
    case TAS5707_CH2_BQ2_REG:
    case TAS5707_CH2_BQ3_REG:
    case TAS5707_CH2_BQ4_REG:
    case TAS5707_CH2_BQ5_REG:
    case TAS5707_CH2_BQ6_REG:
        return 20;
    default:
        return 1;
    }
}

static bk_err_t tas5711_validate_state(void)
{
    if (!s_tas5711.initialized)
    {
        return BK_ERR_NOT_INIT;
    }

    if (!tas5711_gpio_is_used(s_tas5711.config.sda_gpio) || !tas5711_gpio_is_used(s_tas5711.config.scl_gpio))
    {
        return BK_ERR_PARAM;
    }

    return BK_OK;
}

static void tas5711_configure_output_gpio(gpio_id_t gpio)
{
    gpio_dev_unmap(gpio);
    bk_gpio_disable_input(gpio);
    bk_gpio_enable_output(gpio);
}

static bk_err_t tas5711_write_bytes(uint8_t reg, const uint8_t *data, uint32_t size)
{
    bk_err_t ret;
    uint32_t index;
    uint32_t attempt;

    if ((data == NULL) || (size == 0U))
    {
        return BK_ERR_NULL_PARAM;
    }

    ret = tas5711_validate_state();
    if (ret != BK_OK)
    {
        return ret;
    }

    for (attempt = 0; attempt < TAS5711_I2C_TIMEOUT_RETRY; ++attempt)
    {
        tas5711_i2c_start();

        ret = tas5711_i2c_write_byte((uint8_t)((s_tas5711.config.i2c_address << 1) | 0U));
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        ret = tas5711_i2c_write_byte(reg);
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        for (index = 0; index < size; ++index)
        {
            ret = tas5711_i2c_write_byte(data[index]);
            if (ret != BK_OK)
            {
                goto stop_and_retry;
            }
        }

        tas5711_i2c_stop();
        return BK_OK;

    stop_and_retry:
        tas5711_i2c_stop();
        rtos_delay_milliseconds(1);
    }

    BK_LOGE(TAS5711_TAG, "I2C write failed, reg=0x%02X size=%lu\r\n", reg, (unsigned long)size);
    return BK_FAIL;
}

static bk_err_t tas5711_read_bytes(uint8_t reg, uint8_t *data, uint32_t size)
{
    bk_err_t ret;
    uint32_t index;
    uint32_t attempt;

    if ((data == NULL) || (size == 0U))
    {
        return BK_ERR_NULL_PARAM;
    }

    ret = tas5711_validate_state();
    if (ret != BK_OK)
    {
        return ret;
    }

    for (attempt = 0; attempt < TAS5711_I2C_TIMEOUT_RETRY; ++attempt)
    {
        tas5711_i2c_start();

        ret = tas5711_i2c_write_byte((uint8_t)((s_tas5711.config.i2c_address << 1) | 0U));
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        ret = tas5711_i2c_write_byte(reg);
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        tas5711_i2c_start();
        ret = tas5711_i2c_write_byte((uint8_t)((s_tas5711.config.i2c_address << 1) | 1U));
        if (ret != BK_OK)
        {
            goto stop_and_retry;
        }

        for (index = 0; index < size; ++index)
        {
            data[index] = tas5711_i2c_read_byte(index + 1U < size);
        }

        tas5711_i2c_stop();
        return BK_OK;

    stop_and_retry:
        tas5711_i2c_stop();
        rtos_delay_milliseconds(1);
    }

    BK_LOGE(TAS5711_TAG, "I2C read failed, reg=0x%02X size=%lu\r\n", reg, (unsigned long)size);
    return BK_FAIL;
}

void tas5711_init_default_config(tas5711_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config->sda_gpio = GPIO_1;
    config->scl_gpio = GPIO_0;
    config->reset_gpio = TAS5711_GPIO_UNUSED;
    config->pdn_gpio = TAS5711_GPIO_UNUSED;
    config->i2c_address = TAS5711_I2C_ADDRESS_DEFAULT;
    config->delay_count = TAS5711_DEFAULT_DELAY_COUNT;
    config->post_reset_delay_ms = TAS5711_DEFAULT_RESET_DELAY_MS;
    config->apply_default_registers = true;
    config->start_muted = true;
}

const tas5711_config_t *tas5711_get_config(void)
{
    if (!s_tas5711.initialized)
    {
        return NULL;
    }

    return &s_tas5711.config;
}

bool tas5711_is_ready(void)
{
    return s_tas5711.initialized;
}

bk_err_t tas5711_reset(void)
{
    uint32_t delay_ms;

    if (!s_tas5711.initialized)
    {
        return BK_ERR_NOT_INIT;
    }

    delay_ms = s_tas5711.config.post_reset_delay_ms;
    if (delay_ms == 0U)
    {
        delay_ms = TAS5711_DEFAULT_RESET_DELAY_MS;
    }

    if (tas5711_gpio_is_used(s_tas5711.config.pdn_gpio))
    {
        bk_gpio_set_output_low(s_tas5711.config.pdn_gpio);
    }

    if (tas5711_gpio_is_used(s_tas5711.config.reset_gpio))
    {
        bk_gpio_set_output_low(s_tas5711.config.reset_gpio);
        rtos_delay_milliseconds(1);
        bk_gpio_set_output_high(s_tas5711.config.reset_gpio);
    }

    rtos_delay_milliseconds(delay_ms);
    return BK_OK;
}

bk_err_t tas5711_init(const tas5711_config_t *config)
{
    bk_err_t ret;

    if (config == NULL)
    {
        return BK_ERR_NULL_PARAM;
    }

    if (!tas5711_gpio_is_used(config->sda_gpio) || !tas5711_gpio_is_used(config->scl_gpio))
    {
        return BK_ERR_PARAM;
    }

    s_tas5711.config = *config;
    s_tas5711.initialized = true;

    gpio_dev_unmap(s_tas5711.config.sda_gpio);
    gpio_dev_unmap(s_tas5711.config.scl_gpio);
    bk_gpio_pull_up(s_tas5711.config.sda_gpio);
    bk_gpio_pull_up(s_tas5711.config.scl_gpio);
    tas5711_sda_high();
    tas5711_scl_high();
    tas5711_sda_release();

    if (tas5711_gpio_is_used(s_tas5711.config.pdn_gpio))
    {
        tas5711_configure_output_gpio(s_tas5711.config.pdn_gpio);
        bk_gpio_set_output_low(s_tas5711.config.pdn_gpio);
    }

    if (tas5711_gpio_is_used(s_tas5711.config.reset_gpio))
    {
        tas5711_configure_output_gpio(s_tas5711.config.reset_gpio);
        bk_gpio_set_output_high(s_tas5711.config.reset_gpio);
    }

    ret = tas5711_reset();
    if (ret != BK_OK)
    {
        return ret;
    }

    ret = tas5711_write_register(TAS571X_OSC_TRIM_REG, 0x00);
    if (ret != BK_OK)
    {
        return ret;
    }

    rtos_delay_milliseconds(50);

    if (s_tas5711.config.apply_default_registers)
    {
        ret = tas5711_apply_default_config();
        if (ret != BK_OK)
        {
            return ret;
        }
    }

    ret = tas5711_set_mute(s_tas5711.config.start_muted);
    if (ret != BK_OK)
    {
        return ret;
    }

    return tas5711_set_shutdown(false);
}

bk_err_t tas5711_deinit(void)
{
    if (!s_tas5711.initialized)
    {
        return BK_OK;
    }

    tas5711_set_mute(true);
    tas5711_set_shutdown(true);
    s_tas5711.initialized = false;
    os_memset(&s_tas5711.config, 0, sizeof(s_tas5711.config));
    return BK_OK;
}

bk_err_t tas5711_apply_default_config(void)
{
    uint32_t index;

    for (index = 0; index < (sizeof(s_tas5711_defaults) / sizeof(s_tas5711_defaults[0])); ++index)
    {
        bk_err_t ret = tas5711_write_register(s_tas5711_defaults[index].reg, s_tas5711_defaults[index].value);
        if (ret != BK_OK)
        {
            return ret;
        }
    }

    return BK_OK;
}

bk_err_t tas5711_write_register(uint8_t reg, uint32_t value)
{
    uint32_t size = tas5711_register_size(reg);
    uint8_t buffer[4];
    uint32_t index;

    if (size > sizeof(buffer))
    {
        return BK_ERR_PARAM;
    }

    for (index = 0; index < size; ++index)
    {
        buffer[size - 1U - index] = (uint8_t)(value & 0xFFU);
        value >>= 8;
    }

    return tas5711_write_bytes(reg, buffer, size);
}

bk_err_t tas5711_read_register(uint8_t reg, uint32_t *value)
{
    uint32_t size = tas5711_register_size(reg);
    uint8_t buffer[4];
    uint32_t index;
    bk_err_t ret;

    if (value == NULL)
    {
        return BK_ERR_NULL_PARAM;
    }

    if (size > sizeof(buffer))
    {
        return BK_ERR_PARAM;
    }

    ret = tas5711_read_bytes(reg, buffer, size);
    if (ret != BK_OK)
    {
        return ret;
    }

    *value = 0;
    for (index = 0; index < size; ++index)
    {
        *value <<= 8;
        *value |= buffer[index];
    }

    return BK_OK;
}

bk_err_t tas5711_write_block(uint8_t reg, const uint8_t *data, uint32_t size)
{
    return tas5711_write_bytes(reg, data, size);
}

bk_err_t tas5711_read_block(uint8_t reg, uint8_t *data, uint32_t size)
{
    return tas5711_read_bytes(reg, data, size);
}

bk_err_t tas5711_write_biquad(uint8_t reg, const uint32_t coefficients[5])
{
    uint8_t buffer[20];
    uint32_t index;

    if (coefficients == NULL)
    {
        return BK_ERR_NULL_PARAM;
    }

    for (index = 0; index < 5U; ++index)
    {
        buffer[index * 4U + 0U] = (uint8_t)((coefficients[index] >> 24) & 0xFFU);
        buffer[index * 4U + 1U] = (uint8_t)((coefficients[index] >> 16) & 0xFFU);
        buffer[index * 4U + 2U] = (uint8_t)((coefficients[index] >> 8) & 0xFFU);
        buffer[index * 4U + 3U] = (uint8_t)(coefficients[index] & 0xFFU);
    }

    return tas5711_write_bytes(reg, buffer, sizeof(buffer));
}

bk_err_t tas5711_set_shutdown(bool enable)
{
    uint32_t value = enable ? TAS571X_SYS_CTRL_2_SDN_MASK : 0U;
    return tas5711_write_register(TAS571X_SYS_CTRL_2_REG, value);
}

bk_err_t tas5711_set_mute(bool mute)
{
    uint32_t value = mute ? ((1U << TAS571X_SOFT_MUTE_CH1_SHIFT) | (1U << TAS571X_SOFT_MUTE_CH2_SHIFT)) : 0U;
    return tas5711_write_register(TAS571X_SOFT_MUTE_REG, value);
}

bk_err_t tas5711_set_master_volume(uint8_t value)
{
    return tas5711_write_register(TAS571X_MVOL_REG, value);
}

bk_err_t tas5711_set_channel_volume(uint8_t ch1_value, uint8_t ch2_value)
{
    bk_err_t ret = tas5711_write_register(TAS571X_CH1_VOL_REG, ch1_value);
    if (ret != BK_OK)
    {
        return ret;
    }

    return tas5711_write_register(TAS571X_CH2_VOL_REG, ch2_value);
}

bk_err_t tas5711_configure_serial_audio(tas5711_serial_format_t format, uint8_t sample_bits)
{
    uint32_t value;

    switch (format)
    {
    case TAS5711_SERIAL_FORMAT_RIGHT_JUSTIFIED:
        value = 0x00;
        break;
    case TAS5711_SERIAL_FORMAT_I2S:
        value = 0x03;
        break;
    case TAS5711_SERIAL_FORMAT_LEFT_JUSTIFIED:
        value = 0x06;
        break;
    default:
        return BK_ERR_PARAM;
    }

    if (sample_bits >= 24U)
    {
        value += 2U;
    }
    else if (sample_bits >= 20U)
    {
        value += 1U;
    }

    return tas5711_write_register(TAS571X_SDI_REG, value & TAS571X_SDI_FMT_MASK);
}

bk_err_t tas5711_read_basic_status(uint32_t *device_id, uint32_t *error_status)
{
    bk_err_t ret;

    if ((device_id == NULL) || (error_status == NULL))
    {
        return BK_ERR_NULL_PARAM;
    }

    ret = tas5711_read_register(TAS571X_DEV_ID_REG, device_id);
    if (ret != BK_OK)
    {
        return ret;
    }

    return tas5711_read_register(TAS571X_ERR_STATUS_REG, error_status);
}

const tas5711_reg_default_t *tas5711_get_default_registers(uint32_t *count)
{
    if (count != NULL)
    {
        *count = (uint32_t)(sizeof(s_tas5711_defaults) / sizeof(s_tas5711_defaults[0]));
    }

    return s_tas5711_defaults;
}