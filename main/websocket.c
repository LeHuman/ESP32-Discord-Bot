#include "esp_event.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

#include "blink.c"
#include "bot.c"
#include "wifi_interface.c"

#define NO_DATA_TIMEOUT_SEC CONFIG_WEBSOCKET_TIMEOUT_SEC
#define WEBSOCKET_BUFFER_SIZE CONFIG_WEBSOCKET_BUFFER_SIZE
#define MESSAGE_LENGTH_SIZE sizeof(int)
#define MAX_MESSAGE_QUEUE 5

static const char *WS_TAG = "WebSocket";

static esp_websocket_client_handle_t client;
static QueueHandle_t message_queue;
static QueueHandle_t message_length_queue;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        blink_mult(3);
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        blink_mult(2);
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        blink();
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DATA");
        if (data->data_len > 0) {
            if (xQueueSendToBack(message_length_queue, &data->data_len, 0) == errQUEUE_FULL) {
                ESP_LOGE(WS_TAG, "length queue is full, unable to receive last message length");
            } else if (xQueueSendToBack(message_queue, data->data_ptr, 0) == errQUEUE_FULL) {
                ESP_LOGE(WS_TAG, "Message queue is full, unable to receive last message"); // Should never be called?
                // TODO: remove last queued length
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

static void websocket_app_start(void) {
    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.disable_auto_reconnect = true; // Must implement this with discord API
    // websocket_cfg.disable_pingpong_discon = true;
    // websocket_cfg.disable_pingpong = true;
    // websocket_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;
    // websocket_cfg.buffer_size = WEBSOCKET_BUFFER_SIZE;

    ESP_LOGI(WS_TAG, "Connecting to %s...", websocket_cfg.uri);

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
}

// static void websocket_app_stop(void) {
//     xSemaphoreTake(shutdown_sema, portMAX_DELAY);
//     esp_websocket_client_stop(client);
//     ESP_LOGI(WS_TAG, "Websocket Stopped");
//     esp_websocket_client_destroy(client);
// }

static void websocket_data_handler(char *data) {
    if (esp_websocket_client_is_connected(client)) {
        ESP_LOGI(WS_TAG, "Sending %s", data);
        esp_websocket_client_send_text(client, data, strlen(data), portMAX_DELAY);
    }
}

void app_main(void) {
    ESP_LOGI(WS_TAG, "Startup..");
    ESP_LOGI(WS_TAG, "Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(WS_TAG, "IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);
    esp_log_level_set("TRANS_TCP", ESP_LOG_DEBUG);
    esp_log_level_set(WS_TAG, ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(attempt_connect());
    start_blink_task();

    message_queue = xQueueCreate(MAX_MESSAGE_QUEUE, WEBSOCKET_BUFFER_SIZE);
    message_length_queue = xQueueCreate(MAX_MESSAGE_QUEUE, MESSAGE_LENGTH_SIZE);
    if (message_queue == NULL || message_length_queue == NULL) {
        ESP_LOGE(WS_TAG, "Unable to create message queue");
        return;
    }

    ESP_LOGI(WS_TAG, "Initalizing Bot session");
    BOT_init(websocket_data_handler, message_queue, message_length_queue);

    ESP_LOGI(WS_TAG, "Initalizing Websocket");
    websocket_app_start();

    ESP_LOGI(WS_TAG, "Free memory: %d bytes", esp_get_free_heap_size());
}
