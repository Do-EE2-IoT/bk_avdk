#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include "cli.h"
#include "audio_record.h"
#include "media_service.h"

#include "driver/gpio.h"
#include "gpio_driver.h"

#define SPEAKER_PA_PIN GPIO_50

void speaker_pa_enable(void)
{
	// 1. Unmap chân nếu nó đang được dùng cho chức năng khác (VD: JTAG/UART)
	gpio_dev_unmap(SPEAKER_PA_PIN);

	// 2. Disable chức năng Input (để tránh nhiễu)
	bk_gpio_disable_input(SPEAKER_PA_PIN);

	// 3. Enable chức năng Output
	bk_gpio_enable_output(SPEAKER_PA_PIN);

	// 4. Set mức logic để bật Loa (Ví dụ: Active High)
	bk_gpio_set_output_high(SPEAKER_PA_PIN);

	os_printf("Speaker PA enabled on GPIO %d\n", SPEAKER_PA_PIN);

}

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

#if (CONFIG_SYS_CPU0)
static void cli_audio_record_to_sdcard_help(void)
{
	os_printf("audio_record_to_sdcard {start|stop file_name sample_rate} \r\n");
}

void cli_audio_record_to_sdcard_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	uint32_t samp_rate = 8000;

	if (argc < 2)
	{
		cli_audio_record_to_sdcard_help();
		return;
	}

	if (os_strcmp(argv[1], "start") == 0)
	{
		if (argc < 4)
		{
			cli_audio_record_to_sdcard_help();
			return;
		}

		samp_rate = strtoul(argv[3], NULL, 10);
		if (BK_OK != audio_record_to_sdcard_start(argv[2], samp_rate))
			os_printf("start audio record to sdcard fail\n");
		else
			os_printf("start audio record to sdcard ok\n");
	}
	else if (os_strcmp(argv[1], "stop") == 0)
	{
		audio_record_to_sdcard_stop();
	}
	else
	{
		cli_audio_record_to_sdcard_help();
	}
}

#define AUDIO_RECORD_CMD_CNT (sizeof(s_audio_record_commands) / sizeof(struct cli_command))
static const struct cli_command s_audio_record_commands[] =
	{
		{"audio_record_to_sdcard", "audio_record_to_sdcard {start|stop file_name sample_rate}", cli_audio_record_to_sdcard_cmd},
};

int cli_audio_record_init(void)
{
	return cli_register_commands(s_audio_record_commands, AUDIO_RECORD_CMD_CNT);
}

void user_app_main(void)
{
	cli_audio_record_init();
}
#endif

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
	int media_service_init(void);
	media_service_init();

#if (CONFIG_SYS_CPU0)
	if (BK_OK != audio_record_to_sdcard_start("test.mp3", 16000))
		os_printf("start audio record to sdcard fail\n");
	else
		os_printf("start audio record to sdcard ok\n");

	os_printf("%s: media service init started!\n", __func__);
#endif

	speaker_pa_enable();

	return 0;
}
