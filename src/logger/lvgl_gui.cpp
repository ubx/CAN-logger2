#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static lv_obj_t *log_area = nullptr;

[[noreturn]] static void LVGL_task(void* arg)
{
    while (1)
    {
        lv_timer_handler(); // process LVGL tasks
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void gui_log_init(lv_obj_t *parent)
{
    log_area = lv_textarea_create(parent);
    lv_obj_set_size(log_area, 300, 200);   // adjust to your screen
    lv_obj_align(log_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_textarea_set_cursor_click_pos(log_area, false);
    lv_textarea_set_text(log_area, "");    // start empty
    xTaskCreate(LVGL_task, "LVGL_task", 4096, nullptr, 1, nullptr);
}

void gui_log(const char *fmt, ...)
{
    if (!log_area) return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Append text
    lv_textarea_add_text(log_area, buf);
    lv_textarea_add_text(log_area, "\n");
}