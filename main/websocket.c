#include "esp_event.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client_mod.h"

#include "blink.c"
#include "bot.c"
#include "wifi_interface.c"

#define NO_DATA_TIMEOUT_SEC CONFIG_WEBSOCKET_TIMEOUT_SEC

static const char *WS_TAG = "WebSocket";

static SemaphoreHandle_t shutdown_sema;
static esp_websocket_client_handle_t client;

/*
    Reconnecting should attempt to resume the session
    Resetting should attempt to re-identify and start new session
*/

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
        ESP_LOGD(WS_TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
        ESP_LOGD(WS_TAG, "OP Code=%d", data->data_len, data->op_code);
        if (data->data_len > 0)
            BOT_receive_payload(data->data_ptr, data->data_len);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

static void websocket_app_start(void) {
    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.disable_auto_reconnect = true; // Must implement this with discord API
    websocket_cfg.disable_pingpong_discon = true;
    // websocket_cfg.disable_pingpong = true;
    // websocket_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;
    websocket_cfg.buffer_size = CONFIG_WEBSOCKET_BUFFER_SIZE;

    shutdown_sema = xSemaphoreCreateBinary();

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
    esp_log_level_set(BOT_TAG, ESP_LOG_DEBUG);
    esp_log_level_set(PM_TAG, ESP_LOG_DEBUG);
    esp_log_level_set(JSM_TAG, ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(attempt_connect());
    start_blink_task();

    ESP_LOGI(WS_TAG, "Initalizing Json parser");
    jsmn_init(&parser);

    ESP_LOGI(WS_TAG, "Initalizing Bot session");
    BOT_init(websocket_data_handler);

    websocket_app_start();
}
