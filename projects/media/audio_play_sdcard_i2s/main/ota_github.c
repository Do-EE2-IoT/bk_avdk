/**
 * ota_github.c – OTA qua GitHub Releases (HTTPS)
 *
 * Luồng:
 *   1. Query  https://api.github.com/repos/<owner>/<repo>/releases/latest
 *   2. Parse  tag_name + browser_download_url của asset OTA_FIRMWARE_ASSET_NAME
 *   3. So sánh version → nếu khác → gọi bk_https_ota_download() → reboot
 *
 * NOTE: Không dùng cJSON để tiết kiệm RAM. Parse bằng strstr/sscanf thuần.
 * NOTE: Cần bật CONFIG_OTA_HTTPS=y trong sdkconfig.
 */

#include "sdkconfig.h"

#ifdef CONFIG_OTA_HTTPS

#include <string.h>
#include <stdio.h>

#include "components/log.h"
#include "components/system.h"
#include "os/os.h"          /* rtos_create_thread, rtos_delay_milliseconds */
#if CONFIG_HTTPS
#include "bk_https.h"       /* bk_http_client_init, bk_http_client_perform */
#endif
#include "modules/ota.h"    /* bk_reboot                                   */

#include "ota_github.h"

/* =========================================================
 *  TAG cho log
 * ========================================================= */
#define TAG "OTA_GH"

/* =========================================================
 *  GitHub API – buffer sizes
 * ========================================================= */
#define OTA_API_RESP_BUF_SIZE   (4096)   /* Đủ chứa JSON response của /releases/latest */
#define OTA_VERSION_BUF_SIZE    (32)
#define OTA_URL_BUF_SIZE        (512)

/* =========================================================
 *  CA Certificate cho GitHub (DigiCert High Assurance EV Root CA)
 *  Dùng cho api.github.com và objects.githubusercontent.com
 * ========================================================= */
static const char s_github_ca_cert[] =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh\r\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\r\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\r\n"
    "QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT\r\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\r\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG\r\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB\r\n"
    "CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97\r\n"
    "nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt\r\n"
    "43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P\r\n"
    "T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4\r\n"
    "gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO\r\n"
    "BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR\r\n"
    "TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw\r\n"
    "DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr\r\n"
    "hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg\r\n"
    "06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF\r\n"
    "PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls\r\n"
    "YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk\r\n"
    "CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=\r\n"
    "-----END CERTIFICATE-----\r\n";

/* =========================================================
 *  Buffer dùng chung trong task OTA (static để tiết kiệm stack)
 * ========================================================= */
static char s_resp_buf[OTA_API_RESP_BUF_SIZE];
static int  s_resp_len = 0;

/* =========================================================
 *  HTTP event callback – gom JSON response vào s_resp_buf
 * ========================================================= */
static bk_err_t s_github_api_event_cb(bk_http_client_event_t *evt)
{
    if (!evt) return BK_FAIL;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0) {
            int remain = OTA_API_RESP_BUF_SIZE - s_resp_len - 1;
            if (remain > 0) {
                int copy = (evt->data_len < remain) ? evt->data_len : remain;
                memcpy(s_resp_buf + s_resp_len, evt->data, copy);
                s_resp_len += copy;
                s_resp_buf[s_resp_len] = '\0';
            }
        }
        break;
    case HTTP_EVENT_ERROR:
        BK_LOGE(TAG, "HTTP_EVENT_ERROR\r\n");
        break;
    default:
        break;
    }
    return BK_OK;
}

/* =========================================================
 *  Helper: trích chuỗi giữa key":"  và  "
 *  Ví dụ: json_get_string(buf, "tag_name", out, sizeof(out))
 *  Trả về 0 nếu tìm thấy, -1 nếu không.
 * ========================================================= */
static int json_get_string(const char *json, const char *key,
                            char *out, int out_size)
{
    /* Tìm "key":" */
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);

    const char *p = strstr(json, search);
    if (!p) return -1;

    p += strlen(search);  /* trỏ vào đầu value */

    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* =========================================================
 *  Tìm download_url cho asset có tên OTA_FIRMWARE_ASSET_NAME
 *  trong mảng "assets" của JSON response.
 *
 *  Logic:
 *    Tìm "name":"<OTA_FIRMWARE_ASSET_NAME>" rồi QUÉT XUÔI
 *    để tìm "browser_download_url" thuộc về file đó.
 * ========================================================= */
static int find_asset_download_url(const char *json, char *url_out, int url_size)
{
    /* 1. Tìm vị trí của asset name */
    char asset_search[64];
    snprintf(asset_search, sizeof(asset_search), "\"name\":\"%s\"", OTA_FIRMWARE_ASSET_NAME);

    const char *asset_pos = strstr(json, asset_search);
    if (!asset_pos) {
        BK_LOGE(TAG, "Asset '%s' not found in release\r\n", OTA_FIRMWARE_ASSET_NAME);
        return -1;
    }

    /* 2. Tìm "browser_download_url":"..." SAU vị trí asset_pos */
    const char *dl_key = "\"browser_download_url\":\"";
    const char *found = strstr(asset_pos, dl_key);

    if (!found) {
        BK_LOGE(TAG, "browser_download_url not found\r\n");
        return -1;
    }

    found += strlen(dl_key);
    int i = 0;
    while (*found && *found != '"' && i < url_size - 1) {
        url_out[i++] = *found++;
    }
    url_out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* =========================================================
 *  Query GitHub API để lấy latest release info.
 *  Điền version_out và url_out nếu thành công.
 *  Trả về 0 = OK, âm = lỗi.
 * ========================================================= */
static int ota_query_github(char *version_out, int version_size,
                             char *url_out,     int url_size)
{
    /* Build API URL */
    char api_url[256];
    snprintf(api_url, sizeof(api_url),
             "https://api.github.com/repos/%s/%s/releases/latest",
             OTA_GITHUB_OWNER, OTA_GITHUB_REPO);

    BK_LOGW(TAG, "Query: %s\r\n", api_url);

    /* Reset buffer */
    memset(s_resp_buf, 0, sizeof(s_resp_buf));
    s_resp_len = 0;

    bk_http_input_t config = {
        .url           = api_url,
        .cert_pem      = s_github_ca_cert,
        .event_handler = s_github_api_event_cb,
        .buffer_size   = 2048,
        .timeout_ms    = 15000,
    };

    bk_http_client_handle_t client = bk_http_client_init(&config);
    if (!client) {
        BK_LOGE(TAG, "bk_http_client_init failed\r\n");
        return -1;
    }

    bk_err_t ret = bk_http_client_perform(client);
    bk_http_client_cleanup(client);

    if (ret != BK_OK) {
        BK_LOGE(TAG, "HTTP perform failed: %d\r\n", ret);
        return -2;
    }

    if (s_resp_len == 0) {
        BK_LOGE(TAG, "Empty response from GitHub API\r\n");
        return -3;
    }

    BK_LOGD(TAG, "Response (%d bytes): %.200s...\r\n", s_resp_len, s_resp_buf);

    /* Parse tag_name */
    if (json_get_string(s_resp_buf, "tag_name", version_out, version_size) != 0) {
        BK_LOGE(TAG, "Parse tag_name failed\r\n");
        return -4;
    }
    BK_LOGW(TAG, "Latest version on GitHub: %s\r\n", version_out);

    /* Parse download URL của asset */
    if (find_asset_download_url(s_resp_buf, url_out, url_size) != 0) {
        BK_LOGE(TAG, "Parse download URL failed\r\n");
        return -5;
    }
    BK_LOGW(TAG, "Firmware URL: %s\r\n", url_out);

    return 0;
}

/* =========================================================
 *  Task OTA chạy trong background
 * ========================================================= */
static void ota_github_task(beken_thread_arg_t arg)
{
    (void)arg;

    char latest_version[OTA_VERSION_BUF_SIZE] = {0};
    char firmware_url[OTA_URL_BUF_SIZE]        = {0};

    BK_LOGW(TAG, "=== OTA: Checking for updates ===\r\n");
    BK_LOGW(TAG, "Current firmware version: %s\r\n", OTA_CURRENT_VERSION);

    /* Chờ DHCP cấp IP (STA_CONNECTED fire trước DHCP hoàn thành ~3-5s) */
    rtos_delay_milliseconds(5000);

    /* 1. Query GitHub API */
    int qret = ota_query_github(latest_version, sizeof(latest_version),
                                 firmware_url,   sizeof(firmware_url));
    if (qret != 0) {
        BK_LOGE(TAG, "Failed to query GitHub (err=%d). OTA skipped.\r\n", qret);
        goto done;
    }

    /* 2. So sánh version */
    if (strcmp(latest_version, OTA_CURRENT_VERSION) == 0) {
        BK_LOGW(TAG, "Firmware is up-to-date (%s). No OTA needed.\r\n", OTA_CURRENT_VERSION);
        goto done;
    }

    BK_LOGW(TAG, "New version found: %s (current: %s). Starting download...\r\n",
            latest_version, OTA_CURRENT_VERSION);

    /* 3. Download & Flash */
    extern int bk_https_ota_download(const char *url);
    int dret = bk_https_ota_download(firmware_url);
    if (dret == 0) {
        /* bk_https_ota_download gọi bk_reboot() bên trong nếu thành công,
         * nhưng để an toàn ta gọi thêm lần nữa ở đây. */
        BK_LOGI(TAG, "OTA download OK. Rebooting...\r\n");
        bk_reboot();
    } else {
        BK_LOGE(TAG, "OTA download failed (err=%d). Device continues.\r\n", dret);
    }

done:
    rtos_delete_thread(NULL);
}

/* =========================================================
 *  Public API – gọi từ app_main sau khi Wi-Fi got IP
 * ========================================================= */
void ota_github_start(void)
{
    os_printf( "Spawning OTA task...\r\n");
    bk_err_t ret = rtos_create_thread(
        NULL,
        BEKEN_APPLICATION_PRIORITY,
        "ota_github",
        (beken_thread_function_t)ota_github_task,
        8192,   /* Stack 8KB – cần đủ cho HTTPS + JSON buffer */
        0
    );

    if (ret != BK_OK) {
        BK_LOGE(TAG, "Failed to create OTA task (err=%d)\r\n", (int)ret);
    }
}

#else  /* CONFIG_OTA_HTTPS not set */

#include "components/log.h"
#define TAG "OTA_GH"

void ota_github_start(void)
{
    BK_LOGW(TAG, "CONFIG_OTA_HTTPS is not enabled. OTA disabled.\r\n");
}

#endif /* CONFIG_OTA_HTTPS */
