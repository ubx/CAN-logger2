#include <esp_vfs_fat.h>
#include "common.h"

static const char* TAG = "SDCARD";

// ---- SD mounting ----
bool mount_sdcard()
{
    static sdmmc_card_t* card = nullptr;
    static sdmmc_host_t host = {};
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_CS;
    slot_config.host_id = SDCARD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_HOST;
    //host.max_freq_khz = 25 * 1000;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "mount: %s", esp_err_to_name(ret));
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted");
    return true;
}
