#include "wifi_web.h"
#include "logging.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"

extern "C" void app_main(void) {
    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // WiFi Phase
    mount_sd_for_wifi();
    wifi_init_softap();
    httpd_handle_t server = start_webserver();

    vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    reset_web_activity();

    while (true) {
        if (esp_timer_get_time() / 1000ULL - get_last_web_activity() >= 5 * 60 * 1000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (server) httpd_stop(server);
    esp_wifi_stop();
    unmount_sd_after_wifi();

    // Logging Mode
    start_logging_mode();
}
