# DMIC -> I2S Debug Notes

## Bối cảnh

Phần cứng DMIC đã được xác nhận hoạt động tốt bằng CLI gốc:

```c
aud_dmic_dma_test start 44100
```

CLI này cho chất lượng âm thanh tốt khi phát ra loa analog vì đường dữ liệu rất đơn giản:

```text
DMIC FIFO -> DMA -> DAC FIFO
```

Khi chuyển sang project `bluetooth/headset`, đầu ra là TAS5805M qua I2S, nên đường dữ liệu ban đầu khác nhiều:

```text
DMIC FIFO -> CPU polling/DMA buffer -> filter/gain/channel select -> I2S ringbuffer -> TAS5805M
```

Sự khác biệt này là nguyên nhân chính làm debug bị lệch hướng lúc đầu.

## Triệu chứng đã gặp

- DMIC FIFO có data nhưng loa phát tiếng rất nhỏ, rè hoặc méo.
- Có lúc tiếng bị đứt đoạn, nghe như "cộc cộc".
- Có lúc tiếng người bị kéo trầm nặng, nghe "ồ ồ", không rõ lời.
- Direct DMA thử nghiệm `DMIC FIFO -> I2S TX register` không ra tiếng, log chỉ thấy FIFO empty.
- Các log stats có lúc cho thấy raw data thay đổi nhưng output không tương ứng với tiếng người.

## Các nguyên nhân đã loại trừ

### 1. Phần cứng DMIC không hỏng

DMIC cùng phần cứng đã chạy tốt với CLI `aud_dmic_dma_test`, nên lỗi không nằm ở clock/data/power của mic.

### 2. TAS5805M và I2S output không hỏng

Mode Bluetooth/I2S và test TAS5805M đã phát được âm thanh, nên TAS5805M, I2C init và I2S output cơ bản đều hoạt động.

### 3. Filter/gain không phải hướng fix chính

Đã thử nhiều cấu hình:

- chọn L/R channel khác nhau
- trừ DC offset
- high-pass filter
- noise gate
- tăng/giảm gain
- mono duplicate sang L/R

Các chỉnh này có thể thay đổi âm lượng, nhưng không xử lý được lỗi gốc là luồng dữ liệu sang I2S không được pace đúng.

## Lỗi chính

### 1. Pipeline ban đầu xử lý audio quá nhiều

Code ban đầu đọc DMIC FIFO bằng CPU, gom block, xử lý sample rồi ghi vào I2S ringbuffer. Pipeline này phức tạp hơn CLI tốt rất nhiều.

Khi audio còn chưa đúng format/timing, việc thêm filter, gain, DC remove và chọn kênh làm khó xác định lỗi thật.

Fix: quay về raw passthrough trước.

```text
DMIC FIFO -> CPU read raw word -> I2S write raw word -> TAS5805M
```

### 2. Bỏ `bk_aud_dac_init()` / `bk_aud_dac_start()` làm DMIC không sinh FIFO ổn định

CLI tốt luôn init và start cả DAC:

```c
bk_aud_dac_init(&dac_config);
bk_aud_dmic_init(&dmic_config);
bk_aud_dmic_start();
bk_aud_dac_start();
```

Khi thử direct I2S mà bỏ DAC init/start, log cho thấy:

```text
fifo_status=0x00A0F000
received_words=0
raw stats: empty
out stats: empty
```

Điều này cho thấy audio block của BK7258 cần DAC path được init/start để clock/audio engine hoạt động đúng cho DMIC test này, dù output thực tế đang phát qua I2S/TAS.

Fix: trong mode DMIC/I2S vẫn giữ:

```c
bk_aud_dac_init(&dac_config);
bk_aud_dac_start();
```

### 3. `bk_i2s_write_data()` không tự chờ TX FIFO ready

Hàm `bk_i2s_write_data()` trong SDK chỉ ghi thẳng register:

```c
for (i = 0; i < data_len; i++)
    i2s_hal_data_write(channel_id, data_buf[i]);
```

Nó không chờ TX FIFO sẵn sàng.

Vì vậy nếu loop DMIC đọc FIFO rồi gọi `bk_i2s_write_data()` liên tục, tốc độ ghi không được pace theo I2S. Kết quả là mẫu audio bị overwrite/drop/jitter, làm tiếng người bị trầm, méo hoặc không rõ.

Fix: trước mỗi lần ghi I2S direct, phải chờ:

```c
bk_i2s_get_write_ready(&write_ready);
```

Chỉ khi `write_ready != 0` mới gọi:

```c
bk_i2s_write_data(I2S_CHANNEL_1, &data, 1);
```

Đây là fix quan trọng nhất giúp âm thanh trong và rõ hơn.

## Cách khắc phục cuối cùng

Mode DMIC hiện tại nên giữ theo hướng raw/direct để tránh xử lý thừa:

```text
1. Start I2S direct output to TAS5805M
2. Init DAC giống CLI gốc
3. Init DMIC
4. Start DMIC
5. Start DAC
6. Poll DMIC FIFO
7. Với mỗi raw 32-bit word:
   - chờ I2S TX FIFO ready
   - ghi nguyên word sang I2S
```

Không filter, không gain, không chọn lại kênh trong bước test ổn định đầu tiên.

## Log kỳ vọng

Sau khi chạy đúng, log sẽ có dạng:

```text
AUD_PLAY:W(...):direct I2S stream started, rate=44100
DMIC_I2S WARN: DMIC direct CPU->I2S running ...
DMIC_I2S WARN: DMIC direct CPU->I2S words=... dropped=0 ...
```

Không nên thấy `received_words=0` kéo dài.

Nếu có log empty, nó chỉ nên xuất hiện ngắn hoặc không tăng liên tục bất thường.

## Bài học rút ra

- Khi CLI chuẩn đã chạy tốt, cần copy topology của CLI trước, chưa vội thêm filter/gain.
- Với BK7258 audio block, DMIC test tốt cần giữ DAC init/start giống CLI, kể cả khi output chính là I2S.
- Ghi I2S bằng CPU phải chờ TX FIFO ready. Không được giả định `bk_i2s_write_data()` tự block hoặc tự pace.
- Chỉ thêm gain/filter sau khi raw passthrough đã nghe rõ tiếng người.
