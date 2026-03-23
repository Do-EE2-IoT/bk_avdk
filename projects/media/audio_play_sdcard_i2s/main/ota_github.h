#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Version firmware hiện tại đang chạy trên device.
 *         Thay đổi giá trị này mỗi khi build firmware mới và đẩy lên GitHub Release.
 *         Format: "vMAJOR.MINOR.PATCH"
 */
#define OTA_CURRENT_VERSION   "v0.0.1"

/**
 * @brief  Thông tin GitHub repository chứa firmware release.
 *         Thay <YOUR_GITHUB_USER> và <YOUR_REPO> bằng thông tin thực tế.
 *
 *  Ví dụ:
 *    #define OTA_GITHUB_OWNER  "your-username"
 *    #define OTA_GITHUB_REPO   "bk7258-firmware"
 *
 *  API endpoint sẽ là:
 *    https://api.github.com/repos/<owner>/<repo>/releases/latest
 */
#define OTA_GITHUB_OWNER      "Do-EE2-IoT"
#define OTA_GITHUB_REPO       "bk_avdk"

/**
 * @brief  Tên file asset (.bin) trong GitHub Release.
 *         Phải khớp với tên file bạn upload lên Release Asset.
 */
#define OTA_FIRMWARE_ASSET_NAME  "all-app.bin"

/**
 * @brief  Khởi động task OTA (chạy trong background thread).
 *         Gọi hàm này SAU KHI Wi-Fi đã kết nối và có IP.
 *         Task sẽ:
 *           1. Query GitHub Releases API để lấy version mới nhất.
 *           2. So sánh với OTA_CURRENT_VERSION.
 *           3. Nếu có bản mới → download và flash → reboot.
 *           4. Nếu đã là mới nhất → log và thoát.
 */
void ota_github_start(void);

#ifdef __cplusplus
}
#endif
