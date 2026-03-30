#include "bk_private/bk_init.h"

#include <components/system.h>
#include <os/os.h>

#include "app_ws.h"
#include "audio_play.h"
#include "bk_gpio.h"
#include "gpio_driver.h"
#include "media_service.h"
#include "tas_5711.h"

#define TAS5711_APP_SDA GPIO_1
#define TAS5711_APP_SCL GPIO_0
#define TAS5711_APP_RESET GPIO_13
#define TAS5711_APP_PDN GPIO_2
#define TAS5711_APP_I2C_DELAY_INIT 25U
#define TAS5711_APP_ENABLE_GPIO GPIO_12
#define TAS5711_APP_TAG "TAS5711_APP"

#if CONFIG_SYS_CPU0
extern float gain;
#endif

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

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
	tas_cfg.start_muted = true;

	ret = tas5711_init(&tas_cfg);
	if (ret != BK_OK)
	{
		BK_LOGE(TAS5711_APP_TAG, "tas5711_init failed: %d\n", ret);
		return;
	}

	tas5711_configure_serial_audio(TAS5711_SERIAL_FORMAT_I2S, 16);
	tas5711_set_master_volume(0x30);
	tas5711_set_channel_volume(0x30, 0x30);
	tas5711_set_mute(false);

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

static bk_err_t tas5711_start_init_task(void)
{
	return rtos_create_thread(NULL,
							  5,
							  "tas5711_init_task",
							  (beken_thread_function_t)tas5711_init_task,
							  1024 * 3,
							  NULL);
}

static void audio_board_enable(void)
{
	gpio_dev_unmap(TAS5711_APP_ENABLE_GPIO);
	bk_gpio_disable_input(TAS5711_APP_ENABLE_GPIO);
	bk_gpio_enable_output(TAS5711_APP_ENABLE_GPIO);
	bk_gpio_set_output_high(TAS5711_APP_ENABLE_GPIO);
}

void user_app_main(void)
{
}

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
	media_service_init();

	audio_board_enable();

#if CONFIG_SYS_CPU0
	bk_err_t ret;
	ret = tas5711_start_init_task();
	if (ret != BK_OK)
	{
		BK_LOGE(TAS5711_APP_TAG, "Failed to create TAS5711 init task: %d\n", ret);
	}
	ret = app_ws_start();
	if (ret != BK_OK)
	{
		BK_LOGE(TAS5711_APP_TAG, "Failed to start WS audio app: %d\n", ret);
	}
#endif

	return 0;
}
