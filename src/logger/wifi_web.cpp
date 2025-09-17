#include "wifi_web.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "common.h"

#define WIFI_PASSWORD     "12345678"

#pragma GCC diagnostic ignored "-Wformat-truncation"  // todo -- fix that!!

static const char* TAG = "WIFI_WEB";

// Track last HTTP request activity
static unsigned long lastWebActivity = 0;

static unsigned long millis()
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

unsigned long get_last_web_activity() { return lastWebActivity; }

void reset_web_activity() { lastWebActivity = millis(); }

// Function to generate unique SSID
static std::string generate_unique_ssid()
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac); // Get AP MAC
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "CAN_Logger_%02X%02X%02X", mac[3], mac[4], mac[5]);
    return std::string(ssid);
}

std::string human_size(size_t bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unitIndex = 0;
    auto size = static_cast<double>(bytes);

    while (size >= 1024 && unitIndex < 3)
    {
        size /= 1024.0;
        unitIndex++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f %s", size, units[unitIndex]);
    return std::string(buf);
}

void wifi_init_softap()
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    std::string ssid = generate_unique_ssid();
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, ssid.c_str(), sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = ssid.length();
    strncpy((char*)wifi_config.ap.password, WIFI_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen(WIFI_PASSWORD) == 0) wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started. SSID:%s password:%s", ssid.c_str(), WIFI_PASSWORD);
}

// ---- HTTP Handlers ----
esp_err_t root_get_handler(httpd_req_t* req)
{
    reset_web_activity();
    DIR* dir = opendir(SD_MOUNT_POINT);
    if (!dir)
    {
        httpd_resp_sendstr(req, "Failed to open SD card root");
        return ESP_FAIL;
    }

    std::string html =
        "<!DOCTYPE html><html><head><title>ESP32 File Browser</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{font-family:Arial;padding:1rem;}table{border-collapse:collapse;width:100%;}"
        "th,td{padding:8px;border-bottom:1px solid #ccc;text-align:left;}th{background:#eee;}"
        "a{text-decoration:none;color:#0066cc;word-break:break-all;}</style></head><body>"
        "<h2>ESP32 File Browser (SD Card)</h2><table><tr><th>Name</th><th>Size</th></tr>";

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char fullpath[256];
        snprintf(fullpath, sizeof(fullpath), SD_MOUNT_POINT "/%s", entry->d_name);

        struct stat st{};
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode))
        {
            html += "<tr><td>[DIR] ";
            html += entry->d_name;
            html += "</td><td></td></tr>";
        }
        else
        {
            html += "<tr><td><a href=\"/download?file=";
            html += entry->d_name;
            html += "\">";
            html += entry->d_name;
            html += "</a></td><td>";
            html += human_size(st.st_size);
            html += "</td></tr>";
        }
    }
    closedir(dir);

    html += "</table></body></html>";
    httpd_resp_send(req, html.c_str(), html.size());
    return ESP_OK;
}

esp_err_t download_get_handler(httpd_req_t* req)
{
    reset_web_activity();
    char filepath[256];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        std::unique_ptr<char[]> buf(new char[buf_len]);
        if (httpd_req_get_url_query_str(req, buf.get(), buf_len) == ESP_OK)
        {
            char param[128];
            if (httpd_query_key_value(buf.get(), "file", param, sizeof(param)) == ESP_OK)
            {
                snprintf(filepath, sizeof(filepath), SD_MOUNT_POINT "/%s", param);
                FILE* f = fopen(filepath, "rb");
                if (!f)
                {
                    httpd_resp_send_404(req);
                    return ESP_FAIL;
                }

                const char* ext = strrchr(param, '.');
                if (ext &&
                    (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".csv") == 0 || strcasecmp(ext, ".log") == 0))
                {
                    httpd_resp_set_type(req, "text/plain; charset=utf-8");
                }
                else
                {
                    httpd_resp_set_type(req, "application/octet-stream");
                    char header[256];
                    snprintf(header, sizeof(header), "attachment; filename=\"%s\"", param);
                    httpd_resp_set_hdr(req, "Content-Disposition", header);
                }

                char chunk[1024];
                size_t read_bytes;
                while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0)
                {
                    if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK)
                    {
                        fclose(f);
                        httpd_resp_sendstr_chunk(req, nullptr);
                        return ESP_FAIL;
                    }
                }
                fclose(f);
                httpd_resp_send_chunk(req, nullptr, 0);
            }
        }
    }
    return ESP_OK;
}

httpd_handle_t start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = nullptr};
        httpd_uri_t download = {
            .uri = "/download", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = nullptr
        };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &download);
    }
    return server;
}
