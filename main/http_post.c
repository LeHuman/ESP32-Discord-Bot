#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_http_client.h"

#define HTTP_MAX_BUFFER CONFIG_HTTP_MAX_BUFFER
#define HTTP_HOST CONFIG_HTTP_HOST
#define HTTP_MAX_QUEUE CONFIG_WEBSOCKET_QUEUE_SIZE

static const char HTTP_TAG[] = "HTTP";
static const char *authHeader;
static char local_response_buffer[HTTP_MAX_BUFFER] = {0};
static QueueHandle_t HTTP_POST_Queue;

typedef struct http_post_data {
    char *jsonContent;
    char *path;
} http_post_data_t;

static inline void clean_post_data(http_post_data_t *postData) {
    free(postData->jsonContent);
    free(postData->path);
}

void http_rest_post_task(void *pvParameters) {
    for (;;) {
        ESP_LOGI(HTTP_TAG, "Waiting for queue");

        http_post_data_t postData;
        xQueueReceive(HTTP_POST_Queue, &postData, portMAX_DELAY); // Wait for new message in queue
        ESP_LOGI(HTTP_TAG, "Payload received");

        esp_http_client_config_t config = {
            .host = HTTP_HOST,
            .path = postData.path,
            .user_data = local_response_buffer, // Pass address of local buffer to get response
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        ESP_LOGI(HTTP_TAG, "Delaying message");
        vTaskDelay(pdMS_TO_TICKS(550));

        // POST
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", authHeader);
        esp_http_client_set_post_field(client, postData.jsonContent, strlen(postData.jsonContent));
        ESP_LOGI(HTTP_TAG, "Waiting for HTTP Client");
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int len = esp_http_client_get_content_length(client);
            ESP_LOGI(HTTP_TAG, "HTTP POST Status = %d, content_length = %d",
                     esp_http_client_get_status_code(client),
                     len);
            if (len > 0) {
                char *response = malloc(len);
                esp_http_client_read_response(client, response, len);
                ESP_LOGI(HTTP_TAG, "Received=%.*s", len, response);
                free(response);
            }
        } else {
            ESP_LOGE(HTTP_TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        clean_post_data(&postData);
    }
    vTaskDelete(NULL);
}

extern void http_queue_message(http_post_data_t *postData) {
    ESP_LOGI(HTTP_TAG, "Queuing message: %s", postData->jsonContent);
    xQueueSendToBack(HTTP_POST_Queue, postData, 0);
}

extern esp_err_t http_init(const char *authHeaderStr) {
    authHeader = authHeaderStr;
    ESP_LOGI(HTTP_TAG, "Creating HTTP POST Queue");
    HTTP_POST_Queue = xQueueCreate(HTTP_MAX_QUEUE, sizeof(struct http_post_data)); // strings should be allocated then freed
    if (HTTP_POST_Queue == NULL) {
        ESP_LOGE(HTTP_TAG, "Failed to create queue");
        return ESP_FAIL;
    }
    if (xTaskCreate(http_rest_post_task, "HTTP POST", 4096, NULL, 16, NULL) != pdPASS) {
        ESP_LOGE(HTTP_TAG, "Failed to start HTTP task");
        return ESP_FAIL;
    }

    return ESP_OK;
}