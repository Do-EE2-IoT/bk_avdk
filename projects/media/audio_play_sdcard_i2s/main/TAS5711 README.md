# TAS5711 README

## 1. Mục đích của bộ mã này

Thư mục này hiện có thêm hai file mới:

- `tas_5711.h`
- `tas_5711.c`

Hai file này là bản port TAS5711 cho nền tảng BKAVDK/Beken, viết theo kiểu nhúng trực tiếp, không dùng Linux ASoC, không dùng `regmap`, không dùng `i2c_client` như file `.example`.

Driver mới sử dụng đúng kiểu I2C software bit-bang bằng GPIO giống logic đang có trong `app_main.c`.

## 2. Điểm khác nhau giữa file `.example` và bản mới

File `.example` là driver Linux cho nhiều chip trong họ TAS571x. Nó phụ thuộc vào:

- Linux kernel I2C
- Linux regmap
- Linux ASoC
- regulator
- clk
- gpio descriptor

Trong project BK hiện tại các phụ thuộc đó không tồn tại. Vì vậy bản mới được viết lại để chỉ giữ lại phần cần thiết cho TAS5711:

- đọc và ghi thanh ghi qua I2C bit-bang
- reset và power-down bằng GPIO nếu có nối chân
- nạp một nhóm thanh ghi mặc định cơ bản
- cấu hình format audio serial
- mute, shutdown, volume
- ghi block 20 byte cho biquad

## 3. Các file và vai trò

### `tas_5711.h`

Chứa:

- macro địa chỉ thanh ghi TAS5711/TAS571x cần dùng
- `tas5711_config_t` để cấu hình chân và hành vi khởi tạo
- các API công khai để ứng dụng gọi

### `tas_5711.c`

Chứa:

- I2C bit-bang nội bộ bằng GPIO
- context driver singleton
- sequence khởi tạo và reset
- hàm đọc/ghi thanh ghi 1 byte, 4 byte và block 20 byte
- các API điều khiển TAS5711

## 4. Cấu hình đang có trong code

### 4.1. Cấu hình chân mặc định

Hàm `tas5711_init_default_config()` gán mặc định:

- `sda_gpio = GPIO_1`
- `scl_gpio = GPIO_0`
- `reset_gpio = GPIO_NUM` để biểu thị không dùng
- `pdn_gpio = GPIO_NUM` để biểu thị không dùng
- `i2c_address = 0x1A`
- `delay_count = 25`
- `post_reset_delay_ms = 50`
- `apply_default_registers = true`
- `start_muted = true`

Điều này bám theo cách quét I2C đang có trong `app_main.c`, nơi bus scan cũng đang dùng `GPIO_1` và `GPIO_0`.

### 4.2. Cấu hình thanh ghi mặc định

Driver hiện nạp nhóm thanh ghi mặc định cơ bản sau:

- `0x04` (`TAS571X_SDI_REG`) = `0x05`
- `0x05` (`TAS571X_SYS_CTRL_2_REG`) = `0x40`
- `0x06` (`TAS571X_SOFT_MUTE_REG`) = `0x00`
- `0x07` (`TAS571X_MVOL_REG`) = `0xFF`
- `0x08` (`TAS571X_CH1_VOL_REG`) = `0x30`
- `0x09` (`TAS571X_CH2_VOL_REG`) = `0x30`
- `0x1B` (`TAS571X_OSC_TRIM_REG`) = `0x82`

Lý do chỉ nạp nhóm này là vì đây là nhóm cấu hình cơ bản và an toàn nhất được suy ra từ driver mẫu cho TAS5711, đủ để tạo một driver nhúng gọn và dễ kiểm soát.

### 4.3. Chuỗi khởi tạo

`tas5711_init()` đang thực hiện theo thứ tự:

1. Lưu cấu hình vào context nội bộ.
2. Unmap hai chân SDA/SCL khỏi chức năng khác.
3. Bật pull-up cho SDA/SCL.
4. Đưa bus về trạng thái idle.
5. Nếu có `pdn_gpio` hoặc `reset_gpio` thì cấu hình chúng ở mode output.
6. Gọi `tas5711_reset()`.
7. Ghi `OSC_TRIM = 0x00`.
8. Delay 50 ms.
9. Nếu `apply_default_registers = true` thì nạp bảng default.
10. Thiết lập mute ban đầu theo `start_muted`.
11. Clear shutdown để IC chạy.

### 4.4. Cấu hình format audio serial

API `tas5711_configure_serial_audio()` hỗ trợ ba mode:

- `TAS5711_SERIAL_FORMAT_RIGHT_JUSTIFIED`
- `TAS5711_SERIAL_FORMAT_I2S`
- `TAS5711_SERIAL_FORMAT_LEFT_JUSTIFIED`

Ánh xạ vào thanh ghi `TAS571X_SDI_REG` như sau:

- Right-Justified: base `0x00`
- I2S: base `0x03`
- Left-Justified: base `0x06`

Nếu `sample_bits >= 20` thì cộng thêm `1`.

Nếu `sample_bits >= 24` thì cộng thêm `2`.

Đây là logic được rút gọn từ driver tham chiếu, chuyển thành API nhúng dễ dùng.

### 4.5. Cấu hình retry I2C

Driver có retry nội bộ:

- `TAS5711_I2C_TIMEOUT_RETRY = 2`

Nếu một phiên ghi hoặc đọc không nhận ACK, driver sẽ stop bus, delay ngắn 1 ms rồi thử lại.

### 4.6. Kích thước thanh ghi mà code đang hỗ trợ

Driver tự suy luận kích thước dữ liệu theo thanh ghi:

- đa số thanh ghi control thông thường: 1 byte
- `INPUT_MUX`, `CH4_SRC_SELECT`, `PWM_MUX`: 4 byte
- các thanh ghi Biquad `TAS5707_CHx_BQx_REG`: 20 byte

Vì vậy:

- `tas5711_write_register()` và `tas5711_read_register()` dùng cho thanh ghi có kích thước tối đa 4 byte
- `tas5711_write_block()` và `tas5711_read_block()` dùng cho block tùy ý
- `tas5711_write_biquad()` dùng riêng cho một block coefficient 20 byte

## 5. Danh sách API công khai

### 5.1. Cấu hình và trạng thái

- `void tas5711_init_default_config(tas5711_config_t *config);`
- `const tas5711_config_t *tas5711_get_config(void);`
- `bool tas5711_is_ready(void);`

### 5.2. Vòng đời driver

- `bk_err_t tas5711_init(const tas5711_config_t *config);`
- `bk_err_t tas5711_deinit(void);`
- `bk_err_t tas5711_reset(void);`
- `bk_err_t tas5711_apply_default_config(void);`

### 5.3. Truy cập thanh ghi

- `bk_err_t tas5711_write_register(uint8_t reg, uint32_t value);`
- `bk_err_t tas5711_read_register(uint8_t reg, uint32_t *value);`
- `bk_err_t tas5711_write_block(uint8_t reg, const uint8_t *data, uint32_t size);`
- `bk_err_t tas5711_read_block(uint8_t reg, uint8_t *data, uint32_t size);`
- `bk_err_t tas5711_write_biquad(uint8_t reg, const uint32_t coefficients[5]);`

### 5.4. Điều khiển audio

- `bk_err_t tas5711_set_shutdown(bool enable);`
- `bk_err_t tas5711_set_mute(bool mute);`
- `bk_err_t tas5711_set_master_volume(uint8_t value);`
- `bk_err_t tas5711_set_channel_volume(uint8_t ch1_value, uint8_t ch2_value);`
- `bk_err_t tas5711_configure_serial_audio(tas5711_serial_format_t format, uint8_t sample_bits);`
- `bk_err_t tas5711_read_basic_status(uint32_t *device_id, uint32_t *error_status);`

### 5.5. Bảng default đang nạp

- `const tas5711_reg_default_t *tas5711_get_default_registers(uint32_t *count);`

API này hữu ích nếu bạn muốn log ra toàn bộ bảng preset đang được driver dùng.

## 6. Cách dùng nhanh trong `app_main.c`

Ví dụ tối thiểu:

```c
#include "tas_5711.h"

static void tas5711_demo_init(void)
{
	static tas5711_config_t tas_cfg;
	bk_err_t ret;

	tas5711_init_default_config(&tas_cfg);
	tas_cfg.sda_gpio = GPIO_1;
	tas_cfg.scl_gpio = GPIO_0;
	tas_cfg.reset_gpio = GPIO_2;
	tas_cfg.pdn_gpio = GPIO_13;
	tas_cfg.start_muted = true;

	ret = tas5711_init(&tas_cfg);
	if (ret != BK_OK) {
		BK_LOGE("APP", "tas5711_init failed: %d\n", ret);
		return;
	}

	tas5711_configure_serial_audio(TAS5711_SERIAL_FORMAT_I2S, 16);
	tas5711_set_master_volume(0x30);
	tas5711_set_channel_volume(0x30, 0x30);
	tas5711_set_mute(false);
}
```

## 7. Ý nghĩa một số API quan trọng

### `tas5711_set_shutdown()`

- `true`: ghi bit shutdown vào `SYS_CTRL_2`
- `false`: clear shutdown để IC hoạt động

### `tas5711_set_mute()`

- `true`: mute đồng thời CH1 và CH2
- `false`: bỏ mute cả hai kênh

### `tas5711_set_master_volume()`

Ghi trực tiếp thanh ghi `MVOL`. Đây là raw register value, không phải phần trăm.

### `tas5711_set_channel_volume()`

Ghi trực tiếp `CH1_VOL_REG` và `CH2_VOL_REG`.

### `tas5711_write_biquad()`

Nhận 5 hệ số 32-bit và pack theo big-endian thành 20 byte để ghi xuống một thanh ghi biquad.

## 8. Các lưu ý phần cứng

### 8.1. Pull-up I2C

Code có bật pull-up nội bộ cho SDA/SCL. Tuy nhiên với bus I2C thực tế vẫn nên có pull-up ngoài nếu đường dài hoặc tốc độ cao.

### 8.2. Chân reset và PDN

Driver cho phép bỏ trống hai chân này bằng cách giữ:

- `reset_gpio = TAS5711_GPIO_UNUSED`
- `pdn_gpio = TAS5711_GPIO_UNUSED`

Nếu board của bạn có nối các chân đó, nên khai báo để sequence khởi tạo ổn định hơn.

### 8.3. Địa chỉ I2C

Mặc định driver dùng `0x1A`. Nếu strap phần cứng của TAS5711 làm địa chỉ khác thì sửa `i2c_address` trong `tas5711_config_t`.

## 9. Các giới hạn hiện tại của bản port

Driver này cố ý giữ phạm vi gọn để dễ dùng trên firmware hiện tại. Vì vậy nó chưa bao gồm:

- mixer control kiểu ALSA
- tự động đồng bộ cache thanh ghi
- parser device tree
- nhiều profile chip khác ngoài TAS5711
- API volume theo dB thực
- API đọc/ghi coefficient phức tạp ngoài block 20 byte cơ bản

Nếu cần, có thể mở rộng tiếp theo các hướng sau:

- thêm preset thanh ghi đầy đủ cho pipeline audio cụ thể
- thêm API volume theo dB hoặc percent đã hiệu chuẩn
- thêm CLI command để đọc ghi TAS5711 trực tiếp từ shell
- nối driver này vào `app_main.c` để init ngay khi boot

## 10. Gợi ý trình tự vận hành ổn định

Trình tự nên dùng ở ứng dụng:

1. Tạo `tas5711_config_t`.
2. Gọi `tas5711_init_default_config()`.
3. Gán lại các chân đúng theo board.
4. Gọi `tas5711_init()`.
5. Gọi `tas5711_configure_serial_audio()` để khớp với nguồn I2S của hệ thống.
6. Gọi `tas5711_set_master_volume()` và `tas5711_set_channel_volume()`.
7. Khi dữ liệu audio đã sẵn sàng thì `tas5711_set_mute(false)`.
8. Khi tắt hệ thống hoặc muốn im lặng an toàn thì `tas5711_set_mute(true)` rồi `tas5711_set_shutdown(true)`.

## 11. Build system

`CMakeLists.txt` trong thư mục này đã được cập nhật để thêm:

- `tas_5711.c`

Vì vậy từ bây giờ file driver sẽ được biên dịch cùng project `audio_play_sdcard_i2s`.
