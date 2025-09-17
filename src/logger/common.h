#pragma once

#include <soc/gpio_num.h>

#define MOSI_IO_NUM         GPIO_NUM_39
#define MISO_IO_NUM         GPIO_NUM_40
#define SCLK_IO_NUM         GPIO_NUM_41
#define GPIO_CS             GPIO_NUM_38

#define PIN_NUM_LCD_CS      GPIO_NUM_9
#define PIN_NUM_LCD_PCLK    GPIO_NUM_10
#define PIN_NUM_LCD_DATA0   GPIO_NUM_11
#define PIN_NUM_LCD_DATA1   GPIO_NUM_12
#define PIN_NUM_LCD_DATA2   GPIO_NUM_13
#define PIN_NUM_LCD_DATA3   GPIO_NUM_14
#define PIN_NUM_LCD_RST     GPIO_NUM_21

#define SDCARD_HOST SPI2_HOST
#define DISPLAY_HOST SPI3_HOST

#define SD_MOUNT_POINT "/SDCARD"
