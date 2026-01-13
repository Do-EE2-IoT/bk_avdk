#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include "cli.h"
#include "audio_play.h"
#include "media_service.h"
#include <driver/pwr_clk.h>
#include "bk_gpio.h"
#include "gpio_driver.h"

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

#if (CONFIG_SYS_CPU0 && CONFIG_SOC_BK7236XX)
static void cli_audio_play_sdcard_i2s_help(void)
{
	os_printf("audio_play_sdcard_i2s {start|stop file_name} \r\n");
}

void cli_audio_play_sdcard_i2s_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc != 2 && argc != 3)
	{
		cli_audio_play_sdcard_i2s_help();
		return;
	}

	if (os_strcmp(argv[1], "start") == 0)
	{
		if (BK_OK != audio_play_sdcard_i2s_start(argv[2]))
			os_printf("start audio play sdcard i2s fail \n");
		else
			os_printf("start audio play sdcard i2s ok \n");
	}
	else if (os_strcmp(argv[1], "stop") == 0)
	{
		audio_play_sdcard_i2s_stop();
	}
	else
	{
		cli_audio_play_sdcard_i2s_help();
	}
}

#define AUDIO_PLAY_SDCARD_I2S_CMD_CNT (sizeof(s_audio_play_sdcard_i2s_commands) / sizeof(struct cli_command))
static const struct cli_command s_audio_play_sdcard_i2s_commands[] =
	{
		{"audio_play_sdcard_i2s", "audio_play_sdcard_i2s {start|stop file_name}", cli_audio_play_sdcard_i2s_cmd},
};

int cli_audio_play_sdcard_i2s_init(void)
{
	os_printf("cli_audio_play_sdcard_i2s_init \n");

	return cli_register_commands(s_audio_play_sdcard_i2s_commands, AUDIO_PLAY_SDCARD_I2S_CMD_CNT);
}

void user_app_main(void)
{
	cli_audio_play_sdcard_i2s_init();
}
#endif // #if (CONFIG_SYS_CPU0 && CONFIG_SOC_BK7236XX)

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
	media_service_init();

	// // bk_gpio_config_output(14);
	// // bk_gpio_config_output(15);
	// // bk_gpio_config_output(16);
	// gpio_dev_unmap(2);
	// bk_gpio_disable_input(2);
	// bk_gpio_enable_output(2);

	// gpio_dev_unmap(3);
	// bk_gpio_disable_input(3);
	// bk_gpio_enable_output(3);

	// gpio_dev_unmap(4);
	// bk_gpio_disable_input(4);
	// bk_gpio_enable_output(4);
	// while(1){
	// 	bk_gpio_set_output_high(2);
	// 	bk_gpio_set_output_high(3);
	// 	bk_gpio_set_output_high(4);
	// 	rtos_delay_milliseconds(3000);
	// 	bk_gpio_set_output_low(2);
	// 	bk_gpio_set_output_low(3);
	// 	bk_gpio_set_output_low(4);
	// 	rtos_delay_milliseconds(3000);
	// }




#if (CONFIG_SYS_CPU0)
//	bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_AUDP_AUDIO, PM_POWER_MODULE_STATE_ON);
#endif

	return 0;
}
