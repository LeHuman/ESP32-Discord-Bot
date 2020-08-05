/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

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
#include "esp_websocket_client.h"
#include "jsmn.h"

#include "wifi_interface.c"

jsmn_parser parser;
jsmntok_t tokens[25];

#define NO_DATA_TIMEOUT_SEC CONFIG_WEBSOCKET_TIMEOUT_SEC
#define BOT_TOKEN "NzAwODA1NjA5MjM4NTYwOTE5.Xp38Pg.CsK47JNMss9dOA3Cwo8AzYQL4D8"

static const char *WS_TAG = "WebSocket";
static const char *LOGINSTR = "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"$os\":\"FreeRTOS\",\"$browser\":\"ESP32\",\"$device\":\"ESP32\"}}}";

static TimerHandle_t shutdown_signal_timer;
static SemaphoreHandle_t shutdown_sema;
static esp_websocket_client_handle_t client;
static bool web_connected = false;

static void shutdown_signaler(TimerHandle_t xTimer) {
    ESP_LOGI(WS_TAG, "No data received for %d seconds, signaling shutdown", NO_DATA_TIMEOUT_SEC);
    xSemaphoreGive(shutdown_sema);
}

#if CONFIG_WEBSOCKET_URI_FROM_STDIN
static void get_string(char *line, size_t size) {
    int count = 0;
    while (count < size) {
        int c = fgetc(stdin);
        if (c == '\n') {
            line[count] = '\0';
            break;
        } else if (c > 0 && c < 127) {
            line[count] = c;
            ++count;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

#endif /* CONFIG_WEBSOCKET_URI_FROM_STDIN */

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

// static void sendMsg(char *data) {
//     ESP_LOGI(WS_TAG, "DATA TO BE SENT: $%.*s", (char *)data);
//     esp_websocket_client_send_text(client, data, strlen(data), portMAX_DELAY);
//     vTaskDelay(1000 / portTICK_RATE_MS * 5);
// }

// static void doLogin() {
//     ESP_LOGI(WS_TAG, "Logging in...");
//     char buffer[160];
//     snprintf(buffer, 160, LOGINSTR, BOT_TOKEN);
//     sendMsg(buffer);
// }

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_CONNECTED");
        web_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        web_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(WS_TAG, "Received opcode=%d", data->op_code);
        ESP_LOGW(WS_TAG, "Received=%.*s", data->data_len, (char *)data->data_ptr);
        ESP_LOGW(WS_TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

        // char *data_str = data->data_ptr;

        // int r = jsmn_parse(&parser, data_str, strlen(data_str), tokens, 25);
        // if (r < 0) {
        //     ESP_LOGI(WS_TAG, "Failed to parse JSON: %d", r);
        // } else {
        //     for (size_t i = 0; i < r; i++) {
        //         jsmntok_t *t = &tokens[i];

        //         if (jsoneq(data_str, &t[i], "op") == 10) {
        //             // doLogin();
        //             ESP_LOGI(WS_TAG, "Hello op found!");
        //         }
        //     }
        // }

        xTimerReset(shutdown_signal_timer, portMAX_DELAY);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(WS_TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    }
}

static void websocket_app_start(void) {
    esp_websocket_client_config_t websocket_cfg = {
        bool disable_auto_reconnect = true,
        bool disable_pingpong_discon = true,
    };

    shutdown_signal_timer = xTimerCreate("Websocket shutdown timer", NO_DATA_TIMEOUT_SEC * 1000 / portTICK_PERIOD_MS, pdFALSE, NULL, shutdown_signaler);
    shutdown_sema = xSemaphoreCreateBinary();

#if CONFIG_WEBSOCKET_URI_FROM_STDIN
    char line[128];

    ESP_LOGI(WS_TAG, "Please enter uri of websocket endpoint");
    get_string(line, sizeof(line));

    websocket_cfg.uri = line;
    ESP_LOGI(WS_TAG, "Endpoint uri: %s\n", line);

#else
    websocket_cfg.uri = CONFIG_WEBSOCKET_URI;

#endif /* CONFIG_WEBSOCKET_URI_FROM_STDIN */

    ESP_LOGI(WS_TAG, "Connecting to %s...", websocket_cfg.uri);

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
    xTimerStart(shutdown_signal_timer, portMAX_DELAY);

    vTaskDelay(1000 / portTICK_RATE_MS * 2);
    // while (web_connected) {
    //     vTaskDelay(1000 / portTICK_RATE_MS);
    // }

    // char data[32];
    // int i = 0;
    // while (i < 10) {
    //     if (esp_websocket_client_is_connected(client)) {
    //         int len = sprintf(data, "hello %04d", i++);
    //         ESP_LOGI(WS_TAG, "Sending %s", data);
    //         esp_websocket_client_send_text(client, data, len, portMAX_DELAY);
    //     }
    //     vTaskDelay(1000 / portTICK_RATE_MS);
    // }

    xSemaphoreTake(shutdown_sema, portMAX_DELAY);
    esp_websocket_client_stop(client);
    ESP_LOGI(WS_TAG, "Websocket Stopped");
    esp_websocket_client_destroy(client);
}

void app_main(void) {
    ESP_LOGI(WS_TAG, "[APP] Startup..");
    ESP_LOGI(WS_TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(WS_TAG, "[APP] IDF version: %s", esp_get_idf_version());
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);
    esp_log_level_set("TRANS_TCP", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(attempt_connect());

    ESP_LOGI(WS_TAG, "Initalizing Json parser");
    jsmn_init(&parser);

    websocket_app_start();
}
