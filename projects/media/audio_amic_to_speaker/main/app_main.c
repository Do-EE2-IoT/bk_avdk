#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include "cli.h"
#include "audio_record.h"
#include "media_service.h"

#if (CONFIG_SYS_CPU0)
#include <components/event.h>
#include <components/log.h>
#include <components/netif.h>
#include <modules/wifi.h>
#include <stdbool.h>
#include <string.h>
#endif

#include "driver/gpio.h"
#include "gpio_driver.h"

#define SPEAKER_PA_PIN GPIO_50

#if (CONFIG_SYS_CPU0)
#define WIFI_SSID "LUMI"
#define WIFI_PASSWORD "lumivn274"
#define WIFI_CONNECT_TIMEOUT_MS 20000

static beken_semaphore_t s_wifi_got_ip_sem = NULL;
static volatile bool s_wifi_got_ip = false;

static bk_err_t wifi_netif_event_cb(void *arg, event_module_t event_module, int event_id, void *event_data)
{
	(void)arg;
	(void)event_module;

	if (event_id == EVENT_NETIF_GOT_IP4)
	{
		netif_event_got_ip4_t *got_ip = (netif_event_got_ip4_t *)event_data;
		if (got_ip->netif_if == NETIF_IF_STA)
		{
			s_wifi_got_ip = true;
			os_printf("wifi: STA got ip\n");
			if (s_wifi_got_ip_sem)
				rtos_set_semaphore(&s_wifi_got_ip_sem);
		}
	}

	return BK_OK;
}

static bk_err_t wifi_event_cb(void *arg, event_module_t event_module, int event_id, void *event_data)
{
	(void)arg;
	(void)event_module;

	switch (event_id)
	{
	case EVENT_WIFI_STA_CONNECTED:
	{
		wifi_event_sta_connected_t *sta_connected = (wifi_event_sta_connected_t *)event_data;
		os_printf("wifi: STA connected to %s\n", sta_connected->ssid);
		break;
	}
	case EVENT_WIFI_STA_DISCONNECTED:
	{
		wifi_event_sta_disconnected_t *sta_disconnected = (wifi_event_sta_disconnected_t *)event_data;
		s_wifi_got_ip = false;
		os_printf("wifi: STA disconnected, reason=%d\n", sta_disconnected->disconnect_reason);
		break;
	}
	default:
		break;
	}

	return BK_OK;
}

static void wifi_sta_connect(void)
{
	wifi_sta_config_t sta_config = WIFI_DEFAULT_STA_CONFIG();
	bk_err_t ret;

	if (s_wifi_got_ip_sem == NULL)
		rtos_init_semaphore(&s_wifi_got_ip_sem, 1);

	ret = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL);
	if ((ret != BK_OK) && (ret != BK_ERR_EVENT_CB_EXIST))
		os_printf("wifi: register wifi event fail, ret=%d\n", ret);

	ret = bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, wifi_netif_event_cb, NULL);
	if ((ret != BK_OK) && (ret != BK_ERR_EVENT_CB_EXIST))
		os_printf("wifi: register netif event fail, ret=%d\n", ret);

	strncpy(sta_config.ssid, WIFI_SSID, WIFI_SSID_STR_LEN);
	strncpy(sta_config.password, WIFI_PASSWORD, WIFI_PASSWORD_LEN);

	os_printf("wifi: connecting ssid=%s\n", sta_config.ssid);
	BK_LOG_ON_ERR(bk_wifi_sta_set_config(&sta_config));
	BK_LOG_ON_ERR(bk_wifi_sta_start());

	if (!s_wifi_got_ip && s_wifi_got_ip_sem)
	{
		ret = rtos_get_semaphore(&s_wifi_got_ip_sem, WIFI_CONNECT_TIMEOUT_MS);
		if (ret != BK_OK)
			os_printf("wifi: wait got ip timeout, audio stream will retry http connect\n");
	}
}
#endif

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

void enable_gpio(gpio_id_t gpio_pin)
{
	// 1. Unmap chân nếu nó đang được dùng cho chức năng khác (VD: JTAG/UART)
	gpio_dev_unmap(gpio_pin);

	// 2. Disable chức năng Input (để tránh nhiễu)
	bk_gpio_disable_input(gpio_pin);

	// 3. Enable chức năng Output
	bk_gpio_enable_output(gpio_pin);

	// 4. Set mức logic để bật Loa (Ví dụ: Active High)
	bk_gpio_set_output_high(gpio_pin);
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
	wifi_sta_connect();
#endif

#if (CONFIG_SYS_CPU0)
	if (BK_OK != audio_record_to_sdcard_start("test.mp3", 16000))
		os_printf("start audio record to sdcard fail\n");
	else
		os_printf("start audio record to sdcard ok\n");

	os_printf("%s: media service init started!\n", __func__);
#endif

// Pin enable speakeraP
	//enable_gpio(GPIO_27);

	return 0;
}
