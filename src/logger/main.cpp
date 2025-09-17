#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include "gui.h"
#include "wifi_web.h"
#include "logging.h"
#include "spi.h"

#ifndef APP_NAME
#define APP_NAME "UnknownApp"
#endif

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#ifndef GIT_REVISION
#define GIT_REVISION "unknown"
#endif

static const unsigned IDLE_TIME = 1 * 60 * 1000;
static const char* TAG = "CAN_Logger";

// === Counter Task ===
[[noreturn]] static void display_message_counter(void* arg)
{
    ESP_LOGI(TAG, "Starting display_message_counter task");
    while (true)
    {
        set_label2(get_message_count());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "\n=== %s Info ====", TAG);
    ESP_LOGI(TAG, "App: %s", APP_NAME);
    ESP_LOGI(TAG, "Version: %s", APP_VERSION);
    ESP_LOGI(TAG, "Git Rev: %s", GIT_REVISION);
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0)
    {
        ESP_LOGI(TAG, "PSRAM detected");
        ESP_LOGI(TAG, "Total PSRAM: %u bytes", (unsigned)psram_size);
        ESP_LOGI(TAG, "Free PSRAM:  %u bytes", (unsigned)psram_free);
    }
    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    spi_init();
    gui_init();
    mount_sdcard();

    // WiFi Phase
    wifi_init_softap();
    set_label1("AP");
    set_label2(get_ip_address());
    httpd_handle_t server = start_webserver();
    reset_web_activity();
    while (true)
    {
        if (esp_timer_get_time() / 1000ULL - get_last_web_activity() >= IDLE_TIME)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (server) httpd_stop(server);
    esp_wifi_stop();

    // Logging Mode
    set_label1("Logger");
    xTaskCreate(display_message_counter, "Counter", 4096, nullptr, 2, nullptr);
    start_logging_mode();
}
