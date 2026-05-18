# TAS5805M trong project headset

Tài liệu này mô tả cách TAS5805M hoạt động trong project headset, vai trò của I2C/I2S, thứ tự init/start, và quan hệ với hai mode test hiện có.

## Vai trò của TAS5805M

TAS5805M là digital input class-D amplifier. Trong project này chip nhận:

| Đường | Vai trò |
| --- | --- |
| I2C | cấu hình chip, power state, mute, volume, fault clear |
| I2S | nhận dữ liệu PCM audio để phát |
| PDN | bật/tắt phần cứng chip |
| ADR | chọn địa chỉ I2C |

Vì vậy khác với module như MAX98357, TAS5805M không chỉ cần I2S. Chip phải được bật bằng `PDN`, trả lời I2C đúng địa chỉ, được đưa vào trạng thái `PLAY`, rồi mới phát audio từ I2S.

## Chân phần cứng đang dùng

Cấu hình nằm trong `main/armino_main.c`:

| Tín hiệu | GPIO |
| --- | --- |
| I2C SDA | `GPIO_1` |
| I2C SCL | `GPIO_0` |
| PDN | `GPIO_12` |
| ADR | `GPIO_28` |
| ADR level | low |
| I2C address | `0x2C` |

Với `ADR` kéo low, code dùng địa chỉ mặc định:

```c
#define TAS5805M_I2C_ADDRESS_DEFAULT 0x2C
```

## Driver đang làm gì

Driver nằm ở:

```text
main/tas_5805.c
main/tas_5805.h
```

Driver hiện dùng bit-bang I2C bằng GPIO, không dùng hardware I2C peripheral. Lý do thực tế là dễ kiểm soát chân SDA/SCL, dễ scan bus, và không phụ thuộc thêm cấu hình I2C controller của SDK.

Các API chính:

| API | Vai trò |
| --- | --- |
| `tas5805m_init_default_config()` | tạo cấu hình mặc định |
| `tas5805m_init()` | setup GPIO, PDN, ADR, scan I2C, đưa chip vào Hi-Z/mute |
| `tas5805m_start(sample_rate)` | chờ clock I2S ổn định, gửi cấu hình tối thiểu, clear fault, chuyển PLAY |
| `tas5805m_stop()` | đưa chip về Hi-Z + mute |
| `tas5805m_set_mute()` | bật/tắt mute |
| `tas5805m_set_volume()` | ghi volume digital L/R |
| `tas5805m_dump_status()` | đọc các thanh ghi monitor/fault để debug |

## Thứ tự hoạt động chuẩn

Luồng chuẩn của chip:

```text
Set ADR pin
  -> PDN low
  -> delay
  -> PDN high
  -> delay
  -> setup SDA/SCL
  -> scan/probe I2C address 0x2C
  -> select page/book 0
  -> DEVICE_CTRL_2 = Hi-Z + mute
  -> chờ I2S start
  -> gửi cấu hình tối thiểu
  -> clear fault
  -> DEVICE_CTRL_2 = PLAY
```

Điểm quan trọng: `tas5805m_init()` chưa phát âm thanh. Nó chỉ làm chip sống và sẵn sàng. Audio chỉ phát sau khi `tas5805m_start()` được gọi.

## Vì sao `tas5805m_start()` gọi sau I2S start

TAS5805M cần thấy clock I2S hợp lệ như BCLK/LRCLK trước khi vào PLAY ổn định. Vì vậy project đang gọi theo thứ tự trong `audio_play.c`:

```text
bk_i2s_driver_init()
bk_i2s_init()
bk_i2s_chl_init()
bk_i2s_set_samp_rate()
bk_i2s_start()
tas5805m_start(sample_rate)
```

Trong `tas5805m_start()` có delay ngắn để clock ổn định rồi mới chuyển chip sang `PLAY`.

Nếu gọi TAS5805M PLAY trước khi I2S clock chạy, chip có thể báo clock fault hoặc không phát.

## Cấu hình I2S hiện tại

I2S output nằm trong `main/audio_play.c`:

| Tham số | Giá trị |
| --- | --- |
| Role | master |
| Work mode | I2S |
| Channel | TX |
| Sample rate | theo input, ví dụ 44100 Hz |
| Data length | 16-bit |
| Store mode | `I2S_LRCOM_STORE_16R16L` |
| Ring buffer | TX ring buffer của I2S channel 1 |

Luồng ghi PCM:

```text
audio_play_pcm_i2s_start()
  -> start I2S
  -> tas5805m_start()

audio_play_pcm_i2s_write()
  -> apply gain phần mềm
  -> ghi PCM vào I2S TX ring buffer

I2S driver
  -> phát PCM ra bus I2S

TAS5805M
  -> nhận PCM I2S
  -> khuếch đại ra loa
```

## Hai mode đang dùng TAS5805M

### TEST_BLUETOOTH_I2S

Luồng tổng quát:

```text
Điện thoại
  -> Bluetooth A2DP
  -> decode thành PCM
  -> audio_play_pcm_i2s_write()
  -> I2S
  -> TAS5805M
  -> loa
```

Mode này giữ Bluetooth stack và A2DP sink demo hoạt động.

### DMIC_AND_I2S

Luồng tổng quát:

```text
DMIC
  -> DMA vào RAM
  -> mono 16-bit thành stereo 16-bit
  -> audio_play_pcm_i2s_write()
  -> I2S
  -> TAS5805M
  -> loa
```

Mode này tắt Bluetooth demo để chỉ test đường mic -> loa.

## Các thanh ghi chính

Một số register driver đang dùng:

| Register | Vai trò |
| --- | --- |
| `PAGE` / `BOOK` | chọn vùng register |
| `DEVICE_CTRL_2` | deep sleep/sleep/Hi-Z/play/mute |
| `RESET_CTRL` | reset logic |
| `SAP_CTRL_1` | cấu hình serial audio port nếu cần mở rộng |
| `FS_MON` | monitor sample rate |
| `BCK_MON` | monitor BCLK |
| `CLKDET_STATUS` | trạng thái clock detect |
| `DIG_VOL_LEFT/RIGHT` | volume digital |
| `AGAIN` | analog gain |
| `FAULT` | clear/read fault |
| `CHAN_FAULT`, `GLOBAL_FAULT1/2` | fault chi tiết |

`report_error_task()` đang gọi `tas5805m_dump_status()` định kỳ để xem các register monitor/fault.

## Debug nhanh

Nếu không có âm thanh:

1. Kiểm tra log scan I2C có thấy device `0x2C`.
2. Kiểm tra `PDN GPIO_12` đã lên high sau init.
3. Kiểm tra `ADR GPIO_28` đúng level low.
4. Kiểm tra I2S đã start trước `tas5805m_start()`.
5. Kiểm tra `CLKDET_STATUS`, `FS_MON`, `BCK_MON` trong log dump.
6. Kiểm tra `FAULT`, `CHAN_FAULT`, `GLOBAL_FAULT1/2`.

Nếu Bluetooth phát được nhưng DMIC không phát:

- TAS5805M/I2S cơ bản đã đúng.
- Tập trung kiểm tra DMIC `GPIO_8/GPIO_9`, nguồn mic, DMA byte counter.

Nếu DMIC log có `bytes in` tăng nhưng loa im:

- Kiểm tra `audio_play_pcm_i2s_write()` có timeout/drop không.
- Kiểm tra TAS5805M status/fault.
- Kiểm tra gain trong `audio_play.c`.

Nếu âm thanh rè/vỡ:

- Giảm `gain` trong `audio_play.c`.
- Kiểm tra nguồn TAS5805M và loa.
- Kiểm tra sample rate source và I2S có cùng 44100 Hz.

## Ghi chú triển khai

- `tas5805m_init()` chạy một lần lúc boot.
- `tas5805m_start()` chạy mỗi khi I2S playback bắt đầu.
- `tas5805m_stop()` chạy khi playback dừng.
- Không nên chuyển `PLAY` trước khi I2S clock ổn định.
- Nếu sau này dùng file cấu hình PPC3 đầy đủ, có thể đưa sequence vào `tas5805m_send_cfg()` bằng format register/value hoặc burst/delay đã hỗ trợ trong driver.
