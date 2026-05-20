# Khắc Phục Tiếng Xẹt DMIC to Speaker - BK7258

Tài liệu phân tích và khắc phục vấn đề tiếng xẹt khi phát âm thanh từ DMIC ra loa.

## 🎯 Vấn Đề Hiện Tại

**Triệu chứng:** Nghe thấy tiếng xẹt, xẹt khi phát âm thanh từ DMIC → DAC → Loa. Tiếng xẹt nhiều hơn khi nói.

**Nguyên nhân chính được phát hiện trong code:**

### 1. ❌ Polling Không Hiệu Quả (CRITICAL)

```c
// app_main.c dòng 89-102
while (1) {
    if (get_fifo(&d) == BK_OK) {
        // os_printf("dmic=%08X\n", d);
    } else {
        os_printf("empty\n");  // ← Print này gây blocking
        rtos_delay_milliseconds(10);  // ← Delay 10ms là quá lâu!
    }
}
```

**Vấn đề:**
- **Delay 10ms quá lớn:** DMIC chạy 16kHz → mỗi 10ms có 160 samples, FIFO sẽ bị overflow
- **Polling từ main loop:** Không ổn định, dễ bị gián đoạn bởi task khác
- **Print "empty":** Blocking I/O làm mất thời gian

### 2. ❌ Không Có Buffer Trung Gian

```c
// audio_record.c dòng 153-156
if (dmic_status & (AUD_DMIC_NEAR_FULL_MASK | AUD_DMIC_FIFO_FULL_MASK)) {
    bk_aud_dmic_get_fifo_data(d);
    os_printf("DMIC FIFO data: 0x%08X\n", *d);  // ← Blocking print
    bk_aud_dac_write(*d);  // ← Ghi trực tiếp không có buffer
}
```

**Vấn đề:**
- Dữ liệu từ DMIC được ghi **trực tiếp** vào DAC không qua buffer
- Nếu DAC chưa sẵn sàng → data bị drop → tiếng xẹt
- `os_printf` làm chậm quá trình xử lý

### 3. ❌ Sample Rate Có Thể Không Khớp

```c
// app_main.c dòng 82
audio_record_to_sdcard_start("test.wav", 16000);  // 16kHz

// audio_record.c dòng 178, 192
dac_cfg.samp_rate = samp_rate;  // DAC: 16kHz
dmic_cfg.samp_rate = samp_rate; // DMIC: 16kHz
```

**Tiềm ẩn vấn đề:**
- Clock source của DMIC và DAC có thể không đồng bộ
- Drift tích lũy theo thời gian → underrun/overrun

### 4. ❌ Thiếu Xử Lý FIFO Threshold

```c
bk_aud_dmic_set_dmic_wr_threshold(8);  // Threshold = 8
```

**Vấn đề:**
- Threshold = 8 mẫu là quá ít
- Polling mỗi 10ms sẽ để FIFO tràn

### 5. ⚠️ Không Sử Dụng Interrupt

```c
// Interrupt đã bị comment out
// bk_aud_dmic_register_isr(audio_dmic_isr);
// bk_aud_dmic_enable_int();
```

**Hậu quả:**
- Mất đi xử lý real-time
- Data không được đọc kịp thời

---

## ✅ Giải Pháp Chi Tiết

### Giải Pháp 1: Sử Dụng Interrupt + Ring Buffer (RECOMMENDED)

#### Bước 1: Tạo Ring Buffer

```c
// Thêm vào đầu audio_record.c
#include <driver/audio_ring_buff.h>

#define DMIC_TO_DAC_BUFFER_SIZE (16384)  // 16KB buffer
static RingBufferContext *dmic_to_dac_rb = NULL;
static bool enable_playback = false;
```

#### Bước 2: DMIC ISR - Đọc và Lưu vào Buffer

```c
static void audio_dmic_isr(void)
{
    uint32_t dmic_data;
    uint32_t dmic_status = 0;
    
    bk_aud_dmic_get_status(&dmic_status);
    
    // Đọc nhiều samples cùng lúc để tối ưu
    while (dmic_status & (AUD_DMIC_NEAR_FULL_MASK | AUD_DMIC_FIFO_FULL_MASK)) {
        if (bk_aud_dmic_get_fifo_data(&dmic_data) == BK_OK) {
        // Ghi vào ring buffer thay vì trực tiếp vào DAC
            uint32_t written = ring_buffer_write(dmic_to_dac_rb, 
                                                  (uint8_t*)&dmic_data, 
                                                  sizeof(dmic_data));
            if (written != sizeof(dmic_data)) {
                // Buffer full - có thể log warning (không nên print trong ISR)
                break;
            }
        }
        
        bk_aud_dmic_get_status(&dmic_status);
    }
}
```

#### Bước 3: DAC Playback Thread

```c
static beken_thread_t dac_playback_thread = NULL;
static bool playback_thread_running = false;

static void dac_playback_task(void *arg)
{
    uint32_t dac_data;
    uint32_t available;
    
    os_printf("DAC playback thread started\n");
    
    while (playback_thread_running && enable_playback) {
        available = ring_buffer_get_fill_size(dmic_to_dac_rb);
        
        // Chỉ phát khi có đủ data (hysteresis để tránh xẹt)
        if (available >= sizeof(dac_data) * 4) {  // Tối thiểu 4 samples
            if (ring_buffer_read(dmic_to_dac_rb, 
                                (uint8_t*)&dac_data, 
                                sizeof(dac_data)) == sizeof(dac_data)) {
                bk_aud_dac_write(dac_data);
            }
        } else {
            // Buffer underrun - chờ thêm data
            rtos_delay_milliseconds(1);
        }
    }
    
    os_printf("DAC playback thread exiting\n");
    playback_thread_running = false;
    rtos_delete_thread(NULL);
}
```

#### Bước 4: Khởi Tạo Đầy Đủ

```c
bk_err_t audio_record_to_sdcard_start(char *file_name, uint32_t samp_rate)
{
    bk_err_t ret = BK_OK;

    // 1. Khởi tạo Ring Buffer
    ret = ring_buffer_init(&dmic_to_dac_rb, DMIC_TO_DAC_BUFFER_SIZE, 
                          RB_TYPE_NORMAL);
    if (ret != BK_OK) {
        os_printf("Ring buffer init failed\n");
        goto fail;
    }

    // 2. Khởi tạo DAC
    aud_dac_config_t dac_cfg = DEFAULT_AUD_DAC_CONFIG();
    dac_cfg.samp_rate = samp_rate;
    dac_cfg.dac_chl = AUD_DAC_CHL_LR;
    dac_cfg.work_mode = AUD_DAC_WORK_MODE_DIFFEN;  // Differential mode
    dac_cfg.dac_gain = 0x2D;  // Giảm gain để tránh distortion
    
    ret = bk_aud_dac_init(&dac_cfg);
    if (ret != BK_OK) {
        os_printf("DAC init failed: %d\n", ret);
        goto fail;
    }

    // 3. Khởi tạo DMIC
    aud_dmic_config_t dmic_cfg = DEFAULT_AUD_DMIC_CONFIG();
    dmic_cfg.samp_rate = samp_rate;
    dmic_cfg.dmic_chl = AUD_DMIC_CHL_LR;
    dmic_cfg.clk_invert = true;  // Thử invert clock để cải thiện
    
    ret = bk_aud_dmic_init(&dmic_cfg);
    if (ret != BK_OK) {
        os_printf("DMIC init failed: %d\n", ret);
        goto fail;
    }

    // 4. Đăng ký ISR
    ret = bk_aud_dmic_register_isr(audio_dmic_isr);
    if (ret != BK_OK) {
        os_printf("Register DMIC ISR failed: %d\n", ret);
        goto fail;
    }

    // 5. Set threshold cao hơn để giảm interrupt frequency
    bk_aud_dmic_set_dmic_wr_threshold(128);  // Tăng từ 8 → 128
    
    // 6. Enable interrupt
    bk_aud_dmic_enable_int();

    // 7. Start hardware
    bk_aud_dac_start();
    bk_aud_dmic_start();
    
    // 8. Tạo playback thread
    playback_thread_running = true;
    enable_playback = true;
    
    ret = rtos_create_thread(&dac_playback_thread,
                            BEKEN_DEFAULT_WORKER_PRIORITY,
                            "dac_playback",
                            (beken_thread_function_t)dac_playback_task,
                            4096,
                            NULL);
    if (ret != BK_OK) {
        os_printf("Create playback thread failed: %d\n", ret);
        goto fail;
    }

    os_printf("DMIC to Speaker started at %u Hz\n", samp_rate);
    speaker_pa_enable();

    return BK_OK;

fail:
    // Cleanup
    if (dmic_to_dac_rb) {
        ring_buffer_clear(dmic_to_dac_rb);
        dmic_to_dac_rb = NULL;
    }
    bk_aud_dmic_stop();
    bk_aud_dmic_deinit();
    bk_aud_dac_stop();
    bk_aud_dac_deinit();
    
    return BK_FAIL;
}
```

#### Bước 5: Sửa Main Loop

```c
// Trong app_main.c, xóa polling loop
int main(void)
{
    bk_init();
    media_service_init();

#if (CONFIG_SYS_CPU0)
    if (BK_OK != audio_record_to_sdcard_start("test.wav", 16000)) {
        os_printf("start audio record to sdcard fail\n");
    } else {
        os_printf("start audio record to sdcard ok\n");
    }

    os_printf("%s: DMIC to Speaker running!\n", __func__);
    
    // KHÔNG CẦN polling loop nữa - ISR và thread sẽ xử lý
    // Chỉ cần giữ main task alive
    while (1) {
        rtos_delay_milliseconds(1000);
    }
#endif
    return 0;
}
```

---

### Giải Pháp 2: Tối Ưu Sample Rate và Clock

#### Sample Rate Recommendations

| Application | Recommended Sample Rate | Lý Do |
|------------|-------------------------|-------|
| Voice/Speech | **8kHz hoặc 16kHz** | Tiết kiệm CPU, đủ cho giọng nói |
| Music/Audio | **44.1kHz hoặc 48kHz** | Chất lượng cao |
| Testing | **16kHz** | Cân bằng giữa chất lượng và hiệu năng |

**⚠️ Lưu ý quan trọng:**
- DMIC và DAC **PHẢI** cùng sample rate
- Tránh dùng sample rate lẻ (vd: 12kHz, 24kHz) nếu không cần thiết

#### Kiểm Tra Clock Configuration

```c
// Thêm vào DMIC config
dmic_cfg.clk_invert = true;  // Thử cả true và false
```

**Giải thích:**
- `clk_invert = true`: Lấy mẫu ở cạnh xuống của clock
- `clk_invert = false`: Lấy mẫu ở cạnh lên của clock
- Tùy DMIC module, có thể cần invert hoặc không

---

### Giải Pháp 3: Điều Chỉnh DAC Gain

```c
dac_cfg.dac_gain = 0x2D;  // Giá trị 0x00 - 0x3F
```

**Gain quá cao → distortion → tiếng xẹt**

Thử các giá trị:
- `0x2D` (45/63) ≈ 71% - Khởi điểm tốt
- `0x20` (32/63) ≈ 51% - Nếu vẫn xẹt
- `0x15` (21/63) ≈ 33% - Để test

---

### Giải Pháp 4: FIFO Threshold Optimization

```c
// QUAN TRỌNG: Threshold phải phù hợp với tốc độ xử lý
bk_aud_dmic_set_dmic_wr_threshold(128);  // 128 samples
```

**Công thức tính:**
```
Threshold = Sample_Rate / Interrupt_Frequency

Ví dụ: 16kHz / 125Hz = 128 samples
→ Interrupt mỗi 8ms
```

**Khuyến nghị:**
- **8kHz:** Threshold = 64
- **16kHz:** Threshold = 128
- **44.1kHz:** Threshold = 256

---

## 🔍 Checklist Troubleshooting

Khi gặp tiếng xẹt, kiểm tra theo thứ tự:

### Bước 1: Kiểm Tra Phần Cứng
- ☐ Mic có được cấp nguồn đúng? (thường 3.3V)
- ☐ Kết nối CLK và DATA có chắc chắn?
- ☐ Loa/PA có bật? (GPIO13 = HIGH)
- ☐ GND chung giữa board và mic

### Bước 2: Kiểm Tra Sample Rate
- ☐ DMIC và DAC cùng sample rate
- ☐ Thử 16kHz trước, sau đó 8kHz hoặc 48kHz
- ☐ Log để verify sample rate thực tế

### Bước 3: Kiểm Tra Buffer
- ☐ Ring buffer đủ lớn (≥16KB)
- ☐ Hysteresis: chỉ phát khi buffer có ≥4 samples
- ☐ Monitor buffer fill level

### Bước 4: Kiểm Tra Gain
- ☐ Giảm DAC gain xuống 0x2D hoặc thấp hơn
- ☐ Nếu vẫn xẹt → giảm tiếp xuống 0x20

### Bước 5: Kiểm Tra Timing
- ☐ Không có delay trong ISR
- ☐ Không có print trong ISR hoặc critical path
- ☐ Playback thread priority đủ cao

### Bước 6: Test Clock Invert
```c
// Thử cả 2 mode
dmic_cfg.clk_invert = false;  // Test 1
dmic_cfg.clk_invert = true;   // Test 2
```

---

## 📊 Configuration Chuẩn Khuyến Nghị

### Configuration Tốt Nhất cho Voice (16kHz)

```c
// DMIC Configuration
aud_dmic_config_t dmic_cfg = DEFAULT_AUD_DMIC_CONFIG();
dmic_cfg.samp_rate = 16000;
dmic_cfg.dmic_chl = AUD_DMIC_CHL_LR;
dmic_cfg.clk_invert = true;  // Thử cả true/false

// DAC Configuration  
aud_dac_config_t dac_cfg = DEFAULT_AUD_DAC_CONFIG();
dac_cfg.samp_rate = 16000;
dac_cfg.dac_chl = AUD_DAC_CHL_LR;
dac_cfg.work_mode = AUD_DAC_WORK_MODE_DIFFEN;
dac_cfg.dac_gain = 0x2D;  // 71% volume

// FIFO Threshold
bk_aud_dmic_set_dmic_wr_threshold(128);

// Ring Buffer
#define DMIC_TO_DAC_BUFFER_SIZE 16384  // 16KB
```

### So Sánh Cấu Hình

| Parameter | Code Hiện Tại (Xẹt) | Config Khuyến Nghị | Lý Do |
|-----------|----------------------|-------------------|-------|
| **Polling Loop** | 10ms delay | ISR + Thread | Real-time, không mất data |
| **Buffer** | Không có | Ring Buffer 16KB | Chống underrun/overrun |
| **Threshold** | 8 samples | 128 samples | Giảm interrupt overhead |
| **DAC Gain** | Default | 0x2D (71%) | Tránh distortion |
| **ISR** | Disabled | Enabled | Xử lý kịp thời |
| **Print** | Trong critical path | Chỉ log error | Tránh blocking |

---

## 🐛 Common Mistakes và Cách Khắc Phục

### Mistake 1: Print trong ISR hoặc Critical Path
```c
// ❌ SAI
static void audio_dmic_isr(void) {
    uint32_t dmic_data;
    bk_aud_dmic_get_fifo_data(&dmic_data);
    os_printf("DMIC: 0x%08X\n", dmic_data);  // ← BLOCKING!
    bk_aud_dac_write(dmic_data);
}

// ✅ ĐÚNG
static void audio_dmic_isr(void) {
    uint32_t dmic_data;
    while (bk_aud_dmic_get_fifo_data(&dmic_data) == BK_OK) {
        ring_buffer_write(rb, (uint8_t*)&dmic_data, sizeof(dmic_data));
    }
    // Không print trong ISR!
}
```

### Mistake 2: Delay Quá Lớn
```c
// ❌ SAI
while (1) {
    if (get_fifo(&d) == BK_OK) {
        // process
    } else {
        rtos_delay_milliseconds(10);  // ← 10ms quá lâu!
    }
}

// ✅ ĐÚNG - Dùng ISR, không cần polling
```

### Mistake 3: Không Kiểm Tra Buffer Status
```c
// ❌ SAI
bk_aud_dac_write(dmic_data);  // Ghi trực tiếp, không check DAC ready

// ✅ ĐÚNG
if (ring_buffer_get_fill_size(rb) >= threshold) {
    ring_buffer_read(rb, (uint8_t*)&dac_data, sizeof(dac_data));
    bk_aud_dac_write(dac_data);
}
```

---

## 📈 Monitoring và Debug

### Log Buffer Fill Level

```c
// Trong playback thread, thêm monitoring
static uint32_t log_counter = 0;

while (playback_thread_running) {
    available = ring_buffer_get_fill_size(dmic_to_dac_rb);
    
    // Log mỗi 100 lần
    if (++log_counter % 100 == 0) {
        os_printf("Buffer fill: %u / %u bytes\n", 
                  available, DMIC_TO_DAC_BUFFER_SIZE);
    }
    
    // ... rest of playback logic
}
```

### Detect Underrun/Overrun

```c
static uint32_t underrun_count = 0;
static uint32_t overrun_count = 0;

// Trong ISR
if (ring_buffer_write(...) != sizeof(dmic_data)) {
    overrun_count++;  // Buffer full
}

// Trong playback thread
if (available < sizeof(dac_data)) {
    underrun_count++;  // Buffer empty
}

// Log định kỳ
os_printf("Underruns: %u, Overruns: %u\n", underrun_count, overrun_count);
```

---

## 🎯 Test Plan

### Test 1: Verify No Crackling
1. Flash firmware với configuration khuyến nghị
2. Chạy `audio_record_to_sdcard start test.wav 16000`
3. Nói vào mic, nghe trên loa
4. **Expected:** Không có tiếng xẹt, âm thanh rõ ràng

### Test 2: Buffer Stability
1. Monitor log buffer fill level
2. **Expected:** Fill level ổn định trong khoảng 4KB - 12KB
3. Nếu fill = 0 → underrun
4. Nếu fill = 16KB → overrun

### Test 3: Different Sample Rates
Thử các sample rate:
- 8kHz (voice)
- 16kHz (recommended)
- 44.1kHz (high quality)

### Test 4: Gain Adjustment
1. Start với gain = 0x2D
2. Nếu xẹt → giảm xuống 0x20
3. Nếu vẫn xẹt → giảm xuống 0x15

---

## 📚 Tài Liệu Tham Khảo

### Header Files
- `driver/aud_dmic.h` - DMIC APIs
- `driver/aud_dac.h` - DAC APIs  
- `driver/audio_ring_buff.h` - Ring Buffer APIs

### Key APIs

**DMIC:**
- `bk_aud_dmic_init()` - Khởi tạo DMIC
- `bk_aud_dmic_register_isr()` - Đăng ký interrupt handler
- `bk_aud_dmic_set_dmic_wr_threshold()` - Set FIFO threshold
- `bk_aud_dmic_enable_int()` - Bật interrupt
- `bk_aud_dmic_get_fifo_data()` - Đọc data từ FIFO

**DAC:**
- `bk_aud_dac_init()` - Khởi tạo DAC
- `bk_aud_dac_write()` - Ghi data vào DAC
- `bk_aud_dac_start()` - Start DAC

**Ring Buffer:**
- `ring_buffer_init()` - Khởi tạo buffer
- `ring_buffer_write()` - Ghi data
- `ring_buffer_read()` - Đọc data
- `ring_buffer_get_fill_size()` - Lấy số byte hiện có

---

## ⚡ Quick Fix Summary

Nếu bạn muốn fix nhanh, làm theo thứ tự:

1. **Bật ISR:** Uncomment `bk_aud_dmic_register_isr()` và `bk_aud_dmic_enable_int()`
2. **Tăng threshold:** `bk_aud_dmic_set_dmic_wr_threshold(128)`
3. **Giảm gain:** `dac_cfg.dac_gain = 0x2D`
4. **Xóa print:** Comment tất cả `os_printf` trong ISR và critical path
5. **Thêm ring buffer:** Implement ring buffer như solution 1
6. **Xóa delay 10ms** trong main loop

---

**Happy Coding! 🎵**
**Chúc bạn có âm thanh DMIC trong trẻo!**
