#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include "bot.c"
#include "websocket.c"
#include "wifi_interface.c"

static const char LOG_TAG[] = "Main";

static void websocket_data_handler(char *data) {
    websocket_send_text(data);
}

void app_main(void) {
    ESP_LOGI(LOG_TAG, "Startup..");
    ESP_LOGI(LOG_TAG, "Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(LOG_TAG, "IDF version: %s", esp_get_idf_version());

    // BLINK
#ifdef CONFIG_BLINK_ENABLE
    ESP_LOGI(LOG_TAG, "Starting Blink Task");
    start_blink_task();
#endif

    // WIFI
    ESP_LOGI(LOG_TAG, "Attempting Wifi connection");
    ESP_ERROR_CHECK(attempt_connect());

    // WEBSOCKET INIT
    ESP_LOGI(LOG_TAG, "Initalizing Websocket");
    QueueHandle_t message_queue = websocket_init(); // Get message queue from websocket
    if (message_queue == NULL) {
        ESP_LOGE(LOG_TAG, "Websocket failed to initialize, aborting");
        abort();
    }
    ESP_ERROR_CHECK(esp_register_shutdown_handler(websocket_app_stop));

    // BOT
    ESP_LOGI(LOG_TAG, "Starting Bot session");
    ESP_ERROR_CHECK(BOT_init(websocket_data_handler, message_queue));

    // WEBSOCKET START
    ESP_LOGI(LOG_TAG, "Starting Websocket");
    ESP_ERROR_CHECK(websocket_app_start());

    ESP_LOGI(LOG_TAG, "Free memory: %d bytes", esp_get_free_heap_size());
}
