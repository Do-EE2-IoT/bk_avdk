#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <common/bk_err.h>
#include <driver/gpio.h>
#include <os/mem.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define TAS5711_I2C_ADDRESS_DEFAULT 0x1A
#define TAS5711_GPIO_UNUSED ((gpio_id_t)GPIO_NUM)

#define TAS571X_CLK_CTRL_REG 0x00
#define TAS571X_DEV_ID_REG 0x01
#define TAS571X_ERR_STATUS_REG 0x02
#define TAS571X_SYS_CTRL_1_REG 0x03
#define TAS571X_SDI_REG 0x04
#define TAS571X_SDI_FMT_MASK 0x0F
#define TAS571X_SYS_CTRL_2_REG 0x05
#define TAS571X_SYS_CTRL_2_SDN_MASK 0x40
#define TAS571X_SOFT_MUTE_REG 0x06
#define TAS571X_SOFT_MUTE_CH1_SHIFT 0
#define TAS571X_SOFT_MUTE_CH2_SHIFT 1
#define TAS571X_SOFT_MUTE_CH3_SHIFT 2
#define TAS571X_MVOL_REG 0x07
#define TAS571X_CH1_VOL_REG 0x08
#define TAS571X_CH2_VOL_REG 0x09
#define TAS571X_CH3_VOL_REG 0x0A
#define TAS571X_VOL_CFG_REG 0x0E
#define TAS571X_MODULATION_LIMIT_REG 0x10
#define TAS571X_IC_DELAY_CH1_REG 0x11
#define TAS571X_IC_DELAY_CH2_REG 0x12
#define TAS571X_IC_DELAY_CH3_REG 0x13
#define TAS571X_IC_DELAY_CH4_REG 0x14
#define TAS571X_PWM_CH_SDN_GROUP_REG 0x19
#define TAS571X_START_STOP_PERIOD_REG 0x1A
#define TAS571X_OSC_TRIM_REG 0x1B
#define TAS571X_BKND_ERR_REG 0x1C
#define TAS571X_INPUT_MUX_REG 0x20
#define TAS571X_CH4_SRC_SELECT_REG 0x21
#define TAS571X_PWM_MUX_REG 0x25

#define TAS5707_CH1_BQ0_REG 0x29
#define TAS5707_CH1_BQ1_REG 0x2A
#define TAS5707_CH1_BQ2_REG 0x2B
#define TAS5707_CH1_BQ3_REG 0x2C
#define TAS5707_CH1_BQ4_REG 0x2D
#define TAS5707_CH1_BQ5_REG 0x2E
#define TAS5707_CH1_BQ6_REG 0x2F
#define TAS5707_CH2_BQ0_REG 0x30
#define TAS5707_CH2_BQ1_REG 0x31
#define TAS5707_CH2_BQ2_REG 0x32
#define TAS5707_CH2_BQ3_REG 0x33
#define TAS5707_CH2_BQ4_REG 0x34
#define TAS5707_CH2_BQ5_REG 0x35
#define TAS5707_CH2_BQ6_REG 0x36

    typedef enum
    {
        TAS5711_SERIAL_FORMAT_RIGHT_JUSTIFIED = 0,
        TAS5711_SERIAL_FORMAT_I2S,
        TAS5711_SERIAL_FORMAT_LEFT_JUSTIFIED,
    } tas5711_serial_format_t;

    typedef struct
    {
        gpio_id_t sda_gpio;
        gpio_id_t scl_gpio;
        gpio_id_t reset_gpio;
        gpio_id_t pdn_gpio;
        uint8_t i2c_address;
        uint32_t delay_count;
        uint32_t post_reset_delay_ms;
        bool apply_default_registers;
        bool start_muted;
    } tas5711_config_t;

    typedef struct
    {
        uint8_t reg;
        uint32_t value;
    } tas5711_reg_default_t;

    void tas5711_init_default_config(tas5711_config_t *config);
    const tas5711_config_t *tas5711_get_config(void);
    bool tas5711_is_ready(void);

    bk_err_t tas5711_init(const tas5711_config_t *config);
    bk_err_t tas5711_deinit(void);
    bk_err_t tas5711_reset(void);
    bk_err_t tas5711_apply_default_config(void);

    bk_err_t tas5711_write_register(uint8_t reg, uint32_t value);
    bk_err_t tas5711_read_register(uint8_t reg, uint32_t *value);
    bk_err_t tas5711_write_block(uint8_t reg, const uint8_t *data, uint32_t size);
    bk_err_t tas5711_read_block(uint8_t reg, uint8_t *data, uint32_t size);
    bk_err_t tas5711_write_biquad(uint8_t reg, const uint32_t coefficients[5]);

    bk_err_t tas5711_set_shutdown(bool enable);
    bk_err_t tas5711_set_mute(bool mute);
    bk_err_t tas5711_set_master_volume(uint8_t value);
    bk_err_t tas5711_set_channel_volume(uint8_t ch1_value, uint8_t ch2_value);
    bk_err_t tas5711_configure_serial_audio(tas5711_serial_format_t format, uint8_t sample_bits);
    bk_err_t tas5711_read_basic_status(uint32_t *device_id, uint32_t *error_status);

    const tas5711_reg_default_t *tas5711_get_default_registers(uint32_t *count);

#ifdef __cplusplus
}
#endif