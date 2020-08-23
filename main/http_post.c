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

#define HTTP_MAX_BUFFER 2048 // TODO: add defines to Kconfig
#define HTTP_HOST "discordapp.com"
#define HTTP_MAX_QUEUE CONFIG_WEBSOCKET_QUEUE_SIZE

static const char *HTTP_TAG = "HTTP_CLIENT";
static const char *MSG_STR = "{\"content\":\"%s\",\"tts\":false,\"embed\":{\"title\":\"%s\",\"description\":\"%s\"}}";
static const char *authHeader;
static char local_response_buffer[HTTP_MAX_BUFFER] = {0};
static QueueHandle_t HTTP_POST_Queue;

typedef struct http_post_data {
    char *jsonContent;
    char *path;
} http_post_data_t;

/*esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    static char *output_buffer; // Buffer to store response of http request from event handler
    static int output_len;      // Stores number of bytes read
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(HTTP_TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);

        // Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
        // However, event handler can also be used in case chunked encoding is used.
        if (!esp_http_client_is_chunked_response(evt->client)) {
            // If user_data buffer is configured, copy the response into the buffer
            if (evt->user_data) {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            } else {
                if (output_buffer == NULL) {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL) {
                        ESP_LOGE(HTTP_TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL) {
            // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
            // ESP_LOG_BUFFER_HEX(HTTP_TAG, output_buffer, output_len);
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(HTTP_TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            ESP_LOGI(HTTP_TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(HTTP_TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        break;
    }
    return ESP_OK;
}
*/

inline static void clean_post_data(http_post_data_t *postData) {
    free(postData->jsonContent);
    free(postData->path);
}

void http_rest_post_task(void *pvParameters) {
    for (;;) {
        ESP_LOGI(HTTP_TAG, "Waiting for queue");

        http_post_data_t postData;
        xQueueReceive(HTTP_POST_Queue, &postData, portMAX_DELAY); // Wait for new message in queue

        esp_http_client_config_t config = {
            .host = HTTP_HOST,
            .path = postData.path,
            // .event_handler = _http_event_handler, // IMPROVE: Integration of http handler
            .user_data = local_response_buffer, // Pass address of local buffer to get response
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        // POST
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", authHeader);
        esp_http_client_set_post_field(client, postData.jsonContent, strlen(postData.jsonContent));
        ESP_LOGI(HTTP_TAG, "Waiting for HTTP Client");
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(HTTP_TAG, "HTTP POST Status = %d, content_length = %d",
                     esp_http_client_get_status_code(client),
                     esp_http_client_get_content_length(client));
        } else {
            ESP_LOGE(HTTP_TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        clean_post_data(&postData);
    }
    vTaskDelete(NULL);
}

extern QueueHandle_t http_init(const char *authHeaderStr) {
    authHeader = authHeaderStr;
    ESP_LOGI(HTTP_TAG, "Starting HTTP POST Task");
    HTTP_POST_Queue = xQueueCreate(HTTP_MAX_QUEUE, sizeof(struct http_post_data)); // strings should be allocated then freed
    xTaskCreate(http_rest_post_task, "HTTP POST", 4096, NULL, 16, NULL);
    return HTTP_POST_Queue;
}