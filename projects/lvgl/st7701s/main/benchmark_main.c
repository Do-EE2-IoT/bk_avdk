#include "bk_private/bk_init.h"
#include <components/system.h>
#include <os/os.h>
#include <components/shell_task.h>
#include <driver/pwr_clk.h>
#include "cli.h"
#if (CONFIG_SYS_CPU0) || (CONFIG_SYS_CPU1)
#include "lcd_act.h"
#include "media_app.h"
#endif
#if CONFIG_LVGL
#include "lv_vendor.h"
#include "lv_demo_benchmark.h"
#endif
#include "driver/drv_tp.h"
#include <driver/lcd.h>
#include "media_service.h"

extern void user_app_main(void);
extern void rtos_set_user_app_entry(beken_thread_function_t entry);

#define CMDS_COUNT (sizeof(s_benchmark_commands) / sizeof(struct cli_command))

const lcd_open_t lcd_open =
    {
        .device_ppi = PPI_480X480,
        .device_name = "st7701s",
};

void cli_benchmark_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
    os_printf("%s\r\n", __func__);
}

static const struct cli_command s_benchmark_commands[] =
    {
        {"benchmark", "benchmark", cli_benchmark_cmd},
};

int cli_benchmark_init(void)
{
    return cli_register_commands(s_benchmark_commands, CMDS_COUNT);
}

#if (CONFIG_SYS_CPU1)
#include "yuv_encode.h"
/* Thêm include nếu thiếu, thường nó nằm trong driver/drv_tp.h */
// #include "driver/drv_tp.h"
void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    tp_point_infor_t point_info; // Biến tạm để chứa dữ liệu đọc ra

    // Biến static để lưu trạng thái cuối cùng (quan trọng khi Queue rỗng)
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    static lv_indev_state_t last_state = LV_INDEV_STATE_REL;

    // 1. Gọi hàm đọc từ Queue
    int ret = drv_tp_read(&point_info);

    if (ret == kNoErr) // Đọc thành công (có dữ liệu mới trong Queue)
    {
        // 2. Cập nhật tọa độ mới
        last_x = point_info.m_x;
        last_y = point_info.m_y;

        // 3. Cập nhật trạng thái (Nhấn hay Nhả)
        // Giả sử m_state = 1 là nhấn/di chuyển (dựa trên code cũ của bạn)
        if (point_info.m_state == 1)
        {
            last_state = LV_INDEV_STATE_PR;
        }
        else
        {
            last_state = LV_INDEV_STATE_REL;
        }

        // 4. TỐI ƯU QUAN TRỌNG: Kiểm tra xem còn dữ liệu trong Queue không?
        // Nếu point_info.m_need_continue == 1, báo cho LVGL biết để gọi hàm này ngay lập tức
        // giúp xử lý hết các điểm chạm tồn đọng -> Cảm ứng mượt hơn, không bị trễ.
        if (point_info.m_need_continue == 1)
        {
            data->continue_reading = true;
        }
        else
        {
            data->continue_reading = false;
        }
    }
    else
    {
        // Queue rỗng (không có dữ liệu mới)
        // Giữ nguyên trạng thái cũ và không yêu cầu đọc tiếp
        data->continue_reading = false;
    }

    // 5. Gán dữ liệu cuối cùng cho LVGL
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = last_state;
}

void lvgl_event_handle(media_mailbox_msg_t *msg)
{
    os_printf("%s EVENT_LVGL_OPEN_IND \n", __func__);

    lv_vnd_config_t lv_vnd_config = {0};
    lcd_open_t *lcd_open = (lcd_open_t *)msg->param;

#ifdef CONFIG_LVGL_USE_PSRAM
#define PSRAM_DRAW_BUFFER ((0x60000000UL) + 5 * 1024 * 1024)

    lv_vnd_config.draw_pixel_size = ppi_to_pixel_x(lcd_open->device_ppi) * ppi_to_pixel_y(lcd_open->device_ppi);
    lv_vnd_config.draw_buf_2_1 = (lv_color_t *)PSRAM_DRAW_BUFFER;
    lv_vnd_config.draw_buf_2_2 = (lv_color_t *)(PSRAM_DRAW_BUFFER + lv_vnd_config.draw_pixel_size * sizeof(lv_color_t));
#else

#if CONFIG_LV_ATTRIBUTE_FAST_MEM_L2
#define DRAW_BUFFER_SIZE (124 * 1024 / 2)
#else
#define DRAW_BUFFER_SIZE (128 * 1024 / 2)
#endif
#define PSRAM_FRAME_BUFFER ((0x60000000UL) + 5 * 1024 * 1024)
    static __attribute__((section(".lvgl_draw"))) uint8_t draw_buf1[DRAW_BUFFER_SIZE];
    static __attribute__((section(".lvgl_draw"))) uint8_t draw_buf2[DRAW_BUFFER_SIZE];

    lv_vnd_config.draw_pixel_size = DRAW_BUFFER_SIZE / sizeof(lv_color_t);
    lv_vnd_config.draw_buf_2_1 = (lv_color_t *)draw_buf1;
    lv_vnd_config.draw_buf_2_2 = (lv_color_t *)draw_buf2;
    lv_vnd_config.frame_buf_1 = (lv_color_t *)PSRAM_FRAME_BUFFER;
    lv_vnd_config.frame_buf_2 = NULL;
#endif
    lv_vnd_config.lcd_hor_res = ppi_to_pixel_x(lcd_open->device_ppi);
    lv_vnd_config.lcd_ver_res = ppi_to_pixel_y(lcd_open->device_ppi);
    lv_vnd_config.rotation = ROTATE_NONE;

    lv_vendor_init(&lv_vnd_config);

    lcd_display_open(lcd_open);
#if (CONFIG_TP)
    drv_tp_open(ppi_to_pixel_x(lcd_open->device_ppi), ppi_to_pixel_y(lcd_open->device_ppi), TP_MIRROR_NONE);
    static lv_indev_drv_t indev_drv;

    // 1. Khởi tạo driver input
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;

    // 2. Đăng ký với LVGL và LẤY CON TRỎ TRẢ VỀ
    lv_indev_t *my_indev_handle = lv_indev_drv_register(&indev_drv);

    // --- THÊM ĐOẠN TẠO CURSOR (ĐIỂM ĐỎ) ---

    // Tạo một đối tượng trên lớp hệ thống (để luôn nổi lên trên cùng)
    lv_obj_t *cursor_obj = lv_obj_create(lv_layer_sys());

    // Thiết lập kích thước (ví dụ 20x20 pixel)
    lv_obj_set_size(cursor_obj, 20, 20);

    // Thiết lập kiểu dáng: Tròn và màu đỏ
    lv_obj_set_style_radius(cursor_obj, LV_RADIUS_CIRCLE, 0);                  // Bo tròn hoàn toàn
    lv_obj_set_style_bg_color(cursor_obj, lv_palette_main(LV_PALETTE_RED), 0); // Màu đỏ
    lv_obj_set_style_border_width(cursor_obj, 2, 0);                           // Viền (tùy chọn)
    lv_obj_set_style_border_color(cursor_obj, lv_color_white(), 0);            // Viền trắng cho dễ nhìn

    // QUAN TRỌNG: Làm cho điểm đỏ "trong suốt" với thao tác nhấn
    // Nếu không có dòng này, điểm đỏ sẽ chặn sự kiện click vào nút bên dưới nó
    lv_obj_clear_flag(cursor_obj, LV_OBJ_FLAG_CLICKABLE);

    // Gán đối tượng này làm con trỏ cho driver cảm ứng
    lv_indev_set_cursor(my_indev_handle, cursor_obj);

    // ---------------------------------------

#endif

    lv_vendor_disp_lock();
    lv_demo_benchmark();
    lv_vendor_disp_unlock();

    lv_vendor_start();

    msg_send_rsp_to_media_major_mailbox(msg, BK_OK, APP_MODULE);
}
#endif

#if (CONFIG_SYS_CPU0)
void benchmark_init(void)
{
    bk_err_t ret;

    os_printf("!!!BK7258 LVGL BENCHMARK!!!\r\n");

    cli_benchmark_init();

    ret = media_app_lvgl_open((lcd_open_t *)&lcd_open);
    if (ret != BK_OK)
    {
        os_printf("media_app_lvgl_open failed\r\n");
        return;
    }
}
#endif

void user_app_main(void)
{
}

int main(void)
{
#if (CONFIG_SYS_CPU0)
    rtos_set_user_app_entry((beken_thread_function_t)user_app_main);
#if (CONFIG_LV_CODE_LOAD_PSRAM)
    bk_pm_module_vote_psram_ctrl(PM_POWER_PSRAM_MODULE_NAME_LVGL_CODE_RUN, PM_POWER_MODULE_STATE_ON);
#endif
    // bk_set_printf_sync(true);
    // shell_set_log_level(BK_LOG_WARN);
#endif
    bk_init();
    media_service_init();

#if (CONFIG_SYS_CPU0)
    benchmark_init();
#endif

    return 0;
}
