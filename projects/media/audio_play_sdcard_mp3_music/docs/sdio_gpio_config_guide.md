# Hướng Dẫn Cấu Hình GPIO cho SDIO/SD Card - BK7258

## ⚠️ QUAN TRỌNG: Chip BK7258 vs BK7256

> [!CAUTION]
> **Chip BK7258 và BK7256 sử dụng DRIVER KHÁC NHAU!**
> 
> - **BK7256XX**: Sử dụng `sdio_driver.c` (driver cũ)
> - **BK7258**: Sử dụng `sdio_host_driver.c` (driver mới) ✅

Bạn đang dùng **BK7258**, nên phải làm việc với driver mới!

---

## Vấn Đề Bạn Gặp Phải

Bạn đã thử sửa file `bk_idk/middleware/soc/bk7258_cp2/soc/gpio_map.h` nhưng không hiệu quả vì:
1. File `gpio_map.h` chỉ là **định nghĩa** (define), không phải nơi cấu hình thực tế
2. Bạn có thể đang sửa sai file (`bk7258_cp2` thay vì `bk7258`)

---

## Kiến Trúc SDIO trên BK7258

### 1. Định Nghĩa GPIO Mapping

**File**: [`bk_idk/middleware/soc/bk7258/soc/gpio_map.h`](file:///home/do_lumi/armino-avdk/bk_avdk/bk_idk/middleware/soc/bk7258/soc/gpio_map.h#L192-L212)

```c
#if (CONFIG_PIN_SDIO_GROUP_0)
#define SDIO_HOST_GPIO_MAP \
{\
	{GPIO_14, GPIO_DEV_SDIO_HOST_CLK},\
	{GPIO_15, GPIO_DEV_SDIO_HOST_CMD},\
	{GPIO_16, GPIO_DEV_SDIO_HOST_DATA0},\
	{GPIO_17, GPIO_DEV_SDIO_HOST_DATA1},\
	{GPIO_18, GPIO_DEV_SDIO_HOST_DATA2},\
	{GPIO_19, GPIO_DEV_SDIO_HOST_DATA3},\
}
#else
#define SDIO_HOST_GPIO_MAP \
{\
	{GPIO_2, GPIO_DEV_SDIO_HOST_CLK},\
	{GPIO_3, GPIO_DEV_SDIO_HOST_CMD},\
	{GPIO_4, GPIO_DEV_SDIO_HOST_DATA0},\
	{GPIO_5, GPIO_DEV_SDIO_HOST_DATA1},\
	{GPIO_10, GPIO_DEV_SDIO_HOST_DATA2},\
	{GPIO_11, GPIO_DEV_SDIO_HOST_DATA3},\
}
#endif
```

### 2. Sử Dụng GPIO Map trong Driver

**File**: [`bk_idk/middleware/driver/sdio_host/sdio_host_driver.c`](file:///home/do_lumi/armino-avdk/bk_avdk/bk_idk/middleware/driver/sdio_host/sdio_host_driver.c#L102-L139)

```c
static void sdio_host_init_gpio(void)
{
	const sdio_host_gpio_map_t sdio_host_gpio_map_table[] = SDIO_HOST_GPIO_MAP;

	/* set gpio sdio host map: clk,cmd,data0 */
	for (uint32_t i = SDIO_HOST_GPIO_CLK_INDEX; i < SDIO_HOST_GPIO_PIN_NUMBER; i++) {
		gpio_dev_unmap(sdio_host_gpio_map_table[i].gpio_id);
	}

	// Mapping GPIO...
	gpio_sdio_sel(GPIO_SDIO_MAP_MODE0);

	/* sdio host clk */
	bk_gpio_pull_up(sdio_host_gpio_map_table[SDIO_HOST_GPIO_CLK_INDEX].gpio_id);
	bk_gpio_set_capacity(sdio_host_gpio_map_table[SDIO_HOST_GPIO_CLK_INDEX].gpio_id, 3);
	/* sdio host cmd */
	bk_gpio_pull_up(sdio_host_gpio_map_table[SDIO_HOST_GPIO_CMD_INDEX].gpio_id);
	bk_gpio_set_capacity(sdio_host_gpio_map_table[SDIO_HOST_GPIO_CMD_INDEX].gpio_id, 3);
	/* sdio host data0 */
	bk_gpio_pull_up(sdio_host_gpio_map_table[SDIO_HOST_GPIO_DATA0_INDEX].gpio_id);
	bk_gpio_set_capacity(sdio_host_gpio_map_table[SDIO_HOST_GPIO_DATA0_INDEX].gpio_id, 3);

#if CONFIG_SDIO_4LINES_EN
	/* sdio host data1,data2,data3 */
	for (uint32_t i = SDIO_HOST_GPIO_DATA1_INDEX; i < SDIO_HOST_GPIO_PIN_NUMBER; i++) {
		bk_gpio_pull_up(sdio_host_gpio_map_table[i].gpio_id);
		bk_gpio_set_capacity(sdio_host_gpio_map_table[i].gpio_id, 3);
	}
#endif
}
```

---

## Cách Thay Đổi GPIO cho BK7258

### Phương Án 1: Đổi Nhóm GPIO (Khuyến Nghị)

Project của bạn đang dùng `CONFIG_PIN_SDIO_GROUP_0=y`:

**File**: [`projects/media/audio_play_sdcard_mp3_music/config/bk7258/config`](file:///home/do_lumi/armino-avdk/bk_avdk/projects/media/audio_play_sdcard_mp3_music/config/bk7258/config#L26)

**Hai nhóm GPIO có sẵn**:

| Nhóm | CLK | CMD | D0 | D1 | D2 | D3 |
|------|-----|-----|-----|-----|-----|-----|
| **GROUP_0** (hiện tại) | GPIO_14 | GPIO_15 | GPIO_16 | GPIO_17 | GPIO_18 | GPIO_19 |
| **GROUP_1** | GPIO_2 | GPIO_3 | GPIO_4 | GPIO_5 | GPIO_10 | GPIO_11 |

**Để chuyển sang GROUP_1**, sửa trong file config:

```diff
-CONFIG_PIN_SDIO_GROUP_0=y
+# CONFIG_PIN_SDIO_GROUP_0 is not set
```

### Phương Án 2: GPIO Tùy Chỉnh (Nâng Cao)

Nếu muốn dùng GPIO khác (ngoài 2 nhóm mặc định), sửa file:

**File**: [`bk_idk/middleware/soc/bk7258/soc/gpio_map.h`](file:///home/do_lumi/armino-avdk/bk_avdk/bk_idk/middleware/soc/bk7258/soc/gpio_map.h#L192-L212)

```c
#if (CONFIG_PIN_SDIO_GROUP_0)
#define SDIO_HOST_GPIO_MAP \
{\
	{GPIO_20, GPIO_DEV_SDIO_HOST_CLK},\     // Thay đổi ở đây
	{GPIO_21, GPIO_DEV_SDIO_HOST_CMD},\     // Thay đổi ở đây
	{GPIO_22, GPIO_DEV_SDIO_HOST_DATA0},\   // Thay đổi ở đây
	// ... tiếp tục thay đổi
}
```

> [!WARNING]
> Hãy chắc chắn các GPIO bạn chọn:
> - Hỗ trợ chức năng SDIO (xem GPIO_DEV_MAP trong gpio_map.h)
> - Không xung đột với các peripheral khác đang sử dụng

---

## Các Cấu Hình Quan Trọng

### 1. Bus Width

Kiểm tra config bus width trong project:

```c
#if CONFIG_SDIO_4LINES_EN
// 4-line mode: CLK, CMD, D0, D1, D2, D3
#else
// 1-line mode: CLK, CMD, D0
#endif
```

### 2. GPIO Requirements

- **Pull-up**: Tất cả GPIO SDIO phải có pull-up resistor
- **Drive Capacity**: Set = 3 (mạnh nhất)
- **No conflict**: Không dùng chung với chức năng khác

### 3. LDO Control (QUAN TRỌNG!)

**File**: [`gpio_map.h`](file:///home/do_lumi/armino-avdk/bk_avdk/bk_idk/middleware/soc/bk7258/soc/gpio_map.h#L457-L458)

```c
#define SDCARD_LDO_CTRL_GPIO               (GPIO_13)
#define SDCARD_LDO_CTRL_ACTIVE_LEVEL       (1)
```

> [!IMPORTANT]
> **GPIO_13** điều khiển nguồn LDO cho SD card. Nếu không cấp nguồn đúng, SD card sẽ không hoạt động!

---

## Debugging Checklist

Khi SD card không hoạt động:

### ✅ Kiểm Tra Cấu Hình

- [ ] Config đúng nhóm GPIO trong `config/bk7258/config`
- [ ] File `gpio_map.h` đúng (`bk7258`, KHÔNG phải `bk7258_cp2`)
- [ ] Bus width config: `CONFIG_SDIO_4LINES_EN` hoặc `CONFIG_SDCARD_BUSWIDTH_4LINE`

### ✅ Kiểm Tra Hardware

- [ ] **GPIO_13 (LDO control)**: Có cấp nguồn cho SD card không?
- [ ] **Pull-up resistors**: Hardware có pull-up 10kΩ-47kΩ trên tất cả data lines
- [ ] **GPIO conflicts**: Các GPIO SDIO không bị dùng cho I2C, SPI, UART, LCD...

### ✅ Kiểm T

ra Driver

- [ ] Chip đúng là **BK7258** (không phải BK7256)
- [ ] Driver đang dùng: `sdio_host_driver.c` (KHÔNG phải `sdio_driver.c`)

---

## So Sánh BK7256 vs BK7258

| Đặc điểm |  BK7256XX | BK7258 |
|----------|-----------|--------|
| **Driver file** | `sdcard/sdio_driver.c` | `sdio_host/sdio_host_driver.c` |
| **GPIO init function** | `sdio_gpio_config()` | `sdio_host_init_gpio()` |
| **GPIO mapping** | Hard-coded trong driver | Sử dụng `SDIO_HOST_GPIO_MAP` từ `gpio_map.h` |
| **Nơi sửa GPIO** | Trực tiếp trong `sdio_driver.c` | Sửa trong `gpio_map.h` |

---

## Tóm Tắt - BK7258

| Mục | File/Location |
|-----|---------------|
| **Định nghĩa GPIO** | [`soc/bk7258/soc/gpio_map.h:192-212`](file:///home/do_lumi/armino-avdk/bk_avdk/bk_idk/middleware/soc/bk7258/soc/gpio_map.h#L192-L212) |
| **Driver thực tế** | [`middleware/driver/sdio_host/sdio_host_driver.c:102`](file:///home/do_lumi/armino-avdk/bk_avdk/bk_idk/middleware/driver/sdio_host/sdio_host_driver.c#L102) |
| **Config nhóm GPIO** | [`projects/.../config/bk7258/config:26`](file:///home/do_lumi/armino-avdk/bk_avdk/projects/media/audio_play_sdcard_mp3_music/config/bk7258/config#L26) |
| **LDO control** | `GPIO_13` trong `gpio_map.h:457` |

> [!IMPORTANT]
> **Để thay đổi GPIO trên BK7258:**
> 1. **Cách đơn giản**: Đổi `CONFIG_PIN_SDIO_GROUP_0` trong file config
> 2. **Cách nâng cao**: Sửa `SDIO_HOST_GPIO_MAP` trong `bk7258/soc/gpio_map.h`
> 
> **KHÔNG được** sửa trong `sdio_driver.c` - file đó chỉ dành cho BK7256!
>CONFIG_GPIO_DEFAULT_SET_SUPPORT, đây là macro , cái này auto = y, nó sẽ lấy config default k---> Không thể sửa vì dễ gây lỗi


> #if CONFIG_GPIO_DEFAULT_SET_SUPPORT
> #if !CONFIG_USR_GPIO_CFG_EN
> /*
>  typedef struct {
> 	 uint32_t gpio_id:				 6;  //gpio_id_t

> 	 uint32_t second_func_en:		 1;  //gpio_func_mode_t
> 	 uint32_t second_func_dev:		 8;  //gpio_dev_t

> 	 uint32_t io_mode:				 2;  //gpio_io_mode_t
> 	 uint32_t pull_mode:			 2;  //gpio_pull_mode_t

>	 uint32_t int_en:				 1;  //gpio_int_mode_t
>	 uint32_t int_type: 			 2;  //gpio_int_type_t
> 
> 	 uint32_t low_power_io_ctrl:	 2;  //gpio_lowpower_mode_t

> 	 uint32_t driver_capacity:		2;	//gpio_driver_capacity_t
> }gpio_default_map_t;
> */
> #define GPIO_DEFAULT_DEV_CONFIG  \
> 
> 	{GPIO_0,  GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_I2C1_SCL, GPIO_IO_DISABLE, GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL, GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_3},\
> 	{GPIO_1,  GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_I2C1_SDA, GPIO_IO_DISABLE, GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL, GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_3},\
> 	{GPIO_2,  GPIO_SECOND_FUNC_ENABLE, GPIO_DEV_SDIO_HOST_CLK, GPIO_IO_DISABLE, GPIO_PULL_UP_EN, GPIO_INT_DISABLE, GPIO_INT_TYPE_LOW_LEVEL, GPIO_LOW_POWER_DISCARD_IO_STATUS, GPIO_DRIVER_CAPACITY_3},\


