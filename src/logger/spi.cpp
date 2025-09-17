#include <cstdio>
#include <string>
#include <dirent.h>
#include <esp_log.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include "common.h"

static const char* TAG = "SPI";

bool spi_init()
{
    constexpr spi_bus_config_t bus_config = {
        .mosi_io_num = MOSI_IO_NUM,
        .miso_io_num = MISO_IO_NUM,
        .sclk_io_num = SCLK_IO_NUM,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .data_io_default_level = false,
        .max_transfer_sz = 4000,
        .flags = 0,
        .intr_flags = 0,
    };
    esp_err_t ret = spi_bus_initialize(SDCARD_HOST, &bus_config, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_initialize SDCARD_HOST: %s", esp_err_to_name(ret));
        return false;
    }

    constexpr spi_bus_config_t bus_config_qspi = {
        .data0_io_num = PIN_NUM_LCD_DATA0,
        .data1_io_num = PIN_NUM_LCD_DATA1,
        .sclk_io_num = PIN_NUM_LCD_PCLK,
        .data2_io_num = PIN_NUM_LCD_DATA2,
        .data3_io_num = PIN_NUM_LCD_DATA3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .data_io_default_level = false,
        .max_transfer_sz = 4000,
        .flags = 0,
        .intr_flags = 0,
    };

    ret = spi_bus_initialize(DISPLAY_HOST, &bus_config_qspi, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_initialize DISPLAY_HOST: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}
