#include "logging.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "common.h"
#include "esp_heap_caps.h"

// -----------------------------
// Shared config (from main)
// -----------------------------
#define CAN_TX_PIN         GPIO_NUM_18
#define CAN_RX_PIN         GPIO_NUM_17
#define CAN_QUEUE_LEN          600
#define SD_QUEUE_LEN          1600
#define BATCH_MAX_BYTES    (64*1024)
#define BATCH_MAX_MS       20
#define FICTIONAL_START_TIME 1755839937.312293  // due to missing RTC

static const char* TAG = "LOGGING_MODE";

// -----------------------------
// Log line structure
// -----------------------------
typedef struct
{
    uint16_t len;
    char data[38];
} LogLine;

// -----------------------------
// CAN message structure
// -----------------------------
typedef struct
{
    uint32_t id;
    uint8_t len;
    uint8_t buf[8];
    double timestamp;
} CANMessage_t;

// -----------------------------
// Globals
// -----------------------------
static FILE* logFile = nullptr;
static unsigned long messageCount = 0;
static unsigned long lastSync = 0;

static QueueHandle_t sdQueue = nullptr;
static QueueHandle_t canQueue = nullptr;

// Large batch buffer moved to heap/PSRAM to save internal DRAM for queues
static uint8_t* g_batchBuf = nullptr;
static size_t g_batchBufSize = BATCH_MAX_BYTES;

// -----------------------------
// Helpers
// -----------------------------
static unsigned long millis()
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static double get_unix_timestamp()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (double)tv.tv_sec + (tv.tv_usec / 1000000.0) + FICTIONAL_START_TIME;
}

// cleanup threshold (bytes)
#define SD_LOW_LIMIT   (2ULL * 1024 * 1024 * 1024)  // 2 GB
#define SD_TARGET_FREE (4ULL * 1024 * 1024 * 1024)  // 4 GB

// -----------------------------
// Next free filename (CANxxxxx.LOG) with cleanup
// -----------------------------
static void next_free_file_name(char* path, size_t path_size)
{
    int max_index = -1;
    struct dirent* entry;

    // Check free space
    uint64_t out_total = 0, out_free = 0;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &out_total, &out_free);
    if (err == ESP_OK)
    {
        ESP_LOGI("SD", "Free space: %llu bytes", (unsigned long long) out_free);

        if (out_free < SD_LOW_LIMIT)
        {
            ESP_LOGW("SD", "Low free space (<2GB). Deleting old files...");

            while (out_free < SD_TARGET_FREE)
            {
                int min_index = -1;
                DIR* d = opendir(SD_MOUNT_POINT);
                if (!d) break;

                while ((entry = readdir(d)) != nullptr)
                {
                    int idx;
                    if (sscanf(entry->d_name, "CAN%05d.LOG", &idx) == 1)
                    {
                        if (min_index == -1 || idx < min_index)
                        {
                            min_index = idx;
                        }
                    }
                }
                closedir(d);

                if (min_index == -1)
                {
                    ESP_LOGW("SD", "No CANxxxxx.LOG files to delete");
                    break;
                }

                char del_path[128];
                snprintf(del_path, sizeof(del_path), SD_MOUNT_POINT "/CAN%05d.LOG", min_index);
                ESP_LOGW("SD", "Deleting %s", del_path);
                unlink(del_path);

                if (esp_vfs_fat_info(SD_MOUNT_POINT, &out_total, &out_free) != ESP_OK) break;
            }

            ESP_LOGI("SD", "Free space after cleanup: %llu bytes", (unsigned long long) out_free);
        }
    }
    else
    {
        ESP_LOGW("SD", "esp_vfs_fat_info failed: %s", esp_err_to_name(err));
    }

    // Find next free filename
    DIR* dir = opendir(SD_MOUNT_POINT);
    if (dir == nullptr)
    {
        snprintf(path, path_size, SD_MOUNT_POINT "/CAN%05d.LOG", 0);
        return;
    }

    while ((entry = readdir(dir)) != nullptr)
    {
        int idx;
        if (sscanf(entry->d_name, "CAN%05d.LOG", &idx) == 1)
        {
            if (idx > max_index) max_index = idx;
        }
    }
    closedir(dir);

    snprintf(path, path_size, SD_MOUNT_POINT "/CAN%05d.LOG", max_index + 1);
}

// -----------------------------
// SD Card Init + File Open
// -----------------------------
static bool init_sd_card_and_open_file()
{
    ESP_LOGI("SD", "SD card mounted");
    char path[128];
    next_free_file_name(path, sizeof(path));
    logFile = fopen(path, "w");
    if (!logFile)
    {
        ESP_LOGE("SD", "fopen failed: %s", path);
        return false;
    }
    ESP_LOGI("SD", "Logging to: %s", path);
    static char io_buf[8 * 1024];
    setvbuf(logFile, io_buf, _IOFBF, sizeof(io_buf));
    const char* header = "* CAN Bus Log Started\n";
    fwrite(header, 1, strlen(header), logFile);
    fflush(logFile);
    fsync(fileno(logFile));
    return true;
}

// -----------------------------
// CAN Init
// -----------------------------
static bool init_can()
{
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK)
    {
        ESP_LOGE("CAN", "driver install failed");
        return false;
    }
    if (twai_start() != ESP_OK)
    {
        ESP_LOGE("CAN", "start failed");
        return false;
    }
    ESP_LOGI("CAN", "Driver installed and started");
    return true;
}

// -----------------------------
// Tasks
// -----------------------------
[[noreturn]] static void can_receiver_task(void* arg)
{
    twai_message_t message;
    while (true)
    {
        if (twai_receive(&message, pdMS_TO_TICKS(100)) == ESP_OK)
        {
            if (message.data_length_code > 0)
            {
                CANMessage_t msg;
                msg.id = message.identifier;
                msg.len = message.data_length_code;
                memcpy(msg.buf, message.data, msg.len);
                msg.timestamp = get_unix_timestamp();
                if (xQueueSend(canQueue, &msg, 0) != pdTRUE)
                {
                    ESP_LOGW("CAN_RX", "canQueue full, dropped");
                }
            }
        }
        taskYIELD();
    }
}

[[noreturn]] static void can_processor_task(void* arg)
{
    CANMessage_t msg;
    while (true)
    {
        if (xQueueReceive(canQueue, &msg, portMAX_DELAY) == pdTRUE)
        {
            LogLine line{};
            int n = snprintf(line.data, sizeof(line.data), "(%.6lf) can %03lX#", msg.timestamp, (unsigned long)msg.id);
            for (int i = 0; i < msg.len && n < (int)sizeof(line.data) - 2; i++)
            {
                n += snprintf(line.data + n, sizeof(line.data) - n, "%02X", msg.buf[i]);
            }
            if (n < (int)sizeof(line.data) - 1)
            {
                line.data[n++] = '\n';
            }
            else
            {
                line.data[sizeof(line.data) - 2] = '\n';
                n = sizeof(line.data) - 1;
            }
            line.data[n] = '\0';
            line.len = (uint16_t)n;

            if (xQueueSend(sdQueue, &line, 0) != pdTRUE)
            {
                ESP_LOGW("CAN_Proc", "sdQueue full, dropped line");
            }
            else
            {
                messageCount++;
            }
        }
    }
}

[[noreturn]] static void sd_writer_task(void* arg)
{
    while (true)
    {
        size_t used = 0;
        LogLine line;
        if (xQueueReceive(sdQueue, &line, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            if (g_batchBuf && line.len <= g_batchBufSize)
            {
                memcpy(g_batchBuf, line.data, line.len);
                used = line.len;
            }
            else if (logFile && line.len > 0)
            {
                // Fallback: write directly if no batch buffer
                fwrite(line.data, 1, line.len, logFile);
                fflush(logFile);
            }
        }

        TickType_t start = xTaskGetTickCount();
        while (g_batchBuf && (used + sizeof(LogLine::data) < g_batchBufSize))
        {
            if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= BATCH_MAX_MS) break;

            LogLine more;
            if (xQueueReceive(sdQueue, &more, 0) != pdTRUE) break;
            if (used + more.len > g_batchBufSize) break;
            memcpy(g_batchBuf + used, more.data, more.len);
            used += more.len;
        }

        if (used > 0 && logFile && g_batchBuf)
        {
            size_t written = fwrite(g_batchBuf, 1, used, logFile);
            if (written != used)
            {
                ESP_LOGE("SD", "fwrite failed: wrote %u of %u", (unsigned) written, (unsigned) used);
            }
            fflush(logFile);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// -----------------------------
// Public API: start logging mode
// -----------------------------
void start_logging_mode()
{
    if (!init_sd_card_and_open_file())
    {
        ESP_LOGE(TAG, "SD init/open failed for logging");
        return;
    }

    if (!init_can())
    {
        ESP_LOGE(TAG, "CAN init failed!");
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!init_can())
        {
            ESP_LOGE(TAG, "CAN init failed permanently!");
            return;
        }
    }

    canQueue = xQueueCreate(CAN_QUEUE_LEN, sizeof(CANMessage_t));
    sdQueue = xQueueCreate(SD_QUEUE_LEN, sizeof(LogLine));
    if (!canQueue || !sdQueue)
    {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    // Allocate batch buffer in PSRAM if available to preserve internal DRAM for queues
    if (!g_batchBuf)
    {
        g_batchBuf = (uint8_t*)heap_caps_malloc(BATCH_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_batchBuf)
        {
            g_batchBufSize = BATCH_MAX_BYTES;
            ESP_LOGI("SD", "Batch buffer allocated in PSRAM: %u bytes", (unsigned) g_batchBufSize);
        }
        else
        {
            g_batchBuf = (uint8_t*)heap_caps_malloc(BATCH_MAX_BYTES, MALLOC_CAP_8BIT);
            if (g_batchBuf)
            {
                g_batchBufSize = BATCH_MAX_BYTES;
                ESP_LOGI("SD", "Batch buffer allocated in internal heap: %u bytes", (unsigned) g_batchBufSize);
            }
            else
            {
                g_batchBufSize = 0;
                ESP_LOGW("SD", "Batch buffer allocation failed; will write line-by-line without batching");
            }
        }
    }

    xTaskCreate(can_receiver_task, "CAN_RX", 4096, nullptr, 5, nullptr);
    xTaskCreate(can_processor_task, "CAN_Proc", 4096, nullptr, 4, nullptr);
    if (logFile)
    {
        xTaskCreate(sd_writer_task, "SD_Writer", 8192, nullptr, 3, nullptr);
    }

    int stat_cnt = 0;
    while (true)
    {
        if (millis() - lastSync >= 1000)
        {
            lastSync = millis();
            if (logFile) fsync(fileno(logFile));
            if (stat_cnt++ >= 60)
            {
                ESP_LOGI(TAG, "Messages: %lu", messageCount);
                stat_cnt = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

long get_message_count()
{
    return messageCount;
}
