#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#ifdef CONFIG_BLINK_ENABLE
#include "blink.c"
#endif

#define NO_DATA_TIMEOUT_SEC CONFIG_WEBSOCKET_TIMEOUT_SEC // TODO: implement websocket timeout
#define WEBSOCKET_BUFFER_SIZE CONFIG_WEBSOCKET_BUFFER_SIZE
#define WEBSOCKET_URI CONFIG_WEBSOCKET_URI
#define MAX_MESSAGE_QUEUE CONFIG_WEBSOCKET_QUEUE_SIZE

static const char WS_TAG[] = "WebSocket";

static esp_websocket_client_handle_t client;
static QueueHandle_t message_queue;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
#ifdef CONFIG_BLINK_ENABLE
        blink_mult(3);
#endif
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
#ifdef CONFIG_BLINK_ENABLE
        blink_mult(2);
#endif
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
#ifdef CONFIG_BLINK_ENABLE
        blink();
#endif
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DATA");
        if (data->data_len > 0) {
            char *msg = calloc(1, (data->data_len) + 1);
            snprintf(msg, data->data_len, data->data_ptr);
            if (xQueueSendToBack(message_queue, msg, 0) == errQUEUE_FULL) {
                ESP_LOGE(WS_TAG, "Message queue is full, unable to receive last message");
            }
            free(msg);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

extern void websocket_send_text(char *data) {
    if (esp_websocket_client_is_connected(client)) {
        ESP_LOGI(WS_TAG, "Sending %s", data);
        esp_websocket_client_send_text(client, data, strlen(data), portMAX_DELAY);
    }
}

extern esp_err_t websocket_app_start(void) {
    esp_websocket_client_config_t websocket_cfg = {
        .disable_auto_reconnect = true, // Must implement this with discord API
        .uri = WEBSOCKET_URI,
        .buffer_size = WEBSOCKET_BUFFER_SIZE,
    };

    ESP_LOGI(WS_TAG, "Connecting to %s...", websocket_cfg.uri);
    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    return esp_websocket_client_start(client);
}

extern void websocket_app_stop(void) {
    esp_websocket_client_stop(client);
    ESP_LOGI(WS_TAG, "Websocket Stopped");
    esp_websocket_client_destroy(client);
}

extern QueueHandle_t websocket_init(void) {
    message_queue = xQueueCreate(MAX_MESSAGE_QUEUE, WEBSOCKET_BUFFER_SIZE + 1);
    if (message_queue == NULL) {
        ESP_LOGE(WS_TAG, "Unable to create message queue");
    }
    return message_queue;
}