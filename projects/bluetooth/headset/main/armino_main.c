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
#include "digital_mic.h"
#include "gpio_driver.h"
#include "tas_5805.h"

#define HEADSET_MODE_TEST_BLUETOOTH_I2S 1
#define HEADSET_MODE_DMIC_AND_I2S 2
#define HEADSET_MODE_TEST_HARDWARE_GPIO 3
#define HEADSET_MODE_TEST_DMIC_CLI 4

#ifndef HEADSET_APP_MODE
#define HEADSET_APP_MODE HEADSET_MODE_DMIC_AND_I2S
#endif

#define AUTO_ENABLE_BLUETOOTH_DEMO 1
#define TAS5805M_APP_SDA GPIO_1
#define TAS5805M_APP_SCL GPIO_0
#define TAS5805M_APP_PDN GPIO_12
#define TAS5805M_APP_ADR GPIO_28
#define TAS5805M_APP_ADR_HIGH 0
#define TAS5805M_APP_I2C_DELAY_INIT 25U
#define TAS5805M_APP_I2C_ADDRESS TAS5805M_I2C_ADDRESS_DEFAULT
#define TAS5805M_APP_TAG "TAS5805M_APP"

#define DUMP_I2C_ENABLE 0

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

static void tas5805m_demo_init(void)
{
    static tas5805m_config_t tas_cfg;
    bk_err_t ret;

    tas5805m_init_default_config(&tas_cfg);
    tas_cfg.sda_gpio = TAS5805M_APP_SDA;
    tas_cfg.scl_gpio = TAS5805M_APP_SCL;
    tas_cfg.pdn_gpio = TAS5805M_APP_PDN;
    tas_cfg.adr_gpio = TAS5805M_APP_ADR;
    tas_cfg.adr_high = TAS5805M_APP_ADR_HIGH;
    tas_cfg.i2c_address = TAS5805M_APP_I2C_ADDRESS;
    tas_cfg.delay_count = TAS5805M_APP_I2C_DELAY_INIT;
    tas_cfg.start_muted = false;
    tas_cfg.scan_bus = true;

    ret = tas5805m_init(&tas_cfg);
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5805M_APP_TAG, "tas5805m_init failed: %d\n", ret);
        return;
    }

    BK_LOGW(TAS5805M_APP_TAG, "TAS5805M power ready; I2C config will run after I2S clock starts\r\n");
}

static void tas5805m_init_task(void *arg)
{
    rtos_delay_milliseconds(100);
    tas5805m_demo_init();
    rtos_delete_thread(NULL);
}

static void hardware_gpio_test_configure_pin(gpio_id_t gpio)
{
    gpio_dev_unmap(gpio);
    bk_gpio_disable_input(gpio);
    bk_gpio_enable_output(gpio);
    bk_gpio_set_output_low(gpio);
}

static void hardware_gpio_test_task(void *arg)
{
    bool high = false;

    hardware_gpio_test_configure_pin(TAS5805M_APP_SDA);
    hardware_gpio_test_configure_pin(TAS5805M_APP_SCL);
    hardware_gpio_test_configure_pin(TAS5805M_APP_PDN);
    hardware_gpio_test_configure_pin(TAS5805M_APP_ADR);
    hardware_gpio_test_configure_pin(GPIO_8);

    BK_LOGI(TAS5805M_APP_TAG, "hardware GPIO test: toggle SDA=%d SCL=%d PDN=%d ADR=%d every 1s\r\n",
            TAS5805M_APP_SDA, TAS5805M_APP_SCL, TAS5805M_APP_PDN, TAS5805M_APP_ADR);

    while (1)
    {
        if (high)
        {
            bk_gpio_set_output_low(TAS5805M_APP_SDA);
            bk_gpio_set_output_low(TAS5805M_APP_SCL);
            bk_gpio_set_output_low(TAS5805M_APP_PDN);
            bk_gpio_set_output_low(TAS5805M_APP_ADR);
            bk_gpio_set_output_low(GPIO_8);
            os_printf("HW_GPIO_TEST: PDN GPIO_%d LOW\r\n", TAS5805M_APP_PDN);
        }
        else
        {
            bk_gpio_set_output_high(TAS5805M_APP_SDA);
            bk_gpio_set_output_high(TAS5805M_APP_SCL);
            bk_gpio_set_output_high(TAS5805M_APP_PDN);
            bk_gpio_set_output_high(TAS5805M_APP_ADR);
            bk_gpio_set_output_high(GPIO_8);
            os_printf("HW_GPIO_TEST: PDN GPIO_%d HIGH\r\n", TAS5805M_APP_PDN);
        }

        high = !high;
        rtos_delay_milliseconds(1000);
    }
}

static bk_err_t hardware_gpio_test_start(void)
{
    return rtos_create_thread(NULL,
                              5,
                              "hw_gpio_test",
                              (beken_thread_function_t)hardware_gpio_test_task,
                              1024,
                              NULL);
}

static void report_error_task(void *arg)
{
    bk_err_t ret;
    rtos_delay_milliseconds(5000);
    while (1)
    {
        if (tas5805m_is_started())
        {
            ret = tas5805m_dump_status();
            if (ret != BK_OK)
            {
                BK_LOGE(TAS5805M_APP_TAG, "tas5805m_dump_status failed: %d\n", ret);
                // return;
            }
        }
        rtos_delay_milliseconds(3000);
    }
}

static bk_err_t tas5805m_start_init_task(void)
{
    return rtos_create_thread(NULL,
                              5,
                              "tas5805m_init_task",
                              (beken_thread_function_t)tas5805m_init_task,
                              1024 * 3,
                              NULL);
}

static void dmic_cli_test_init(void)
{
#if CONFIG_CLI && CONFIG_AUDIO_TEST
#if (!defined(CLI_CFG_AUD) || (CLI_CFG_AUD != 1))
    int ret = cli_aud_init();
    os_printf("HEADSET_MODE_TEST_DMIC_CLI: cli_aud_init ret=%d\r\n", ret);
#else
    os_printf("HEADSET_MODE_TEST_DMIC_CLI: cli_aud already registered by bk_cli_init\r\n");
#endif
    os_printf("Use: aud_dmic_raw_test start 44100\r\n");
    os_printf("Use: aud_dmic_raw_test status\r\n");
    os_printf("Use: aud_dmic_raw_test read 16\r\n");
    os_printf("Use: aud_dmic_raw_test stop\r\n");
    os_printf("Use: aud_dmic_dma_test start 44100\r\n");
    os_printf("Use: aud_dmic_dma_test stop\r\n");
#else
    os_printf("HEADSET_MODE_TEST_DMIC_CLI requires CONFIG_CLI=1 and CONFIG_AUDIO_TEST=y\r\n");
#endif
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

#if (HEADSET_APP_MODE == HEADSET_MODE_TEST_HARDWARE_GPIO)
    ret = hardware_gpio_test_start();
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5805M_APP_TAG, "Failed to create hardware GPIO test task: %d\n", ret);
    }
#elif (HEADSET_APP_MODE == HEADSET_MODE_DMIC_AND_I2S)
    os_printf("HEADSET_MODE_DMIC_AND_I2S: DMIC raw example test, TAS/I2S disabled\r\n");
#elif (HEADSET_APP_MODE == HEADSET_MODE_TEST_DMIC_CLI)
    dmic_cli_test_init();
#else
    ret = tas5805m_start_init_task();
    if (ret != BK_OK)
    {
        BK_LOGE(TAS5805M_APP_TAG, "Failed to create TAS5805M init task: %d\n", ret);
    }
#endif

#if (HEADSET_APP_MODE == HEADSET_MODE_TEST_HARDWARE_GPIO)
    os_printf("HEADSET_MODE_TEST_HARDWARE_GPIO started\r\n");
#elif (HEADSET_APP_MODE == HEADSET_MODE_TEST_DMIC_CLI)
    os_printf("HEADSET_MODE_TEST_DMIC_CLI started, TAS/I2S/app DMIC are not auto-started\r\n");
#elif (HEADSET_APP_MODE == HEADSET_MODE_DMIC_AND_I2S)
#if DUMP_I2C_ENABLE
    rtos_create_thread(NULL,
                       5,
                       "Report_error",
                       (beken_thread_function_t)report_error_task,
                       1024 * 3,
                       NULL);

#endif

    if (!ate_is_enabled())
    {
        digital_mic_config_t dmic_config;

        digital_mic_init_default_config(&dmic_config);
        dmic_config.sample_rate = DIGITAL_MIC_SAMPLE_RATE_DEFAULT;
        dmic_config.frame_ms = 20;
        dmic_config.frame_count = 4;
        dmic_config.write_timeout_ms = 200;

        ret = digital_mic_start(&dmic_config);
        if (ret != BK_OK)
        {
            os_printf("DMIC_I2S ERROR: digital_mic_start failed: %d\r\n", ret);
        }
        else
        {
            os_printf("DMIC_I2S: DMIC raw example started, bluetooth/TAS/I2S disabled\r\n");
        }
    }
#elif (HEADSET_APP_MODE == HEADSET_MODE_TEST_BLUETOOTH_I2S)
#if DUMP_I2C_ENABLE
    rtos_create_thread(NULL,
                       5,
                       "Report_error",
                       (beken_thread_function_t)report_error_task,
                       1024 * 3,
                       NULL);

#endif
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
#else
#error "Unsupported HEADSET_APP_MODE"
#endif

#endif

    return 0;
}
