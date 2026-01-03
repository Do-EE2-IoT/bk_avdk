#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include <modules/wifi.h>

#include <components/event.h>
#include <components/netif.h>

#include <common/sys_config.h>
#include <components/log.h>

#include "cli.h"
#include "audio_play.h"
#include "media_service.h"
#include <driver/pwr_clk.h>
#include <string.h>


extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

static int example_wifi_event_cb(void *arg, event_module_t event_module,
								 int event_id, void *event_data)
{
	wifi_scan_result_t scan_result = {0};

	BK_LOG_ON_ERR(bk_wifi_scan_get_result(&scan_result));
	BK_LOG_ON_ERR(bk_wifi_scan_dump_result(&scan_result));
	bk_wifi_scan_free_result(&scan_result);

	return BK_OK;
}

static int example_wifi_init(void)
{
	os_printf("Init wifi now \r\n");
	wifi_init_config_t wifi_config = WIFI_DEFAULT_INIT_CONFIG();
	BK_LOG_ON_ERR(bk_event_init());
	BK_LOG_ON_ERR(bk_netif_init());
	BK_LOG_ON_ERR(bk_wifi_init(&wifi_config));
	return BK_OK;
}

static void example_event_handler_init(void)
{
	BK_LOG_ON_ERR(bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE,
									   example_wifi_event_cb, NULL));
}

static void example_scan_init(void)
{
	BK_LOG_ON_ERR(bk_wifi_scan_start(NULL));
}

#if (CONFIG_SYS_CPU0 && CONFIG_SOC_BK7236XX)
static void cli_audio_play_sdcard_mp3_music_help(void)
{
	os_printf("audio_play_sdcard_mp3_music {start|stop file_name} \r\n");
}

void cli_audio_play_sdcard_mp3_music_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc != 2 && argc != 3)
	{
		cli_audio_play_sdcard_mp3_music_help();
		return;
	}

	if (os_strcmp(argv[1], "start") == 0)
	{
		if (BK_OK != audio_play_sdcard_mp3_music_start(argv[2]))
			os_printf("start audio play sdcard mp3 music fail \n");
		else
			os_printf("start audio play sdcard mp3 music ok \n");
	}
	else if (os_strcmp(argv[1], "stop") == 0)
	{
		audio_play_sdcard_mp3_music_stop();
	}
	else
	{
		cli_audio_play_sdcard_mp3_music_help();
	}
}

#define AUDIO_PLAY_SDCARD_MP3_MUSIC_CMD_CNT (sizeof(s_audio_play_sdcard_mp3_music_commands) / sizeof(struct cli_command))
static const struct cli_command s_audio_play_sdcard_mp3_music_commands[] =
	{
		{"audio_play_sdcard_mp3_music", "audio_play_sdcard_mp3_music {start|stop file_name}", cli_audio_play_sdcard_mp3_music_cmd},
};

int cli_audio_play_sdcard_mp3_music_init(void)
{
	os_printf("cli_audio_play_sdcard_mp3_music_init \n");

	return cli_register_commands(s_audio_play_sdcard_mp3_music_commands, AUDIO_PLAY_SDCARD_MP3_MUSIC_CMD_CNT);
}

void user_app_main(void)
{
	cli_audio_play_sdcard_mp3_music_init();
}
#endif // #if (CONFIG_SYS_CPU0 && CONFIG_SOC_BK7236XX)

static void example_sta_connect(void)
{
	wifi_sta_config_t sta_config = WIFI_DEFAULT_STA_CONFIG();

	strncpy(sta_config.ssid, "ALL LUMI", WIFI_SSID_STR_LEN);
	strncpy(sta_config.password, "lumivn274", WIFI_PASSWORD_LEN);

	os_printf("ssid:%s password:%s\n", sta_config.ssid, sta_config.password);
	BK_LOG_ON_ERR(bk_wifi_sta_set_config(&sta_config));
	BK_LOG_ON_ERR(bk_wifi_sta_start());
}

int main(void)
{
	#if (CONFIG_SYS_CPU0)
		rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
	#endif
	bk_init();
	media_service_init();

#if (CONFIG_SYS_CPU0)
	//	bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_AUDP_AUDIO, PM_POWER_MODULE_STATE_ON);

	// example_wifi_init();
	// example_event_handler_init();
	// example_scan_init();

	// example_sta_connect();

	
#endif

		return 0;
}
