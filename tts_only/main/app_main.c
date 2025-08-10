#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"

static const char *TAG = "TTS_ONLY";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "TTS-only project (ESP-IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip features: %d cores, WiFi%s%s, silicon rev %d",
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "",
             chip_info.revision);

    while (true) {
        ESP_LOGI(TAG, "Hello from TTS-only skeleton");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}