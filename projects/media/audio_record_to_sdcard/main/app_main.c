#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include "cli.h"
#include "audio_record.h"
#include "media_service.h"
#include <modules/wifi.h>
#include <components/netif.h>
#include <components/event.h>
#ifdef CONFIG_WEBSOCKET
#include "bk_websocket_client.h"
#endif

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

/* ---- WiFi STA ---- */
#define WIFI_SSID "PHUC DUNG"
#define WIFI_PASSWORD "18071990"

/* ---- WebSocket auto-connect ---- */
#ifdef CONFIG_WEBSOCKET

#define WS_SERVER_URI "ws://192.168.1.8:8765"
#define WS_AUTO_CONNECT_STACK_SIZE 3072
#define WS_AUTO_CONNECT_PRIORITY 5

static void ws_event_cb(int32_t event_id, char *event_data, int data_len)
{
	switch (event_id)
	{
	case WEBSOCKET_EVENT_CONNECTED:
		os_printf("[WS] Connected to " WS_SERVER_URI "\n");
		break;
	case WEBSOCKET_EVENT_DISCONNECTED:
		os_printf("[WS] Disconnected\n");
		break;
	case WEBSOCKET_EVENT_CLOSED:
		os_printf("[WS] Closed\n");
		break;
	case WEBSOCKET_EVENT_ERROR:
		os_printf("[WS] Error\n");
		break;
	default:
		break;
	}
}

static void ws_auto_connect_task(beken_thread_arg_t arg)
{
	websocket_client_input_t cfg = {
		.uri = WS_SERVER_URI,
		.buffer_size = 0,
		.rx_retry = 0,
	};

	while (1)
	{
		os_printf("[WS] Connecting to %s ...\n", WS_SERVER_URI);
		if (websocket_start(&cfg) == BK_OK)
		{
			os_printf("[WS] WebSocket connected!\n");
			break;
		}
		os_printf("[WS] Connect failed, retry in 5s...\n");
		rtos_delay_milliseconds(5000);
	}
	rtos_delete_thread(NULL);
}

static void ws_auto_connect_start(void)
{
	bk_err_t ret = rtos_create_thread(NULL,
									  WS_AUTO_CONNECT_PRIORITY,
									  "ws_auto_connect",
									  (beken_thread_function_t)ws_auto_connect_task,
									  WS_AUTO_CONNECT_STACK_SIZE,
									  NULL);
	if (ret != BK_OK)
		os_printf("[WS] Failed to create auto-connect task\n");
}

#endif /* CONFIG_WEBSOCKET */

static int wifi_netif_event_cb(void *arg, event_module_t event_module,
							   int event_id, void *event_data)
{
	if (event_module == EVENT_MOD_NETIF && event_id == EVENT_NETIF_GOT_IP4)
	{
		netif_event_got_ip4_t *got_ip = (netif_event_got_ip4_t *)event_data;
		os_printf("[WIFI] Got IP on %s\n",
				  got_ip->netif_if == NETIF_IF_STA ? "STA" : "unknown");
#ifdef CONFIG_WEBSOCKET
		bk_websocket_register_cb(ws_event_cb);
		ws_auto_connect_start();
#endif
	}
	return BK_OK;
}

static int wifi_sta_event_cb(void *arg, event_module_t event_module,
							 int event_id, void *event_data)
{
	switch (event_id)
	{
	case EVENT_WIFI_STA_CONNECTED:
		os_printf("[WIFI] STA connected\n");
		break;
	case EVENT_WIFI_STA_DISCONNECTED:
	{
		wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
		os_printf("[WIFI] STA disconnected, reason=%d\n", disc->disconnect_reason);
		break;
	}
	default:
		break;
	}
	return BK_OK;
}

static void wifi_sta_start(void)
{
	/* bk_event_init / bk_netif_init / bk_wifi_init đã được gọi bởi bk_init(),
	 * không gọi lại để tránh lỗi ALREADY_INITED và watchdog. */
	BK_LOG_ON_ERR(bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_sta_event_cb, NULL));
	BK_LOG_ON_ERR(bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, wifi_netif_event_cb, NULL));

	wifi_sta_config_t sta_config = WIFI_DEFAULT_STA_CONFIG();
	os_strncpy(sta_config.ssid, WIFI_SSID, WIFI_SSID_STR_LEN);
	os_strncpy(sta_config.password, WIFI_PASSWORD, WIFI_PASSWORD_LEN);

	os_printf("[WIFI] Connecting to SSID: %s\n", sta_config.ssid);
	BK_LOG_ON_ERR(bk_wifi_sta_set_config(&sta_config));
	BK_LOG_ON_ERR(bk_wifi_sta_start());
}

void user_app_main(void)
{
	cli_audio_record_init();
	wifi_sta_start();
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

	return 0;
}
