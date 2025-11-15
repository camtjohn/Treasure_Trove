/*
 * Blink + OTA trigger program
 * - Blink task: GPIO40 ON 500ms, OFF 2000ms
 * - Button: GPIO0 (active-low). Pressing the button starts an OTA from OTA_URL.
 *
 * Partition table is provided in `partitions.csv` (two OTA slots).
 *
 * Notes:
 * - Update OTA_URL to point to your firmware binary on jbar.dev
 * - This example uses esp_https_ota for HTTPS OTA. For production, provide
 *   proper server certificate (set .crt_pem in esp_http_client_config_t) or
 *   configure secure connections appropriately.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "wifi.h"
#include "app.h"
#include "ota.h"

static const char *TAG = "blink_ota";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting blink+OTA app");

    Wifi__Start();
    vTaskDelay(pdMS_TO_TICKS(1000));

    App__Init();
    OTA__Init();
}
