# DMIC trong project headset

Tài liệu này mô tả cơ chế test `DMIC_AND_I2S`: nói vào digital mic, dữ liệu mic được lấy bằng DMA, đổi sang PCM stereo 16-bit, rồi đẩy ra I2S để TAS5805M phát ra loa.

## Chọn mode

Mode được chọn trong `main/armino_main.c`:

```c
#define HEADSET_MODE_TEST_BLUETOOTH_I2S 1
#define HEADSET_MODE_DMIC_AND_I2S 2

#ifndef HEADSET_APP_MODE
#define HEADSET_APP_MODE HEADSET_MODE_DMIC_AND_I2S
#endif
```

Khi chọn `HEADSET_MODE_DMIC_AND_I2S`, app sẽ không khởi động Bluetooth demo. Luồng main là:

1. `bk_init()`
2. `media_service_init()`
3. `tas5805m_demo_init()`
4. `digital_mic_start()`

TAS5805M được init trước để bảo đảm amp đã sẵn sàng trước khi I2S bắt đầu chạy.

## Chân DMIC

Driver DMIC của BK7258 đang map cố định:

| Tín hiệu | GPIO |
| --- | --- |
| DMIC CLK | `GPIO_8` |
| DMIC DATA | `GPIO_9` |

Phần map này nằm trong driver SDK: `bk_idk/middleware/driver/audio/aud/aud_dmic_driver.c`.

## Cấu hình âm thanh

Code đang dùng cấu hình mặc định trong `main/digital_mic.h`:

| Tham số | Giá trị |
| --- | --- |
| Sample rate | `44100 Hz` |
| DMIC channel | `AUD_DMIC_CHL_L` |
| Frame duration | `20 ms` |
| Frame count | `4` |
| PCM input | mono, 16-bit |
| PCM output ra I2S | stereo, 16-bit |

Với 44.1 kHz và frame 20 ms:

```text
44100 sample/s * 20 ms * 2 byte = 1764 byte mono / frame
1764 byte mono -> 3528 byte stereo / frame
```

## Pipeline dữ liệu

Luồng dữ liệu thực tế:

```text
DMIC hardware
  -> DMIC FIFO
  -> DMA copy vào RAM buffer
  -> DMA finish ISR báo có frame mới
  -> digital_mic_task lấy frame
  -> mono 16-bit duplicate thành stereo 16-bit
  -> audio_play_pcm_i2s_write()
  -> I2S TX ring buffer
  -> BK I2S DMA phát ra chân I2S
  -> TAS5805M nhận I2S và kéo loa
```

Trong đó:

- DMIC capture dùng driver `bk_aud_dmic_*`.
- DMA lấy dữ liệu từ địa chỉ FIFO trả về bởi `bk_aud_dmic_get_fifo_addr()`.
- Task `digital_mic_task()` không đọc FIFO trực tiếp; nó chỉ xử lý buffer sau khi DMA báo xong frame.
- I2S output dùng lại module `audio_play.c`, giống luồng phát Bluetooth sau khi đã có PCM.

## DMA DMIC

`digital_mic_dma_init()` cấu hình DMA theo hướng:

```text
source:      DMIC FIFO
destination: RAM buffer
mode:        DMA_WORK_MODE_REPEAT
transfer:    1 frame mỗi lần interrupt
```

DMA chạy vòng trên buffer nhiều frame:

```text
frame 0 -> frame 1 -> frame 2 -> frame 3 -> frame 0 ...
```

Mỗi lần DMA hoàn thành một frame, ISR `digital_mic_dma_finish_isr()` sẽ:

1. Cộng `received_bytes`.
2. Tăng index frame DMA.
3. Tăng số frame sẵn sàng cho task xử lý.
4. Nếu task xử lý không kịp, tăng `dropped_frames`.
5. Set semaphore để đánh thức `digital_mic_task()`.

ISR chỉ làm việc nhẹ, không convert audio và không ghi I2S trong interrupt.

## Convert mono sang stereo

DMIC hiện lấy 1 channel. TAS5805M/I2S output đang chạy stereo 16-bit, nên mỗi sample mono được nhân đôi:

```text
mono:   M0, M1, M2, M3
stereo: L0=M0, R0=M0, L1=M1, R1=M1, ...
```

Việc này nằm ở `digital_mic_mono_to_stereo()`.

## Đẩy ra I2S

Sau khi convert, task gọi:

```c
audio_play_pcm_i2s_write(stereo_buffer, stereo_len, timeout_ms);
```

`audio_play_pcm_i2s_write()` sẽ ghi PCM vào ring buffer TX của I2S. Driver I2S tự lấy dữ liệu từ ring buffer để phát ra bus I2S.

Lưu ý: `audio_play.c` đang apply gain phần mềm trước khi ghi ra I2S:

```c
float gain = 0.25f;
```

Nếu âm lượng DMIC quá nhỏ hoặc quá lớn, chỉnh gain ở đây là điểm nhanh nhất để thử.

## Log runtime

Module DMIC dùng `os_printf`, ví dụ:

```text
DMIC_I2S: DMIC DMA ready, fifo=..., buffer=..., frame=1764 frames=4
DMIC_I2S: DMIC->I2S running: DMIC GPIO_8 CLK, GPIO_9 DAT, sample_rate=44100
DMIC_I2S: DMIC bytes in=..., pushed I2S=3528, frame=..., ready=..., dropped=...
```

Ý nghĩa:

| Log | Ý nghĩa |
| --- | --- |
| `bytes in` | Tổng byte DMA đã nhận từ DMIC |
| `pushed I2S` | Số byte stereo vừa đẩy vào I2S |
| `ready` | Số frame còn chờ task xử lý |
| `dropped` | Frame bị bỏ do task/I2S không xử lý kịp |

Nếu `bytes in` tăng nhưng không có âm thanh, kiểm tra I2S/TAS5805M. Nếu `bytes in` không tăng, kiểm tra DMIC clock/data, nguồn mic, hoặc driver DMIC.

## Thứ tự start/stop

Start:

```text
audio_play_pcm_i2s_start()
  -> I2S init/start
  -> tas5805m_start()
bk_aud_dmic_init()
digital_mic_dma_init()
bk_dma_start()
bk_aud_dmic_start()
```

Stop:

```text
bk_aud_dmic_stop()
bk_dma_stop()
bk_dma_deinit/free()
bk_aud_dmic_deinit()
audio_play_pcm_i2s_stop()
  -> tas5805m_stop()
  -> I2S stop/deinit
```

## Các điểm cần nhớ khi test

- DMIC dùng `GPIO_8/GPIO_9`, không phải GPIO tự chọn trong app.
- Mode DMIC hiện disable Bluetooth demo để tránh tranh I2S/audio path.
- TAS5805M vẫn cần I2C init trước, sau đó mới nhận I2S.
- Sample rate 44.1 kHz được cả DMIC driver và I2S path hỗ trợ.
- Nếu nghe rè/vỡ, giảm `gain` trong `audio_play.c`.
- Nếu âm lượng nhỏ, tăng `gain` từ từ, ví dụ `0.35f`, `0.5f`.
