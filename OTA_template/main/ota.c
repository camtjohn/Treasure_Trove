#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"

#include "ota.h"

static const char *TAG = "ota";

// OTA URL (change as needed). Example: https://jbar.dev/firmware.bin
#define OTA_URL "https://192.168.0.112/firmware.bin"
// #define OTA_URL "https://jbar.dev/firmware.bin"

// Symbols created by EMBED_TXTFILES for TLS_Keys/ca.crt in main/CMakeLists.txt
// The linker symbols are named like: _binary_<path_with_underscores>_start/_end
extern const uint8_t _binary_ca_crt_start[];
extern const uint8_t _binary_ca_crt_end[];

// Semaphore for triggering OTA from external modules
SemaphoreHandle_t startOTASemaphore = NULL;               

// Thread variables
TaskHandle_t blockingTaskHandle_OTA = NULL;
void blocking_thread_start_OTA(void *);

// Private method prototypes
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
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
    case HTTP_EVENT_ON_HEADERS_COMPLETE:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
        break;
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
    ESP_LOGI(TAG, "Starting OTA");

    // Log free heap to help diagnose allocation failures during OTA
    size_t free_heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap before OTA: %u bytes", (unsigned)free_heap_before);

    char* cert_pem = get_pem_from_cert();

    // Reduce buffer size to lower heap usage inside the http client / TLS stack.
    // Default buffer is often 8k; using 2k or 4k reduces peak RAM needed.
    esp_http_client_config_t my_http_config = {
        .url = OTA_URL,
        .cert_pem = cert_pem,
        .keep_alive_enable = false,                         // disable keep-alive to free connection resources
        .buffer_size = 2048,
        .skip_cert_common_name_check = true,                // for local IP address server
        .event_handler = _http_event_handler,               // optional, for debug logging
        .tls_dyn_buf_strategy = HTTP_TLS_DYN_BUF_RX_STATIC, // controls TLS buffer allocation
    };

    // this line is added from simple OTA:
    esp_https_ota_config_t ota_config = {
        .http_config = &my_http_config,
    };
    ESP_LOGI(TAG, "Attempting to download update from %s", my_http_config.url);
    esp_err_t ret = esp_https_ota(&ota_config);
    

    if (cert_pem) {
        free(cert_pem);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed. err=0x%x", ret);
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
