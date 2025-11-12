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
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "wifi.h"

// GPIOs
#define BLINK_GPIO  GPIO_NUM_40
#define BUTTON_GPIO GPIO_NUM_0

// Blink timings
#define ON_MS  500
#define OFF_MS 2000

// OTA URL (change as needed). Example: https://jbar.dev/firmware.bin
#ifndef OTA_URL
#define OTA_URL "https://jbar.dev/firmware.bin"
#endif

static const char *TAG = "blink_ota";

// Symbols created by EMBED_TXTFILES for TLS_Keys/ca.crt in main/CMakeLists.txt
// The linker symbols are named like: _binary_<path_with_underscores>_start/_end
extern const uint8_t _binary_ca_crt_start[];
extern const uint8_t _binary_ca_crt_end[];


static void blink_task(void *arg)
{
    (void)arg;
    while (1) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(ON_MS));
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(OFF_MS));
    }
}

static void ota_task(void *arg)
{
    char *url = (char *)arg;
    if (!url) {
        ESP_LOGE(TAG, "OTA task started with NULL URL");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    // Log free heap to help diagnose allocation failures during OTA
    size_t free_heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap before OTA: %u bytes", (unsigned)free_heap_before);

    const uint8_t *crt_start = _binary_ca_crt_start;
    const uint8_t *crt_end = _binary_ca_crt_end;
    size_t cert_len = 0;
    char *cert_pem = NULL;

    if (crt_end > crt_start) {
        cert_len = (size_t)(crt_end - crt_start);
    }

    if (cert_len > 0 && cert_len < 65536) {
        cert_pem = malloc(cert_len + 1);
        if (cert_pem) {
            memcpy(cert_pem, crt_start, cert_len);
            cert_pem[cert_len] = '\0';
        } else {
            ESP_LOGW(TAG, "Failed to allocate memory for cert, continuing without cert verification");
        }
    } else if (cert_len != 0) {
        ESP_LOGW(TAG, "Unexpected cert size %u, skipping cert", (unsigned)cert_len);
    } else {
        ESP_LOGW(TAG, "No embedded CA certificate found, skipping cert verification");
    }

    // Reduce buffer size to lower heap usage inside the http client / TLS stack.
    // Default buffer is often 8k; using 2k or 4k reduces peak RAM needed.
    esp_http_client_config_t http_config = {
        .url = url,
        .cert_pem = cert_pem,
        .keep_alive_enable = false, // disable keep-alive to free connection resources
        .buffer_size = 2048,
    };

    esp_err_t ret = esp_https_ota(&http_config);

    if (cert_pem) {
        free(cert_pem);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        free(url);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed. err=0x%x", ret);
        free(url);
    }

    vTaskDelete(NULL);
}

static void button_task(void *arg)
{
    (void)arg;
    int last_level = gpio_get_level(BUTTON_GPIO);

    while (1) {
        int level = gpio_get_level(BUTTON_GPIO);
        // detect falling edge (high -> low)
        if (level == 0 && last_level == 1) {
            // debounce
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Button pressed -> starting OTA task");
                // Create OTA task
                char *url_copy = strdup(OTA_URL);
                if (url_copy) {
                    if (xTaskCreate(ota_task,
                                    "ota_task",
                                    8192,
                                    url_copy,
                                    5,
                                    NULL) != pdPASS) {
                        ESP_LOGE(TAG, "Failed to create OTA task");
                        free(url_copy);
                    }
                }
                // wait until release to avoid multiple triggers
                while (gpio_get_level(BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting blink+OTA app");

    Wifi__Start();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Configure blink pin
    gpio_config_t out_conf = {
        .pin_bit_mask = 1ULL << BLINK_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));

    // Configure button pin (input, pull-up)
    gpio_config_t btn_conf = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    // Start blink task
    xTaskCreate(blink_task, "blink_task", 2048, NULL, 5, NULL);

    // Start button monitor task
    xTaskCreate(button_task, "button_task", 4096, NULL, 6, NULL);
}
