#include <components/event.h>
#include <components/netif.h>
#include <modules/wifi.h>
#include <os/mem.h>
#include <os/os.h>
#include <os/str.h>

#ifdef CONFIG_WEBSOCKET
#include "bk_websocket_client.h"
#endif

#include "app_ws.h"
#include "audio_play.h"

#define APP_WS_TAG "APP_WS"
#define APP_WS_WIFI_SSID "LUMI"
#define APP_WS_WIFI_PASSWORD "lumivn274"
#define APP_WS_SERVER_URI "ws://10.10.60.87:8765"
#define APP_WS_PCM_SAMPLE_RATE 48000U
#define APP_WS_PCM_CHANNELS 2U
#define APP_WS_PCM_BITS_PER_SAMPLE 16U
#define APP_WS_QUEUE_DEPTH 8U
#define APP_WS_PCM_TASK_STACK_SIZE 3072U
#define APP_WS_PCM_TASK_PRIORITY 6U
#define APP_WS_CONNECT_TASK_STACK_SIZE 3072U
#define APP_WS_CONNECT_TASK_PRIORITY 5U
#define APP_WS_CONNECT_RETRY_MS 5000U
#define APP_WS_PCM_WRITE_TIMEOUT_MS 1000U
#define APP_WS_RX_BUFFER_SIZE 4096U

typedef struct
{
    uint8_t *buf;
    uint32_t len;
} app_ws_pcm_msg_t;

static beken_queue_t s_app_ws_queue = NULL;
static beken_thread_t s_app_ws_pcm_thread = NULL;
static beken_thread_t s_app_ws_connect_thread = NULL;
static bool s_app_ws_started = false;
static bool s_app_ws_have_ip = false;
static bool s_app_ws_pcm_ready = false;

static void app_ws_flush_queue(void)
{
    app_ws_pcm_msg_t msg;

    if (s_app_ws_queue == NULL)
    {
        return;
    }

    while (rtos_pop_from_queue(&s_app_ws_queue, &msg, BEKEN_NO_WAIT) == BK_OK)
    {
        if (msg.buf != NULL)
        {
            os_free(msg.buf);
        }
    }
}

static void app_ws_reset_audio(void)
{
    app_ws_flush_queue();

    if (s_app_ws_pcm_ready)
    {
        audio_play_pcm_i2s_stop();
        s_app_ws_pcm_ready = false;
    }
}

static void app_ws_pcm_task(beken_thread_arg_t arg)
{
    app_ws_pcm_msg_t msg;

    while (1)
    {
        if (rtos_pop_from_queue(&s_app_ws_queue, &msg, BEKEN_WAIT_FOREVER) != BK_OK)
        {
            continue;
        }

        if (msg.buf == NULL)
        {
            continue;
        }

        if (s_app_ws_pcm_ready)
        {
            if (audio_play_pcm_i2s_write(msg.buf, msg.len, APP_WS_PCM_WRITE_TIMEOUT_MS) != BK_OK)
            {
                BK_LOGW(APP_WS_TAG, "PCM frame dropped, len=%lu\n", (unsigned long)msg.len);
            }else
            {
                BK_LOGW(APP_WS_TAG, "PCM frame written, len=%lu\n", (unsigned long)msg.len);
            }
        }

        os_free(msg.buf);
    }
}

#ifdef CONFIG_WEBSOCKET
static void app_ws_schedule_connect(void);

static void app_ws_event_cb(int32_t event_id, char *event_data, int data_len)
{
    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        BK_LOGI(APP_WS_TAG, "[WS] Connected to %s\n", APP_WS_SERVER_URI);
        if (!s_app_ws_pcm_ready)
        {
            if (audio_play_pcm_i2s_start(APP_WS_PCM_SAMPLE_RATE,
                                         APP_WS_PCM_CHANNELS,
                                         APP_WS_PCM_BITS_PER_SAMPLE) == BK_OK)
            {
                s_app_ws_pcm_ready = true;
                os_printf("PCM I2s start ok\n");
            }
            else
            {
                BK_LOGE(APP_WS_TAG, "Failed to start PCM playback\n");
            }
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if ((event_data == NULL) || (data_len <= 0) || (s_app_ws_queue == NULL))
        {
            os_printf("Invalid WS data event, len=%d\n", data_len);
            break;
        }

        os_printf("WS rx bytes: %d\r\n", data_len);

        if (!s_app_ws_pcm_ready)
        {
            break;
        }

        {
            app_ws_pcm_msg_t msg;

            msg.buf = os_malloc((uint32_t)data_len);
            if (msg.buf == NULL)
            {
                BK_LOGE(APP_WS_TAG, "No memory for PCM frame, len=%d\n", data_len);
                break;
            }

            os_memcpy(msg.buf, event_data, (uint32_t)data_len);
            msg.len = (uint32_t)data_len;

            if (rtos_push_to_queue(&s_app_ws_queue, &msg, BEKEN_NO_WAIT) != BK_OK)
            {
                BK_LOGW(APP_WS_TAG, "PCM queue full, dropped len=%d\n", data_len);
                os_free(msg.buf);
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        BK_LOGW(APP_WS_TAG, "[WS] Disconnected\n");
        app_ws_reset_audio();
        if (s_app_ws_started && s_app_ws_have_ip)
        {
            app_ws_schedule_connect();
        }
        break;

    case WEBSOCKET_EVENT_CLOSED:
        BK_LOGW(APP_WS_TAG, "[WS] Closed\n");
        app_ws_reset_audio();
        if (s_app_ws_started && s_app_ws_have_ip)
        {
            app_ws_schedule_connect();
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        BK_LOGE(APP_WS_TAG, "[WS] Error\n");
        app_ws_reset_audio();
        if (s_app_ws_started && s_app_ws_have_ip)
        {
            app_ws_schedule_connect();
        }
        break;

    default:
        break;
    }
}

static void app_ws_connect_task(beken_thread_arg_t arg)
{
    websocket_client_input_t cfg = {
        .uri = APP_WS_SERVER_URI,
        .buffer_size = APP_WS_RX_BUFFER_SIZE,
        .rx_retry = 0,
    };

    while (s_app_ws_started && s_app_ws_have_ip)
    {
        if (websocket_is_connected())
        {
            break;
        }

        BK_LOGI(APP_WS_TAG, "[WS] Connecting to %s\n", APP_WS_SERVER_URI);
        if (websocket_start(&cfg) == BK_OK)
        {
            break;
        }

        BK_LOGW(APP_WS_TAG, "[WS] Connect failed, retry in %lu ms\n",
                (unsigned long)APP_WS_CONNECT_RETRY_MS);
        rtos_delay_milliseconds(APP_WS_CONNECT_RETRY_MS);
    }

    s_app_ws_connect_thread = NULL;
    rtos_delete_thread(NULL);
}

static void app_ws_schedule_connect(void)
{
    bk_err_t ret;

    if (!s_app_ws_started || !s_app_ws_have_ip || websocket_is_connected())
    {
        return;
    }

    if (s_app_ws_connect_thread != NULL)
    {
        return;
    }

    ret = rtos_create_thread(&s_app_ws_connect_thread,
                             APP_WS_CONNECT_TASK_PRIORITY,
                             "app_ws_conn",
                             (beken_thread_function_t)app_ws_connect_task,
                             APP_WS_CONNECT_TASK_STACK_SIZE,
                             NULL);
    if (ret != BK_OK)
    {
        s_app_ws_connect_thread = NULL;
        BK_LOGE(APP_WS_TAG, "Failed to create WS connect task: %d\n", ret);
    }
}
#endif

static int app_ws_wifi_netif_event_cb(void *arg, event_module_t event_module,
                                      int event_id, void *event_data)
{
    if ((event_module == EVENT_MOD_NETIF) && (event_id == EVENT_NETIF_GOT_IP4))
    {
        netif_event_got_ip4_t *got_ip = (netif_event_got_ip4_t *)event_data;

        s_app_ws_have_ip = true;
        BK_LOGI(APP_WS_TAG, "[WIFI] Got IP on %s\n",
                got_ip->netif_if == NETIF_IF_STA ? "STA" : "unknown");
#ifdef CONFIG_WEBSOCKET
        bk_websocket_register_cb(app_ws_event_cb);
        app_ws_schedule_connect();
#endif
    }

    return BK_OK;
}

static int app_ws_wifi_sta_event_cb(void *arg, event_module_t event_module,
                                    int event_id, void *event_data)
{
    if (event_module != EVENT_MOD_WIFI)
    {
        return BK_OK;
    }

    switch (event_id)
    {
    case EVENT_WIFI_STA_CONNECTED:
        BK_LOGI(APP_WS_TAG, "[WIFI] STA connected\n");
        break;

    case EVENT_WIFI_STA_DISCONNECTED:
    {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;

        s_app_ws_have_ip = false;
        BK_LOGW(APP_WS_TAG, "[WIFI] STA disconnected, reason=%d\n",
                disc != NULL ? disc->disconnect_reason : -1);
#ifdef CONFIG_WEBSOCKET
        websocket_stop();
#endif
        app_ws_reset_audio();
        break;
    }

    default:
        break;
    }

    return BK_OK;
}

static bk_err_t app_ws_queue_init(void)
{
    bk_err_t ret;

    if (s_app_ws_queue != NULL)
    {
        return BK_OK;
    }

    ret = rtos_init_queue(&s_app_ws_queue, "app_ws_pcm_q",
                          sizeof(app_ws_pcm_msg_t), APP_WS_QUEUE_DEPTH);
    if (ret != BK_OK)
    {
        BK_LOGE(APP_WS_TAG, "Failed to init WS PCM queue: %d\n", ret);
        s_app_ws_queue = NULL;
        return ret;
    }

    ret = rtos_create_thread(&s_app_ws_pcm_thread,
                             APP_WS_PCM_TASK_PRIORITY,
                             "app_ws_pcm",
                             (beken_thread_function_t)app_ws_pcm_task,
                             APP_WS_PCM_TASK_STACK_SIZE,
                             NULL);
    if (ret != BK_OK)
    {
        BK_LOGE(APP_WS_TAG, "Failed to create WS PCM task: %d\n", ret);
        rtos_deinit_queue(&s_app_ws_queue);
        s_app_ws_queue = NULL;
        s_app_ws_pcm_thread = NULL;
        return ret;
    }

    return BK_OK;
}

static void app_ws_queue_deinit(void)
{
    if (s_app_ws_pcm_thread != NULL)
    {
        rtos_delete_thread(&s_app_ws_pcm_thread);
        s_app_ws_pcm_thread = NULL;
    }

    app_ws_flush_queue();

    if (s_app_ws_queue != NULL)
    {
        rtos_deinit_queue(&s_app_ws_queue);
        s_app_ws_queue = NULL;
    }
}

bk_err_t app_ws_start(void)
{
    bk_err_t ret;
    wifi_sta_config_t sta_config = WIFI_DEFAULT_STA_CONFIG();

    if (s_app_ws_started)
    {
        return BK_OK;
    }

    ret = app_ws_queue_init();
    if (ret != BK_OK)
    {
        return ret;
    }

    ret = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL,
                               app_ws_wifi_sta_event_cb, NULL);
    if (ret != BK_OK)
    {
        BK_LOGE(APP_WS_TAG, "Failed to register WIFI event cb: %d\n", ret);
        app_ws_queue_deinit();
        return ret;
    }

    ret = bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL,
                               app_ws_wifi_netif_event_cb, NULL);
    if (ret != BK_OK)
    {
        BK_LOGE(APP_WS_TAG, "Failed to register NETIF event cb: %d\n", ret);
        app_ws_queue_deinit();
        return ret;
    }

    os_strncpy(sta_config.ssid, APP_WS_WIFI_SSID, WIFI_SSID_STR_LEN);
    os_strncpy(sta_config.password, APP_WS_WIFI_PASSWORD, WIFI_PASSWORD_LEN);

    BK_LOGI(APP_WS_TAG, "[WIFI] Connecting to SSID: %s\n", sta_config.ssid);
    ret = bk_wifi_sta_set_config(&sta_config);
    if (ret != BK_OK)
    {
        BK_LOGE(APP_WS_TAG, "bk_wifi_sta_set_config failed: %d\n", ret);
        app_ws_queue_deinit();
        return ret;
    }

    ret = bk_wifi_sta_start();
    if (ret != BK_OK)
    {
        BK_LOGE(APP_WS_TAG, "bk_wifi_sta_start failed: %d\n", ret);
        app_ws_queue_deinit();
        return ret;
    }

    s_app_ws_started = true;
    return BK_OK;
}

bk_err_t app_ws_stop(void)
{
    s_app_ws_started = false;
    s_app_ws_have_ip = false;

#ifdef CONFIG_WEBSOCKET
    websocket_stop();
#endif

    if (s_app_ws_connect_thread != NULL)
    {
        rtos_delete_thread(&s_app_ws_connect_thread);
        s_app_ws_connect_thread = NULL;
    }

    app_ws_reset_audio();
    app_ws_queue_deinit();
    return BK_OK;
}
