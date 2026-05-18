#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include "components/bluetooth/bk_dm_bluetooth.h"
#include <components/ate.h>

#include "cli.h"

#include "media_service.h"
#include "bt_manager.h"
#include "gatt/dm_gatt.h"
#include "gatt/dm_gatts.h"
#include "hogpd/hogpd_demo.h"
#include "wifi_boarding/wifi_boarding_demo.h"
#include "tas_5711.h"

#define AUTO_ENABLE_BLUETOOTH_DEMO 1
#define TAS5711_APP_SDA GPIO_1
#define TAS5711_APP_SCL GPIO_0
#define TAS5711_APP_RESET GPIO_13
#define TAS5711_APP_PDN GPIO_2
#define TAS5711_APP_I2C_DELAY_INIT 25U
#define TAS5711_APP_TEST_MVOL 0x30
#define TAS5711_APP_TEST_CHVOL 0x30
#define TAS5711_APP_SERIAL_FORMAT TAS5711_SERIAL_FORMAT_I2S
#define TAS5711_APP_SERIAL_BITS 16U
#define TAS5711_APP_TAG "TAS5711_APP"

static bk_err_t tas5711_expect_register(uint8_t reg, uint32_t expected, const char *name)
{
    uint32_t value = 0;
    bk_err_t ret = tas5711_read_register(reg, &value);

    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "Read %s(0x%02X) failed: %d\n", name, reg, ret);
        return ret;
    }

    if (value != expected)
    {
        BK_LOGE(TAS5711_APP_TAG,
                "%s(0x%02X) mismatch, expected=0x%02lX actual=0x%02lX\n",
                name,
                reg,
                (unsigned long)expected,
                (unsigned long)value);
        return BK_FAIL;
    }

    return BK_OK;
}

extern void rtos_set_user_app_entry(beken_thread_function_t entry);

#ifdef CONFIG_CACHE_CUSTOM_SRAM_MAPPING
const unsigned int g_sram_addr_map[4] =
    {
        0x38000000,
        0x30020000,
        0x38020000,
        0x30000000};
#endif

static void user_app_main(void)
{
}

static void tas5711_demo_init(void)
{
    static tas5711_config_t tas_cfg;
    bk_err_t ret;
    uint32_t device_id = 0;
    uint32_t error_status = 0;

    tas5711_init_default_config(&tas_cfg);
    tas_cfg.sda_gpio = TAS5711_APP_SDA;
    tas_cfg.scl_gpio = TAS5711_APP_SCL;
    tas_cfg.reset_gpio = TAS5711_APP_RESET;
    tas_cfg.pdn_gpio = TAS5711_APP_PDN;
    tas_cfg.delay_count = TAS5711_APP_I2C_DELAY_INIT;
    tas_cfg.start_muted = false;

    ret = tas5711_init(&tas_cfg);
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "tas5711_init failed: %d\n", ret);
        return;
    }

    BK_LOGW(TAS5711_APP_TAG, "Configure TAS5711 serial format=%u bits=%u\n",
            (unsigned int)TAS5711_APP_SERIAL_FORMAT,
            (unsigned int)TAS5711_APP_SERIAL_BITS);
    ret = tas5711_configure_serial_audio(TAS5711_APP_SERIAL_FORMAT, TAS5711_APP_SERIAL_BITS);
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "tas5711_configure_serial_audio failed: %d\n", ret);
        return;
    }

    // ret = tas_5711_configure_clock_control(0b01101100);
    // if (ret != BK_OK)
    // {
    //     BK_LOGE(TAS5711_APP_TAG, "tas_5711_configure_clock_control failed: %d\n", ret);
    //     return;
    // }

    BK_LOGW(TAS5711_APP_TAG, "Set TAS5711 test volume MVOL=0x%02X CHVOL=0x%02X\n",
            TAS5711_APP_TEST_MVOL, TAS5711_APP_TEST_CHVOL);
    ret = tas5711_set_master_volume(TAS5711_APP_TEST_MVOL);
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "tas5711_set_master_volume failed: %d\n", ret);
        return;
    }

    ret = tas5711_set_channel_volume(TAS5711_APP_TEST_CHVOL, TAS5711_APP_TEST_CHVOL);
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "tas5711_set_channel_volume failed: %d\n", ret);
        return;
    }

    ret = tas5711_expect_register(TAS571X_SDI_REG, 0x03, "SDI");
    if (ret != BK_OK)
    {
        return;
    }

    ret = tas5711_expect_register(TAS571X_SYS_CTRL_2_REG, TAS571X_SYS_CTRL_2_SDN_MASK, "SYS_CTRL_2");
    if (ret != BK_OK)
    {
        return;
    }

    ret = tas5711_expect_register(TAS571X_SOFT_MUTE_REG, 0x03, "SOFT_MUTE");
    if (ret != BK_OK)
    {
        return;
    }

    ret = tas5711_expect_register(TAS571X_MVOL_REG, TAS5711_APP_TEST_MVOL, "MVOL");
    if (ret != BK_OK)
    {
        return;
    }

    ret = tas5711_expect_register(TAS571X_CH1_VOL_REG, TAS5711_APP_TEST_CHVOL, "CH1_VOL");
    if (ret != BK_OK)
    {
        return;
    }

    ret = tas5711_expect_register(TAS571X_CH2_VOL_REG, TAS5711_APP_TEST_CHVOL, "CH2_VOL");
    if (ret != BK_OK)
    {
        return;
    }

    ret = tas5711_set_shutdown(false);
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "tas5711_set_shutdown(false) failed: %d\n", ret);
        return;
    }

    if (!tas_cfg.start_muted)
    {
        ret = tas5711_set_mute(false);
        if (ret != BK_OK)
        {
            BK_LOGE(TAS5711_APP_TAG, "tas5711_set_mute(false) failed: %d\n", ret);
            return;
        }
    }

    ret = tas5711_dump_core_registers();
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "tas5711_dump_core_registers failed: %d\n", ret);
        return;
    }

    ret = tas5711_read_basic_status(&device_id, &error_status);
    if (ret == BK_OK)
    {
        BK_LOGI(TAS5711_APP_TAG, "TAS5711 ready, DEV_ID=0x%02X ERR=0x%02X\r\n",
                (unsigned int)device_id, (unsigned int)error_status);
    }
    else
    {
        BK_LOGE(TAS5711_APP_TAG, "TAS5711 init ok but status read failed: %d\r\n", ret);
    }
}

static void tas5711_init_task(void *arg)
{
    rtos_delay_milliseconds(100);
    tas5711_demo_init();
    rtos_delete_thread(NULL);
}

static void report_error_task(void *arg)
{
    bk_err_t ret;
    rtos_delay_milliseconds(5000);
    while (1)
    {
        ret = tas5711_dump_core_registers();
        if (ret != BK_OK)
        {
            BK_LOGE(TAS5711_APP_TAG, "tas5711_dump_core_registers failed: %d\n", ret);
            // return;
        }
        rtos_delay_milliseconds(3000);
    }
}

static bk_err_t tas5711_start_init_task(void)
{
    return rtos_create_thread(NULL,
                              5,
                              "tas5711_init_task",
                              (beken_thread_function_t)tas5711_init_task,
                              1024 * 3,
                              NULL);
}

int main(void)
{
#if (CONFIG_SYS_CPU0)
    rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
    // bk_set_printf_sync(true);
    // shell_set_log_level(BK_LOG_INFO);
#endif

    bk_init();

    media_service_init();

#if CONFIG_SYS_CPU0

    bk_err_t ret;

    ret = tas5711_start_init_task();
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5711_APP_TAG, "Failed to create TAS5711 init task: %d\n", ret);
    }

    rtos_create_thread(NULL,
                       5,
                       "Report_error",
                       (beken_thread_function_t)report_error_task,
                       1024 * 3,
                       NULL);

    if (!ate_is_enabled())
    {
        bt_manager_init();

#if AUTO_ENABLE_BLUETOOTH_DEMO
#if CONFIG_A2DP_SINK_DEMO
        extern int a2dp_sink_demo_init(uint8_t aac_supported);
        a2dp_sink_demo_init(0);
        os_printf("a2dp_sink_demo_init");
#endif

#if CONFIG_HFP_HF_DEMO
        extern int hfp_hf_demo_init(uint8_t msbc_supported);
        hfp_hf_demo_init(0);
        os_printf("hfp_hf_demo_init");
#endif

#if 0 // CONFIG_BLE
        cli_gatt_param_t param = {.rpa = 0, .p_rpa = &param.rpa, .pa = 0, .p_pa = &param.pa};

        dm_gatt_main(&param);
        dm_gatts_main(&param);
        hogpd_demo_init();
        wifi_boarding_demo_main();
#endif

#endif

#if CONFIG_BT
        extern int cli_headset_demo_init(void);
        cli_headset_demo_init();
#endif

#if CONFIG_BLE
        extern int cli_ble_gatt_demo_init(void);
        cli_ble_gatt_demo_init();
        extern int cli_ble_hogpd_demo_init(void);
        cli_ble_hogpd_demo_init();
        extern int cli_ble_wboarding_demo_init(void);
        cli_ble_wboarding_demo_init();
#endif
    }

#endif

    return 0;
}
