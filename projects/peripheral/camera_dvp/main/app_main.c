#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>

#include "media_service.h"
#include "driver/media_types.h"

#include <components/log.h>
#include <modules/wifi.h>
#include <components/event.h>
#include <components/netif.h>
#include <generated/lmac_wifi_adapter.h>

#include <os/str.h>
#include <os/mem.h>

#include <modules/jpeg_decode_sw.h>
#include <modules/tjpgd.h>

#ifdef CONFIG_LWIP_V2_1
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#endif

#if (CONFIG_SYS_CPU0)
#include "media_app.h"
#include "media_evt.h"

static bool sending_image_enabled = false;

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);
extern void media_read_frame_callback(frame_buffer_t *frame);
sw_jpeg_dec_res_t result;

/* forward declaration for TCP send helper (defined later) */
int tcp_client_send_frame(const void *data, int len);

media_camera_device_t camera_device = {
	.type = DVP_CAMERA,
	.mode = JPEG_MODE,
	.fmt = PIXEL_FMT_JPEG,
	.info.fps = FPS30,
	.info.resolution.width = 640,
	.info.resolution.height = 480,
};

static void start_sending_data(void) {
    sending_image_enabled = true;
}

static void stop_sending_data(void) {
    sending_image_enabled = false;
}


static void media_read_frame_info_callback(frame_buffer_t *frame)
{
	if(sending_image_enabled) {
		os_printf("##MJPEG:camera_type:%d(1:dvp 2:uvc) frame_id:%d, length:%d, frame_addr:%p \r\n",frame->type,frame->sequence, frame->length, frame->frame);
	}
	
	bk_jpeg_get_img_info(frame->length, frame->frame, &result);

	if(sending_image_enabled) 
	{
		sending_image_enabled = false;
		
		os_printf("##DECODE:pixel_x:%d, pixel_y:%d\n\r", result.pixel_x,result.pixel_y);
		os_printf("rotate_angle:%d(0:0 1:90 2:180 3:270)\n\r",jd_get_rotate());
		os_printf("byte_order:%d(0:little endian 1:big endian)\n\r",jd_get_byte_order());
	
		/* forward raw MJPEG frame to TCP server (if connected) */
		tcp_client_send_frame(frame->frame, frame->length);

		switch(jd_get_format())
		{
			case JD_FORMAT_RGB888:
				os_printf("out_fmt:RGB888\r\n\n");
				break;

			case JD_FORMAT_RGB565:
				os_printf("out_fmt:RGB565\r\n\n");
				break;

			case JD_FORMAT_Grayscale:
				os_printf("out_fmt:Grayscale\r\n\n");
				break;

			case JD_FORMAT_YUYV:
				os_printf("out_fmt:YUYV\r\n\n");
				break;

			case JD_FORMAT_VYUY:
				os_printf("out_fmt:VYUY\r\n\n");
				break;

			case JD_FORMAT_VUYY:
				os_printf("out_fmt:VUYY\r\n\n");
				break;

			default:
				break;
		}
	}
}

/* --- TCP client to send frames to remote server ------------------------- */
#define REMOTE_SERVER_IP   "10.10.30.5"
#define REMOTE_SERVER_PORT 60000

static int tcp_client_fd = -1;
static beken_mutex_t tcp_client_lock;

static int tcp_client_create_socket(void)
{
	int fd = -1;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	return fd;
}

static int tcp_client_connect_server(int fd)
{
	struct sockaddr_in server_addr;
	ip4_addr_t remote_ip;
	int ret;

	if (!inet_aton(REMOTE_SERVER_IP, &remote_ip.addr)) {
		os_printf("invalid server ip %s\r\n", REMOTE_SERVER_IP);
		return -1;
	}

	os_memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = remote_ip.addr;
	server_addr.sin_port = htons(REMOTE_SERVER_PORT);

	ret = connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
	if (ret == 0) {
		os_printf("tcp connect to %s:%d ok\r\n", REMOTE_SERVER_IP, REMOTE_SERVER_PORT);
		return 0;
	}

	os_printf("tcp connect to %s:%d failed %d\r\n", REMOTE_SERVER_IP, REMOTE_SERVER_PORT, ret);
	return -1;
}

static int tcp_client_send_all(int fd, const void *buf, int len)
{
	const uint8_t *p = (const uint8_t *)buf;
	int remaining = len;
	while (remaining > 0) {
		int sent = send(fd, p, remaining, 0);
		if (sent <= 0) {
			return -1;
		}
		remaining -= sent;
		p += sent;
	}
	return len;
}

int tcp_client_send_frame(const void *data, int len)
{
	int fd;
	int ret = -1;
	uint32_t netlen;

	if (len <= 0 || data == NULL)
		return -1;

	rtos_lock_mutex(&tcp_client_lock);
	fd = tcp_client_fd;
	if (fd < 0) {
		rtos_unlock_mutex(&tcp_client_lock);
		return -1;
	}

	/* send 4-byte big-endian length prefix then payload */
	netlen = htonl((uint32_t)len);
	ret = tcp_client_send_all(fd, &netlen, sizeof(netlen));
	if (ret > 0)
		ret = tcp_client_send_all(fd, data, len);

	if (ret < 0) {
		os_printf("tcp send failed, closing fd %d\r\n", fd);
		close(fd);
		tcp_client_fd = -1;
	}

	rtos_unlock_mutex(&tcp_client_lock);
	return ret;
}

void tcp_client_thread(beken_thread_arg_t arg)
{
	int fd = -1;

	os_printf("%s: TCP client thread started\r\n", __func__);
	if (rtos_init_mutex(&tcp_client_lock) != BK_OK) {
		os_printf("tcp: mutex init failed\r\n");
		rtos_delete_thread(NULL);
		return;
	}

	while (1) {
		/* create socket */
		fd = tcp_client_create_socket();
		if (fd < 0) {
			os_printf("tcp: create socket failed\r\n");
			rtos_delay_milliseconds(1000);
			continue;
		}

		/* try connect; retry until success */
		if (tcp_client_connect_server(fd) == 0) {
			rtos_lock_mutex(&tcp_client_lock);
			tcp_client_fd = fd;
			rtos_unlock_mutex(&tcp_client_lock);

			/* stay connected; if socket closed we will detect on send and reconnect */
			while (tcp_client_fd >= 0) {
				rtos_delay_milliseconds(1000);
			}
		} else {
			close(fd);
			fd = -1;
			rtos_delay_milliseconds(2000);
		}
	}

	rtos_delete_thread(NULL);
}

/* --- TCP receive thread: read data sent from remote server ---------------- */
void tcp_client_recv_thread(beken_thread_arg_t arg)
{
	int fd;
	int ret;
	char buf[2048];

	os_printf("%s: TCP recv thread started\r\n", __func__);

	while (1) {
		/* wait until a connection exists */
		rtos_lock_mutex(&tcp_client_lock);
		fd = tcp_client_fd;
		rtos_unlock_mutex(&tcp_client_lock);

		if (fd < 0) {
			/* no connection yet */
			rtos_delay_milliseconds(500);
			continue;
		}

		/* try to receive data; non-blocking read is not assumed, so use small timeout via delay loop */
		ret = recv(fd, buf, sizeof(buf), 0);
		if (ret > 0) {
			os_printf("tcp recv %d bytes\r\n", ret);

			// Thêm ký tự kết thúc chuỗi
			if (ret < sizeof(buf))
				buf[ret] = '\0';
			else
				buf[sizeof(buf) - 1] = '\0';

			// In ra dữ liệu nhận được
			os_printf("Recv data: %s\r\n", buf);

			// Kiểm tra flag
			if (strstr(buf, "CAM_START") != NULL) {
				os_printf("Received START flag -> begin sending data\r\n");
				start_sending_data();   
			} 
			else if (strstr(buf, "CAM_STOP") != NULL) {
				os_printf("Received STOP flag -> stop sending data\r\n");
				stop_sending_data();    
			} 
			else {
				os_printf("Unknown message, ignore\r\n");
			}
		}
 		else if (ret == 0) {
			/* orderly shutdown by peer */
			os_printf("tcp: remote closed connection\r\n");
			rtos_lock_mutex(&tcp_client_lock);
			if (tcp_client_fd == fd) {
				close(tcp_client_fd);
				tcp_client_fd = -1;
			}
			rtos_unlock_mutex(&tcp_client_lock);
			rtos_delay_milliseconds(1000);
		} 
		else {
			/* error */
			os_printf("tcp: recv error %d, closing\r\n", ret);
			rtos_lock_mutex(&tcp_client_lock);
			if (tcp_client_fd == fd) {
				close(tcp_client_fd);
				tcp_client_fd = -1;
			}
			rtos_unlock_mutex(&tcp_client_lock);
			rtos_delay_milliseconds(1000);
		}
	}

	rtos_delete_thread(NULL);
}


/* --- end TCP client ---------------------------------------------------- */

void dvp_debug_init (void)
{
	bk_err_t ret;
	bk_jpeg_dec_sw_init(NULL, 0);

	ret =media_app_camera_open(&camera_device);
	if (ret != BK_OK)
	{
		os_printf("media_app_camera_open failed\r\n");
	}

	ret =media_app_register_read_frame_callback(camera_device.fmt, media_read_frame_info_callback);
	if (ret != BK_OK)
	{
		os_printf("media_app_register_read_frame_callback failed\r\n");
	}
}

/* Fallback Wi-Fi credentials (override with project Kconfig/config) */
#ifndef CONFIG_EXAMPLE_WIFI_SSID
#define CONFIG_EXAMPLE_WIFI_SSID "ALL LUMI"
#endif
#ifndef CONFIG_EXAMPLE_WIFI_PASSWORD
#define CONFIG_EXAMPLE_WIFI_PASSWORD "lumivn274"
#endif

int wifi_netif_event_cb(void *arg, event_module_t event_module,
					   int event_id, void *event_data)
{
	netif_event_got_ip4_t *got_ip;

	switch (event_id) {
	case EVENT_NETIF_GOT_IP4:
		got_ip = (netif_event_got_ip4_t *)event_data;
		os_printf("%s: got ip\n", got_ip->netif_if == NETIF_IF_STA ? "STA" : "unknown netif");
		break;
	default:
		os_printf("%s: rx event <%d %d>\n", __func__, event_module, event_id);
		break;
	}

	return BK_OK;
}

int wifi_event_cb(void *arg, event_module_t event_module,
					  int event_id, void *event_data)
{
	wifi_event_sta_disconnected_t *sta_disconnected;
	wifi_event_sta_connected_t *sta_connected;

	switch (event_id) {
	case EVENT_WIFI_STA_CONNECTED:
		sta_connected = (wifi_event_sta_connected_t *)event_data;
		os_printf("%s: STA connected to %s\n", __func__, sta_connected->ssid);
		break;

	case EVENT_WIFI_STA_DISCONNECTED:
		sta_disconnected = (wifi_event_sta_disconnected_t *)event_data;
		os_printf("%s: STA disconnected, reason(%d)\n", __func__, sta_disconnected->disconnect_reason);
		break;

	default:
		os_printf("%s: rx event <%d %d>\n", __func__, event_module, event_id);
		break;
	}

	return BK_OK;
}

static int wifi_init(void)
{
	wifi_init_config_t wifi_config = WIFI_DEFAULT_INIT_CONFIG();
	BK_LOG_ON_ERR(bk_event_init());
	BK_LOG_ON_ERR(bk_netif_init());
	BK_LOG_ON_ERR(bk_wifi_init(&wifi_config));
	return BK_OK;
}

static void wifi_event_handler_init(void)
{
	BK_LOG_ON_ERR(bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL, wifi_event_cb, NULL));
	BK_LOG_ON_ERR(bk_event_register_cb(EVENT_MOD_NETIF, EVENT_ID_ALL, wifi_netif_event_cb, NULL));
}

static void wifi_sta_connect(void)
{
	wifi_sta_config_t sta_config = WIFI_DEFAULT_STA_CONFIG();

	strncpy(sta_config.ssid, CONFIG_EXAMPLE_WIFI_SSID, WIFI_SSID_STR_LEN);
	strncpy(sta_config.password, CONFIG_EXAMPLE_WIFI_PASSWORD, WIFI_PASSWORD_LEN);

	os_printf("%s: ssid:%s password:%s\n", __func__, sta_config.ssid, sta_config.password);
	BK_LOG_ON_ERR(bk_wifi_sta_set_config(&sta_config));
	BK_LOG_ON_ERR(bk_wifi_sta_start());
}

/* Deferred connect thread: wait a bit for camera and other subsystems to stabilize,
 * then register event handlers and start STA. This avoids resource contention at boot.
 */
static void camera_wifi_connect_thread(beken_thread_arg_t arg)
{
	/* wait 1 second */
	rtos_delay_milliseconds(1000);

	/* bk_init() in main() already calls app_wifi_init() which performs
	 * bk_event_init(), bk_netif_init() and bk_wifi_init(). Calling
	 * wifi_init() here would re-register event callbacks and
	 * may return BK_ERR_EVENT_CB_EXIST (duplicate registration). Avoid
	 * re-initializing the wifi stack; just register handlers and start STA.
	 */
	// wifi_init(); /* removed to avoid duplicate init */
	wifi_event_handler_init();
	wifi_sta_connect();

	while (1)
	{
		rtos_delay_milliseconds(1000);
	}
	
}

void user_app_main(void)
{
	
}
#endif

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
    media_service_init();
#if (CONFIG_SYS_CPU0)
	dvp_debug_init();
#endif

#if (CONFIG_SYS_CPU0)
    /* start deferred connect thread (does nothing if wifi already initialized) */
    rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY,
                       "cam_wifi", (beken_thread_function_t)camera_wifi_connect_thread,
                       8 * 1024, 0);

	/* start tcp client thread to forward frames to remote server */
	rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY,
					   "tcp_client", (beken_thread_function_t)tcp_client_thread,
					   8 * 1024, 0);

	/* start tcp receive thread to process data from remote server */
	rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY,
					   "tcp_recv", (beken_thread_function_t)tcp_client_recv_thread,
					   8 * 1024, 0);
#endif

	return 0;
}