#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"

#include "ota.h"
#include "wifi.h"
#include <inttypes.h>

static const char *TAG = "ota";

#define OTA_URL "https://192.168.0.112/firmware.bin"
// #define OTA_URL "https://jbar.dev/firmware.bin"

extern const uint8_t _binary_ca_crt_start[];
extern const uint8_t _binary_ca_crt_end[];

// Semaphore for triggering OTA from external modules
SemaphoreHandle_t startOTASemaphore = NULL;               

// Thread variables
TaskHandle_t blockingTaskHandle_OTA = NULL;

// Private method prototypes
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static void download_image(void);
static char* get_pem_from_cert(void);
static void blocking_thread_start_OTA(void *pvParameters);

// Private method definitions

// Good for Debug, not necessary for functionality
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    // case HTTP_EVENT_ON_HEADERS_COMPLETE:
    //     ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
    //     break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    default:
        break;
    }
    return ESP_OK;
}

char* get_pem_from_cert(void) {
    char* ret_val = NULL;
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
            ret_val = cert_pem;
        } else {
            ESP_LOGW(TAG, "Failed to allocate memory for cert, continuing without cert verification");
        }
    } else if (cert_len != 0) {
        ESP_LOGW(TAG, "Unexpected cert size %u, skipping cert", (unsigned)cert_len);
    } else {
        ESP_LOGW(TAG, "No embedded CA certificate found, skipping cert verification");
    }

    return(ret_val);
}

 void download_image(void) {
    ESP_LOGI(TAG, "Starting OTA download");

    // Wait for Wi-Fi to be connected before attempting OTA
    EventGroupHandle_t wifi_event_group = Wifi__GetEventGroup();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Wi-Fi event group not initialized");
        return;
    }

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(30000)); // 30 second timeout

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi not connected (bits=0x%lx). OTA requires Wi-Fi.", (unsigned long)bits);
        ESP_LOGE(TAG, "Check Wi-Fi logs above for connection details");
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi connected ✓");
    ESP_LOGI(TAG, "Proceeding with OTA from: %s", OTA_URL);

    // Log free heap to help diagnose allocation failures during OTA
    size_t free_heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap before OTA: %lu bytes", (unsigned long)free_heap_before);

    char* cert_pem = get_pem_from_cert();
    if (cert_pem) {
        ESP_LOGI(TAG, "Using embedded CA certificate for TLS verification");
    } else {
        ESP_LOGW(TAG, "No CA certificate available - cert verification disabled");
    }

    // Buffer size is critical for OTA stability:
    // - Too small (2KB) can cause incomplete reads and checksum failures
    // - Too large wastes heap memory
    // Using 4KB as a middle ground for reliable operation
    esp_http_client_config_t my_http_config = {
        .url = OTA_URL,
        .cert_pem = cert_pem,
        .keep_alive_enable = false,                         // disable keep-alive to free connection resources
        .buffer_size = 4096,                                // 4KB buffer for better transfer reliability
        .skip_cert_common_name_check = true,                // for jbar.dev with self-signed cert
        .event_handler = _http_event_handler,               // optional, for debug logging
        .timeout_ms = 30000,                                // 30 second timeout for large firmware transfers
    };

    // this line is added from simple OTA:
    esp_https_ota_config_t ota_config = {
        .http_config = &my_http_config,
        .bulk_flash_erase = false,  // Erase as we go instead of all at once (saves RAM)
    };
    ESP_LOGI(TAG, "Connecting to server...");

    // Verify that an OTA update partition exists before starting
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found (esp_ota_get_next_update_partition returned NULL)");
        // Print available app partitions to help debugging
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
        if (it == NULL) {
            ESP_LOGE(TAG, "No application partitions found at all");
        } else {
            ESP_LOGI(TAG, "Application partitions present:");
            do {
                const esp_partition_t *p = esp_partition_get(it);
                if (p) {
                    /* Use PRI macros for portable formatting of uint32_t fields */
                    ESP_LOGI(TAG, "  label=%s type=%d subtype=%d addr=0x%08" PRIx32 " size=0x%08" PRIx32,
                             p->label ? p->label : "<none>", p->type, p->subtype,
                             p->address, p->size);
                }
                it = esp_partition_next(it);
            } while (it != NULL);
            esp_partition_iterator_release(it);
        }
        if (cert_pem) free(cert_pem);
        return;
    }

    ESP_LOGI(TAG, "Target partition: offset=0x%08" PRIx32 ", size=0x%08" PRIx32, next->address, next->size);

    esp_err_t ret = esp_https_ota(&ota_config);
    

    if (cert_pem) {
        free(cert_pem);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed with error code 0x%x", ret);
        
        // Provide context on what might have gone wrong
        const char *error_hint = "";
        if (ret == 0xFFFFFFFF) {
            // Common error for header/connection issues
            error_hint = "  → Likely: Server unreachable, timeout, or certificate mismatch";
        }
        
        ESP_LOGE(TAG, "Troubleshooting steps:");
        ESP_LOGE(TAG, "  1. Verify server at %s is running and accessible", OTA_URL);
        ESP_LOGE(TAG, "  2. Check if certificate matches (for HTTPS): use 'openssl s_client -connect <host>'");
        ESP_LOGE(TAG, "  3. Try HTTP instead (dev-only, insecure): change #define OTA_URL to http://...");
        ESP_LOGE(TAG, "  4. Verify Wi-Fi is connected: check logs for WIFI_CONNECTED_BIT");
        ESP_LOGE(TAG, "  5. Current free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
        if (error_hint[0]) {
            ESP_LOGE(TAG, "%s", error_hint);
        }
    }
}

void blocking_thread_start_OTA(void *pvParameters) {
    while(1) {
        // Wait for semaphore from button press
        if(xSemaphoreTake(startOTASemaphore, portMAX_DELAY) == pdTRUE) {
            // Start OTA process: get image from jbar.dev
            download_image();
        }
    }
}

void OTA__Init(void) {
    ESP_LOGI(TAG, "OTA Init start");

    startOTASemaphore = xSemaphoreCreateBinary();
    if (startOTASemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA semaphore");
        return;
    }

    xTaskCreate(
        blocking_thread_start_OTA,       // Task function
        "BlockingTask_StartOTA",       // Task name (for debugging)
        8192,   // Stack size (words)
        NULL,                           // Task parameter
        8,                             // Task priority
        &blockingTaskHandle_OTA       // Task handle
    );
}
