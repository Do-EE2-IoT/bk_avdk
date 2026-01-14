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

	BK_LOG_ON_ERR(bk_adc_driver_init());

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
//	bk_pm_module_vote_boot_cp1_ctrl(PM_BOOT_CP1_MODULE_NAME_AUDP_AUDIO, PM_POWER_MODULE_STATE_ON);
#endif

	rtos_create_thread(NULL,
					   4,		   // Độ ưu tiên
					   "adc_task", // Tên Task
					   (beken_thread_function_t)adc_simple_task,
					   1024 * 4, // Stack size
					   NULL);

	return 0;
}
