#include "audio_http_stream.h"

#include <os/mem.h>
#include <os/os.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"

#define TAG "DMIC_HTTP"

#define AUDIO_HTTP_SERVER_IP "10.10.60.87"
#define AUDIO_HTTP_SERVER_PORT 80
#define AUDIO_HTTP_PATH "/audio"

#define AUDIO_HTTP_QUEUE_DEPTH 12
#define AUDIO_HTTP_MAX_FRAME_BYTES 2048
#define AUDIO_HTTP_RECONNECT_DELAY_MS 1000
#define AUDIO_HTTP_TASK_STACK_SIZE 4096

typedef struct {
	uint32_t len;
	uint8_t data[AUDIO_HTTP_MAX_FRAME_BYTES];
} audio_http_frame_t;

typedef struct {
	uint32_t index;
} audio_http_msg_t;

static audio_http_frame_t s_frame_pool[AUDIO_HTTP_QUEUE_DEPTH];
static beken_queue_t s_free_queue = NULL;
static beken_queue_t s_send_queue = NULL;
static beken_thread_t s_stream_thread = NULL;
static volatile bool s_stream_running = false;
static volatile bool s_stream_started = false;
static uint32_t s_sample_rate = 16000;
static uint32_t s_channels = 1;
static uint32_t s_drop_count = 0;

static int audio_http_send_all(int sock, const void *data, uint32_t len)
{
	const uint8_t *ptr = (const uint8_t *)data;
	uint32_t sent = 0;

	while (sent < len) {
		int ret = send(sock, ptr + sent, len - sent, 0);
		if (ret <= 0)
			return -1;
		sent += ret;
	}

	return 0;
}

static int audio_http_connect(void)
{
	int sock;
	struct sockaddr_in server_addr;
	char header[256];
	int header_len;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		os_printf("%s: socket create fail\n", TAG);
		return -1;
	}

	os_memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(AUDIO_HTTP_SERVER_PORT);
	server_addr.sin_addr.s_addr = inet_addr(AUDIO_HTTP_SERVER_IP);

	if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		os_printf("%s: connect %s:%d fail\n", TAG, AUDIO_HTTP_SERVER_IP, AUDIO_HTTP_SERVER_PORT);
		closesocket(sock);
		return -1;
	}

	header_len = snprintf(header, sizeof(header),
	                      "POST %s HTTP/1.1\r\n"
	                      "Host: %s\r\n"
	                      "Content-Type: application/octet-stream\r\n"
	                      "Transfer-Encoding: chunked\r\n"
	                      "Connection: keep-alive\r\n"
	                      "X-Audio-Format: pcm_s16le\r\n"
	                      "X-Audio-Sample-Rate: %lu\r\n"
	                      "X-Audio-Channels: %lu\r\n"
	                      "\r\n",
	                      AUDIO_HTTP_PATH,
	                      AUDIO_HTTP_SERVER_IP,
	                      (unsigned long)s_sample_rate,
	                      (unsigned long)s_channels);

	if ((header_len <= 0) || (header_len >= (int)sizeof(header)) ||
	    (audio_http_send_all(sock, header, header_len) != 0)) {
		os_printf("%s: send http header fail\n", TAG);
		closesocket(sock);
		return -1;
	}

	os_printf("%s: streaming to http://%s:%d%s\n",
	          TAG, AUDIO_HTTP_SERVER_IP, AUDIO_HTTP_SERVER_PORT, AUDIO_HTTP_PATH);

	return sock;
}

static int audio_http_send_chunk(int sock, const uint8_t *data, uint32_t len)
{
	char chunk_header[16];
	int chunk_header_len;

	chunk_header_len = snprintf(chunk_header, sizeof(chunk_header), "%lx\r\n", (unsigned long)len);
	if ((chunk_header_len <= 0) || (chunk_header_len >= (int)sizeof(chunk_header)))
		return -1;

	if (audio_http_send_all(sock, chunk_header, chunk_header_len) != 0)
		return -1;
	if (audio_http_send_all(sock, data, len) != 0)
		return -1;
	if (audio_http_send_all(sock, "\r\n", 2) != 0)
		return -1;

	return 0;
}

static void audio_http_stream_thread(beken_thread_arg_t arg)
{
	int sock = -1;
	audio_http_msg_t msg;
	uint32_t sent_frames = 0;

	(void)arg;

	while (s_stream_running) {
		if (sock < 0) {
			sock = audio_http_connect();
			if (sock < 0) {
				rtos_delay_milliseconds(AUDIO_HTTP_RECONNECT_DELAY_MS);
				continue;
			}
		}

		if (rtos_pop_from_queue(&s_send_queue, &msg, 1000) != BK_OK)
			continue;

		if ((msg.index >= AUDIO_HTTP_QUEUE_DEPTH) ||
		    (s_frame_pool[msg.index].len > AUDIO_HTTP_MAX_FRAME_BYTES)) {
			continue;
		}

		if (audio_http_send_chunk(sock, s_frame_pool[msg.index].data, s_frame_pool[msg.index].len) != 0) {
			os_printf("%s: send chunk fail, reconnect\n", TAG);
			closesocket(sock);
			sock = -1;
		} else {
			sent_frames++;
			if ((sent_frames % 10) == 0) {
				os_printf("%s: sent_frames=%lu dropped=%lu\n",
				          TAG, (unsigned long)sent_frames, (unsigned long)s_drop_count);
			}
		}

		rtos_push_to_queue(&s_free_queue, &msg, BEKEN_NO_WAIT);
	}

	if (sock >= 0) {
		audio_http_send_all(sock, "0\r\n\r\n", 5);
		closesocket(sock);
	}

	s_stream_started = false;
	s_stream_thread = NULL;
	rtos_delete_thread(NULL);
}

bk_err_t audio_http_stream_start(uint32_t sample_rate, uint32_t channels)
{
	bk_err_t ret;
	audio_http_msg_t msg;

	if (s_stream_started)
		return BK_OK;

	s_sample_rate = sample_rate;
	s_channels = channels;
	s_drop_count = 0;

	ret = rtos_init_queue(&s_free_queue, "dmic_http_free", sizeof(audio_http_msg_t), AUDIO_HTTP_QUEUE_DEPTH);
	if (ret != BK_OK)
		return ret;

	ret = rtos_init_queue(&s_send_queue, "dmic_http_send", sizeof(audio_http_msg_t), AUDIO_HTTP_QUEUE_DEPTH);
	if (ret != BK_OK) {
		rtos_deinit_queue(&s_free_queue);
		s_free_queue = NULL;
		return ret;
	}

	for (uint32_t i = 0; i < AUDIO_HTTP_QUEUE_DEPTH; i++) {
		msg.index = i;
		rtos_push_to_queue(&s_free_queue, &msg, BEKEN_NO_WAIT);
	}

	s_stream_running = true;
	ret = rtos_create_thread(&s_stream_thread,
	                         BEKEN_DEFAULT_WORKER_PRIORITY,
	                         "dmic_http",
	                         audio_http_stream_thread,
	                         AUDIO_HTTP_TASK_STACK_SIZE,
	                         NULL);
	if (ret != BK_OK) {
		s_stream_running = false;
		rtos_deinit_queue(&s_send_queue);
		rtos_deinit_queue(&s_free_queue);
		s_send_queue = NULL;
		s_free_queue = NULL;
		return ret;
	}

	s_stream_started = true;
	return BK_OK;
}

bk_err_t audio_http_stream_stop(void)
{
	s_stream_running = false;
	return BK_OK;
}

bk_err_t audio_http_stream_push_frame(const uint8_t *data, uint32_t len)
{
	audio_http_msg_t msg;

	if ((!s_stream_started) || (!s_free_queue) || (!s_send_queue))
		return BK_FAIL;

	if (len > AUDIO_HTTP_MAX_FRAME_BYTES) {
		s_drop_count++;
		return BK_FAIL;
	}

	if (rtos_pop_from_queue(&s_free_queue, &msg, BEKEN_NO_WAIT) != BK_OK) {
		s_drop_count++;
		return BK_FAIL;
	}

	os_memcpy(s_frame_pool[msg.index].data, data, len);
	s_frame_pool[msg.index].len = len;

	if (rtos_push_to_queue(&s_send_queue, &msg, BEKEN_NO_WAIT) != BK_OK) {
		rtos_push_to_queue(&s_free_queue, &msg, BEKEN_NO_WAIT);
		s_drop_count++;
		return BK_FAIL;
	}

	return BK_OK;
}
