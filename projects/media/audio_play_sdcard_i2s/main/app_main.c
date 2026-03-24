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
#include "driver/adc.h"
#include <driver/i2c.h>

/* ---------- OTA qua GitHub HTTPS ---------- */
#include "ota_github.h"
#include <modules/wifi.h>
#include <components/event.h>
#include <components/netif.h>

#include "tas_5711.h"

/* =========================================================
 * I2C Scan dung Sim I2C Driver V2 (GPIO bit-bang)
 * SDA = GPIO_5,  SCL = GPIO_8  (xem sim_i2c_driver_v2.c)
 * ========================================================= */
#define I2C_SCAN_SDA GPIO_1
#define I2C_SCAN_SCL GPIO_0
#define I2C_SCAN_TAG "I2C_SCAN"

/* Delay don gian bang busy-wait (tuong tu driver v2, ~100 kHz) */
#define I2C_SCAN_DELAY_COUNT 25

static inline void scan_delay(void)
{
	volatile uint32_t i;
	for (i = 0; i < I2C_SCAN_DELAY_COUNT; i++)
	{
	}
}

static inline void scan_sda_high(void)
{
	bk_gpio_disable_input(I2C_SCAN_SDA);
	bk_gpio_enable_output(I2C_SCAN_SDA);
	bk_gpio_set_output_high(I2C_SCAN_SDA);
}
static inline void scan_sda_low(void)
{
	bk_gpio_disable_input(I2C_SCAN_SDA);
	bk_gpio_enable_output(I2C_SCAN_SDA);
	bk_gpio_set_output_low(I2C_SCAN_SDA);
}
static inline void scan_scl_high(void)
{
	bk_gpio_disable_input(I2C_SCAN_SCL);
	bk_gpio_enable_output(I2C_SCAN_SCL);
	bk_gpio_set_output_high(I2C_SCAN_SCL);
}
static inline void scan_scl_low(void)
{
	bk_gpio_disable_input(I2C_SCAN_SCL);
	bk_gpio_enable_output(I2C_SCAN_SCL);
	bk_gpio_set_output_low(I2C_SCAN_SCL);
}
static inline uint8_t scan_read_sda(void)
{
	bk_gpio_disable_output(I2C_SCAN_SDA);
	bk_gpio_enable_input(I2C_SCAN_SDA);
	return (uint8_t)bk_gpio_get_input(I2C_SCAN_SDA);
}

/* ---- Tin hieu START / STOP ---- */
static void scan_i2c_start(void)
{
	scan_sda_high();
	scan_scl_high();
	scan_delay();
	scan_sda_low(); /* SDA xuong trong khi SCL cao -> START */
	scan_delay();
	scan_scl_low();
	scan_delay();
}

static void scan_i2c_stop(void)
{
	scan_scl_low();
	scan_sda_low();
	scan_delay();
	scan_scl_high();
	scan_delay();
	scan_sda_high(); /* SDA len trong khi SCL cao -> STOP */
	scan_delay();
}

/* ---- Gui 8 bit, tra ve 1 neu nhan duoc ACK (SDA = 0) ---- */
static int scan_i2c_send_byte(uint8_t byte)
{
	uint8_t mask;
	for (mask = 0x80; mask; mask >>= 1)
	{
		if (byte & mask)
			scan_sda_high();
		else
			scan_sda_low();
		scan_delay();
		scan_scl_high();
		scan_delay();
		scan_scl_low();
		scan_delay();
	}
	/* Doc bit ACK */
	scan_delay();
	scan_scl_high();
	scan_delay();
	uint8_t ack = (scan_read_sda() == 0); /* ACK = SDA thap */
	scan_scl_low();
	scan_delay();
	return ack;
}

/* ---- Probe mot dia chi 7-bit: 1 = co thiet bi, 0 = khong ---- */
static int scan_i2c_probe(uint8_t addr)
{
	scan_i2c_start();
	int ack = scan_i2c_send_byte((uint8_t)((addr << 1) | 0)); /* Write mode */
	scan_i2c_stop();
	return ack;
}

void tas5711_demo_init(void)
{
	static tas5711_config_t tas_cfg;
	bk_err_t ret;

	tas5711_init_default_config(&tas_cfg);
	tas_cfg.sda_gpio = I2C_SCAN_SDA;
	tas_cfg.scl_gpio = I2C_SCAN_SCL;
	tas_cfg.reset_gpio = GPIO_13;
	tas_cfg.pdn_gpio = GPIO_12;
	tas_cfg.start_muted = true;

	ret = tas5711_init(&tas_cfg);
	if (ret != BK_OK)
	{
		BK_LOGE("APP", "tas5711_init failed: %d\n", ret);
		return;
	}

	tas5711_configure_serial_audio(TAS5711_SERIAL_FORMAT_I2S, 16);
	tas5711_set_master_volume(0x30);
	tas5711_set_channel_volume(0x30, 0x30);
	tas5711_set_mute(false);
}

/* ---- Task chinh: quet toan bo dia chi I2C 7-bit (0x00 - 0x7F) ---- */
void i2c_scan_task(void *arg)
{
	/* 1. Init GPIO (unmap khoi chuc nang phan cung, cau hinh pull-up) */
	gpio_dev_unmap(I2C_SCAN_SDA);
	gpio_dev_unmap(I2C_SCAN_SCL);
	bk_gpio_pull_up(I2C_SCAN_SDA);
	bk_gpio_pull_up(I2C_SCAN_SCL);

	/* 2. Dua bus ve trang thai IDLE */
	scan_sda_high();
	scan_scl_high();
	rtos_delay_milliseconds(100);

	BK_LOGW(I2C_SCAN_TAG, "===== Bat dau quet dia chi I2C (0x00 - 0x7F) =====\r\n");
	BK_LOGW(I2C_SCAN_TAG, "SDA = GPIO_%d  |  SCL = GPIO_%d\r\n",
			(int)I2C_SCAN_SDA, (int)I2C_SCAN_SCL);

	int found = 0;
	uint8_t addr;
	for (addr = 0x00; addr <= 0x7F; addr++)
	{
		rtos_delay_milliseconds(2); /* cho bus on dinh giua moi lan probe */
		if (scan_i2c_probe(addr))
		{
			BK_LOGW(I2C_SCAN_TAG, "  [TIM THAY] Thiet bi tai dia chi: 0x%02X\r\n", addr);
			found++;
		}
	}

	BK_LOGW(I2C_SCAN_TAG, "===== Quet xong! Tim thay %d thiet bi =====\r\n", found);

	/* Task ket thuc, giai phong */
	rtos_delete_thread(NULL);
}

/* ---- Task chinh: quet toan bo dia chi I2C 7-bit (0x00 - 0x7F) ---- */
void test_task(void *arg)
{
	gpio_dev_unmap(GPIO_2);
	gpio_dev_unmap(GPIO_13);
	gpio_dev_unmap(GPIO_29);
	bk_gpio_disable_input(GPIO_2);
	bk_gpio_enable_output(GPIO_2);

	bk_gpio_disable_input(GPIO_13);
	bk_gpio_enable_output(GPIO_13);

	bk_gpio_disable_input(GPIO_29);
	bk_gpio_enable_output(GPIO_29);
	while (1)
	{
		bk_gpio_set_output_high(GPIO_2);
		bk_gpio_set_output_high(GPIO_13);
		bk_gpio_set_output_high(GPIO_29);
		rtos_delay_milliseconds(500);
		bk_gpio_set_output_low(GPIO_2);
		bk_gpio_set_output_low(GPIO_13);
		bk_gpio_set_output_low(GPIO_29);
		rtos_delay_milliseconds(500);
	}
	/* Task ket thuc, giai phong */
	rtos_delete_thread(NULL);
}

#define ADC_BUF_SIZE_1 10
#define ADC_DETECT_CNT_MAX 0x1000
#define ADC_DETECT_CHANNEL_14 ADC_14
#define ADC_DETECT_CLK 75000
#define ADC_DETECT_SAMPLE_RATE 32
#define ADC_DETECT_STEADY_CTRL 7
#define ADC_READ_TIMEOUT 2000
#if CONFIG_SYS_CPU0
extern float gain;
#endif
typedef struct
{
	adc_config_t adc_config;
	int task_id;
	int delay_ms;
} adc_task_config_t;

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

void adc_simple_task(void *arg)
{
	bk_err_t ret;
	uint16_t adc_raw_value = 0;
	adc_config_t config = {0};

	// BK_LOGI(TAG, "Khoi tao ADC...\n");

	// 1. Khởi tạo Driver và xin quyền truy cập
	bk_adc_driver_init();
	bk_adc_acquire();

	// 2. Cấu hình thông số ADC
	bk_adc_init(ADC_DETECT_CHANNEL_14);

	config.chan = ADC_DETECT_CHANNEL_14;
	config.adc_mode = ADC_CONTINUOUS_MODE; // Chế độ đọc liên tục
	config.src_clk = ADC_SCLK_XTAL_26M;
	config.clk = ADC_DETECT_CLK;
	config.saturate_mode = ADC_SATURATE_MODE_3;
	config.sample_rate = ADC_DETECT_SAMPLE_RATE;
	config.steady_ctrl = ADC_DETECT_STEADY_CTRL;
	config.adc_filter = 0;

	ret = bk_adc_set_config(&config);
	if (ret != BK_OK)
	{
		BK_LOGE("ADC", "Loi cau hinh ADC!\n");
		goto exit_task;
	}

	// 3. Bắt đầu ADC
	bk_adc_start();
	BK_LOGI("ADC", "ADC da bat dau, doc moi 500ms...\n");

	// 4. Vòng lặp chính (Loop)
	while (1)
	{
		// Đọc giá trị ADC (Hàm này sẽ chờ đến khi có dữ liệu hoặc timeout)
		ret = bk_adc_read(&adc_raw_value, ADC_READ_TIMEOUT);

		if (ret == BK_OK)
		{
			// In giá trị raw (0 - 4095)
			BK_LOGW("ADC", "Chan: %d | Gia tri Raw: %d\n", ADC_DETECT_CHANNEL_14, adc_raw_value);
			if (adc_raw_value > 4000 && adc_raw_value < 5000)
			{
#if CONFIG_SYS_CPU0
				gain = 0.25;
#endif
				BK_LOGW("ADC", "Set speaker volume to 25 per \r\n");
			}
			else if (adc_raw_value > 2500 && adc_raw_value < 3000)
			{
#if CONFIG_SYS_CPU0
				gain = 0.5;
#endif
				BK_LOGW("ADC", "Set speaker volume to 50 per \r\n");
			}
			else if (adc_raw_value > 1000 && adc_raw_value < 2000)
			{
#if CONFIG_SYS_CPU0
				gain = 0.75;
#endif
				BK_LOGW("ADC", "Set speaker volume to 75 per \r\n");
			}
			else if (adc_raw_value > 8000)
			{
#if CONFIG_SYS_CPU0
				gain = 1.0;
#endif
				BK_LOGW("ADC", "Set speaker volume to 100 per \r\n");
			}
			else
			{
			}

			// --- Nếu bạn muốn tính ra Vol (Ví dụ cho sơ đồ nút bấm cũ) ---
			// float voltage = (adc_raw_value * 2.4) / 4096.0; // Giả sử Vref = 2.4V
			// BK_LOGI(TAG, "Dien ap: %.2f V\n", voltage);
		}
		else
		{
			BK_LOGE("ADC", "Loi doc data ADC\n");
		}

		// 5. Ngủ 500ms
		rtos_delay_milliseconds(500);
	}

exit_task:
	// Nếu thoát vòng lặp thì dọn dẹp (thường không chạy tới đây)
	bk_adc_stop();
	bk_adc_release();
	rtos_delete_thread(NULL);
}

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

/* ------------------------------------------------------------------
 * Wi-Fi event handler: chạy OTA task sau khi STA connected.
 * Chỉ chạy một lần (flag s_ota_started).
 * NOTE: EVENT_WIFI_STA_CONNECTED fire khi associate xong, DHCP
 *       chưa kịp hoàn thành. Task OTA đã có delay 5s bên trong
 *       để chờ DHCP cấp IP trước khi query GitHub.
 * ------------------------------------------------------------------ */
static bool s_ota_started = false;

static bk_err_t wifi_event_handler(void *arg, event_module_t event_module,
								   int event_id, void *event_data)
{
	if (event_id == EVENT_WIFI_STA_CONNECTED && !s_ota_started)
	{
		s_ota_started = true;
		// ota_github_start();
	}
	return BK_OK;
}

void user_app_main(void)
{
	cli_audio_play_sdcard_i2s_init();

	/* Đăng ký event handler để trigger OTA sau khi got IP */
	bk_event_register_cb(EVENT_MOD_WIFI, EVENT_ID_ALL,
						 wifi_event_handler, NULL);
}
#endif // #if (CONFIG_SYS_CPU0 && CONFIG_SOC_BK7236XX)

int main(void)
{
#if (CONFIG_SYS_CPU0)
	rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#endif
	bk_init();
	media_service_init();

	wifi_sta_config_t sta_config = WIFI_DEFAULT_STA_CONFIG();

	strncpy(sta_config.ssid, "LUMI", WIFI_SSID_STR_LEN);
	strncpy(sta_config.password, "lumivn274", WIFI_PASSWORD_LEN);

	os_printf("ssid:%s password:%s\n", sta_config.ssid, sta_config.password);
#if CONFIG_WIFI_ENABLE
	BK_LOG_ON_ERR(bk_wifi_sta_set_config(&sta_config));
	BK_LOG_ON_ERR(bk_wifi_sta_start());
	os_printf("Wi-Fi STA started, connecting to AP...\n");
#endif

	// BK_LOG_ON_ERR(bk_adc_driver_init());

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

	audio_play_sdcard_i2s_start("test_320kbps.mp3");
	bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_AUDP_AUDIO, PM_POWER_MODULE_STATE_ON);
#endif

// rtos_create_thread(NULL,
// 				   4,		   // Độ ưu tiên
// 				   "adc_task", // Tên Task
// 				   (beken_thread_function_t)adc_simple_task,
// 				   1024 * 4, // Stack size
// 				   NULL);
#if CONFIG_SYS_CPU0
	gain = 0.25;
#endif

	/* Tao task I2C scan */
	rtos_create_thread(NULL,
					   5,				// Uu tien
					   "i2c_scan_task", // Ten task
					   (beken_thread_function_t)i2c_scan_task,
					   1024 * 3, // Stack size
					   NULL);

	//tas5711_demo_init();

	// rtos_create_thread(NULL,
	// 				   5,			// Uu tien
	// 				   "test_task", // Ten task
	// 				   (beken_thread_function_t)test_task,
	// 				   1024 * 3, // Stack size
	// 				   NULL);

	return 0;
}
