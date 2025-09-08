#pragma once

#include "lvgl.h"

void gui_log_init(lv_obj_t *parent);
void gui_log(const char *fmt, ...);