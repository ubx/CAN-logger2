#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/task.h"

#include "esp_lcd_sh8601.h"
#include "lvgl.h"

static const char* TAG = "GUI";

// === Pin and LCD Config ===
#define LCD_HOST  SPI2_HOST

#define LCD_H_RES              280
#define LCD_V_RES              456

#define PIN_NUM_LCD_CS         (GPIO_NUM_9)
#define PIN_NUM_LCD_PCLK       (GPIO_NUM_10)
#define PIN_NUM_LCD_DATA0      (GPIO_NUM_11)
#define PIN_NUM_LCD_DATA1      (GPIO_NUM_12)
#define PIN_NUM_LCD_DATA2      (GPIO_NUM_13)
#define PIN_NUM_LCD_DATA3      (GPIO_NUM_14)
#define PIN_NUM_LCD_RST        (GPIO_NUM_21)

#define LVGL_BUF_HEIGHT        (LCD_V_RES / 4)
#define LVGL_TICK_PERIOD_MS    2

static SemaphoreHandle_t lvgl_mux = nullptr;

// SH8601 init cmds
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 80},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 1},
    {0x63, (uint8_t []){0xFF}, 1, 1},
    {0x51, (uint8_t []){0x00}, 1, 1},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0}, // brightness
};

// === LVGL flush callback (v9) ===
static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    auto panel_handle = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));

    // LVGL reserves 8 bytes at the start for palette → skip
    uint8_t* pixels = px_map + 8;

    const int offsetx1 = area->x1 + 0x14;
    const int offsetx2 = area->x2 + 0x14;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel_handle,
                              offsetx1, offsety1,
                              offsetx2 + 1, offsety2 + 1,
                              pixels);

    lv_display_flush_ready(disp);
}

// Optional: align redraw areas to 2x2 boundary
static void lvgl_round_area_align(lv_area_t* area)
{
    int32_t x1 = area->x1;
    int32_t x2 = area->x2;
    int32_t y1 = area->y1;
    int32_t y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

// esp_lcd IO event callback
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t* edata,
                                    void* user_ctx)
{
    (void)panel_io;
    (void)edata;
    if (user_ctx)
    {
        auto* disp = static_cast<lv_display_t*>(user_ctx);
        lv_display_flush_ready(disp);
    }
    return false;
}

static void increase_lvgl_tick(void* arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// === LVGL Task ===
[[noreturn]] static void lvgl_task(void* arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    while (true)
    {
        if (xSemaphoreTake(lvgl_mux, portMAX_DELAY))
        {
            lv_timer_handler();
            xSemaphoreGive(lvgl_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// === Global LVGL label handles ===
static lv_obj_t* label1 = nullptr;
static lv_obj_t* label2 = nullptr;

// === Thread-safe setter functions ===
void set_label1(const char* text)
{
    if (xSemaphoreTake(lvgl_mux, portMAX_DELAY))
    {
        if (label1)
            lv_label_set_text(label1, text);
        xSemaphoreGive(lvgl_mux);
    }
}

void set_label2(const char* text)
{
    if (xSemaphoreTake(lvgl_mux, portMAX_DELAY))
    {
        if (label2)
            lv_label_set_text(label2, text);
        xSemaphoreGive(lvgl_mux);
    }
}

bool gui_init()
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_NUM_LCD_PCLK;
    buscfg.data0_io_num = PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = PIN_NUM_LCD_DATA1;
    buscfg.data2_io_num = PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * 2;

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // === LVGL Init ===
    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();

    // allocate two line buffers (+8 for LVGL internal palette reservation)
    const size_t buf_line_bytes = LCD_H_RES * LVGL_BUF_HEIGHT * 2;
    const size_t buf_alloc_bytes = buf_line_bytes + 8;

    void* buf1 = heap_caps_malloc(buf_alloc_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* buf2 = heap_caps_malloc(buf_alloc_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf1 || !buf2)
    {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
        abort();
    }

    lv_display_t* display = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!display)
    {
        ESP_LOGE(TAG, "lv_display_create failed");
        abort();
    }

    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);
    lv_display_set_buffers(display, buf1, buf2,
                           static_cast<uint32_t>(buf_alloc_bytes),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(display);

    // Start LVGL tick timer
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // Install panel IO
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
        PIN_NUM_LCD_CS,
        notify_lvgl_flush_ready,
        display // user_ctx
    );
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };

    ESP_LOGI(TAG, "Install SH8601 panel driver");
    esp_lcd_panel_handle_t panel_handle = nullptr;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Bind LVGL and panel
    lv_display_set_user_data(display, panel_handle);

    // Create LVGL mutex + task
    lvgl_mux = xSemaphoreCreateMutex();
    xTaskCreate(lvgl_task, "LVGL", 4096, nullptr, 2, nullptr);

    // === Show "Hello" ===
    if (xSemaphoreTake(lvgl_mux, portMAX_DELAY))
    {
        label1 = lv_label_create(lv_scr_act());
        label2 = lv_label_create(lv_scr_act());

        lv_label_set_text(label1, "");
        lv_label_set_text(label2, "");

        lv_obj_align(label1, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(label2, LV_ALIGN_CENTER, 0, 100);

        xSemaphoreGive(lvgl_mux);
    }

    ESP_LOGI(TAG, "Hello text displayed!");
    return false;
}
