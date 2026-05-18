#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <common/bk_err.h>
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define TAS5805M_I2C_ADDRESS_DEFAULT 0x2C
#define TAS5805M_GPIO_UNUSED ((gpio_id_t)GPIO_NUM)

#define TAS5805M_REG_PAGE 0x00
#define TAS5805M_REG_RESET_CTRL 0x01
#define TAS5805M_REG_DEVICE_CTRL_1 0x02
#define TAS5805M_REG_DEVICE_CTRL_2 0x03
#define TAS5805M_REG_SIG_CH_CTRL 0x28
#define TAS5805M_REG_SAP_CTRL_1 0x33
#define TAS5805M_REG_FS_MON 0x37
#define TAS5805M_REG_BCK_MON 0x38
#define TAS5805M_REG_CLKDET_STATUS 0x39
#define TAS5805M_REG_DIG_VOL_LEFT 0x4C
#define TAS5805M_REG_DIG_VOL_RIGHT 0x4D
#define TAS5805M_REG_AGAIN 0x54
#define TAS5805M_REG_ADR_PIN_CTRL 0x60
#define TAS5805M_REG_ADR_PIN_CONFIG 0x61
#define TAS5805M_REG_CHAN_FAULT 0x70
#define TAS5805M_REG_GLOBAL_FAULT1 0x71
#define TAS5805M_REG_GLOBAL_FAULT2 0x72
#define TAS5805M_REG_FAULT 0x78
#define TAS5805M_REG_BOOK 0x7F

#define TAS5805M_DEVICE_CTRL_2_DEEP_SLEEP 0x00
#define TAS5805M_DEVICE_CTRL_2_SLEEP 0x01
#define TAS5805M_DEVICE_CTRL_2_HIZ 0x02
#define TAS5805M_DEVICE_CTRL_2_PLAY 0x03
#define TAS5805M_DEVICE_CTRL_2_MUTE 0x08
#define TAS5805M_DEVICE_CTRL_2_DIS_DSP 0x10

typedef struct
{
    gpio_id_t sda_gpio;
    gpio_id_t scl_gpio;
    gpio_id_t pdn_gpio;
    gpio_id_t adr_gpio;
    uint8_t i2c_address;
    uint32_t delay_count;
    uint32_t pdn_low_delay_ms;
    uint32_t pdn_high_delay_ms;
    uint8_t analog_gain;
    bool start_muted;
    bool scan_bus;
    bool adr_high;
} tas5805m_config_t;

void tas5805m_init_default_config(tas5805m_config_t *config);
const tas5805m_config_t *tas5805m_get_config(void);
bool tas5805m_is_ready(void);
bool tas5805m_is_started(void);

bk_err_t tas5805m_init(const tas5805m_config_t *config);
bk_err_t tas5805m_deinit(void);
bk_err_t tas5805m_start(uint32_t sample_rate);
bk_err_t tas5805m_stop(void);
bk_err_t tas5805m_set_mute(bool mute);
bk_err_t tas5805m_set_volume(uint8_t left, uint8_t right);

bk_err_t tas5805m_write_register(uint8_t reg, uint8_t value);
bk_err_t tas5805m_read_register(uint8_t reg, uint8_t *value);
bk_err_t tas5805m_write_block(uint8_t reg, const uint8_t *data, uint32_t size);
bk_err_t tas5805m_send_cfg(const uint8_t *cfg, uint32_t size);
bk_err_t tas5805m_dump_status(void);

#ifdef __cplusplus
}
#endif
