#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "app.h"
#include "ota.h"

static const char *TAG = "blink";

// GPIOs
#define BLINK_GPIO  GPIO_NUM_40
#define BUTTON_GPIO GPIO_NUM_0

// Blink timings
#define BLINK_MS  300
#define OFF_MS 1000

static void blink_task(void *arg)
{
    (void)arg;
    while (1) {
        for(int i = 0; i < FW_VERSION; i++) {
            // Blink number of times equal to FW_VERSION
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(BLINK_MS));
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(BLINK_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(OFF_MS));
    }
}

static void button_task(void *arg)
{
    (void)arg;
    uint8_t last_level = gpio_get_level(BUTTON_GPIO);

    while (1) {
        uint8_t level = gpio_get_level(BUTTON_GPIO);
        // detect falling edge (high -> low)
        if (level == 0 && last_level == 1) {
            // debounce
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Button pressed -> starting OTA task");

                // Start OTA task
                xSemaphoreGive(startOTASemaphore);

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

void App__Init(void) {
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

