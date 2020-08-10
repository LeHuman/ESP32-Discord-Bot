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

#include "blink.c"
#include "bot.c"
#include "wifi_interface.c"

#define NO_DATA_TIMEOUT_SEC CONFIG_WEBSOCKET_TIMEOUT_SEC

static const char *WS_TAG = "WebSocket";

static SemaphoreHandle_t shutdown_sema;
static esp_websocket_client_handle_t client;

static const char *JSM_TAG = "JSMN";

jsmn_parser parser;
jsmntok_t tkns[128]; // IMPROVE: use dynamic token buffer

static char *json_ttoa(const char *data, jsmntok_t *tok) {
    // (char *)(data->data_ptr + tkns[i + 1].start);
    int len = tok->end - tok->start + 1;
    char *str = malloc(len * sizeof(char));
    snprintf(str, len, (char *)(data + tok->start));
    // strncpy(str, data, len);
    // str[len - 1] = '\0';
    ESP_LOGI(JSM_TAG, "Parsed Token: %s", str);
    return str;
}

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
        ESP_LOGD(WS_TAG, "OP Code=%d", data->op_code);
        if (data->data_len > 0) {

            int r = jsmn_parse(&parser, data->data_ptr, data->data_len, tkns, 128);

            if (r < 0) {
                ESP_LOGE(JSM_TAG, "Failed to parse JSON: %d", r);
                return;
            }

            /* Assume the top-level element is an object */
            if (r < 1 || tkns[0].type != JSMN_OBJECT) {
                ESP_LOGE(JSM_TAG, "Object expected in JSON");
                return;
            }

            for (size_t i = 1; i < r; i++) {
                if (json_equal(data->data_ptr, &tkns[i], "t")) { // Event name
                    // char *event = (char *)(data + tkns[i + 1].start);
                    char *event = json_ttoa(data->data_ptr, &tkns[i + 1]);
                    BOT_event(event);
                    i++;                                                // Skip token that we just read
                } else if (json_equal(data->data_ptr, &tkns[i], "s")) { // Sequence
                    char *new_seq = json_ttoa(data->data_ptr, &tkns[i + 1]);
                    BOT_set_sequence(new_seq);
                    i++; // Skip token that we just read
                } else if (json_equal(data->data_ptr, &tkns[i], "op")) {
                    char *opStr = json_ttoa(data->data_ptr, &tkns[i + 1]);
                    int op = atoi(opStr);
                    free(opStr);
                    BOT_op_code(op);
                    i++; // Skip token that we just read
                } else if (json_equal(data->data_ptr, &tkns[i], "d")) {
                    int j;
                    ESP_LOGD(BOT_TAG, "Reading packet data");
                    if (tkns[i + 1].type != JSMN_OBJECT) {
                        continue; /* We expect data to be an object */
                    }
                    for (j = 0; j < tkns[i + 1].size; j++) {
                        int k = i + j + 2;
                        switch (BOT.event) { // Depends on the message event being identified beforehand
                        case EVENT_GUILD_OBJ:
                            if (json_equal(data->data_ptr, &tkns[k], "name")) {
                                char *name = json_ttoa(data->data_ptr, &tkns[k + 1]);
                                ESP_LOGI(BOT_TAG, "Guild name: %s", name);
                                free(name);
                                // BOT_set_session_id(new_id);
                                j++; // Skip token that we just read
                            }
                            break;
                        case EVENT_READY:
                            if (json_equal(data->data_ptr, &tkns[k], "session_id")) {
                                char *new_id = json_ttoa(data->data_ptr, &tkns[k + 1]);
                                BOT_set_session_id(new_id);
                                j++; // Skip token that we just read
                            }
                            break;
                        default:
                            if (json_equal(data->data_ptr, &tkns[k], "heartbeat_interval")) {
                                char *beatStr = json_ttoa(data->data_ptr, &tkns[k + 1]);
                                int beat = atoi(beatStr);
                                free(beatStr);
                                BOT_set_heartbeat_int(beat);
                                j++; // Skip token that we just read
                            }
                            break;
                        }
                    }
                    i += tkns[i + 1].size + 1; // Skip the tokens that were in the data block
                    // } else {
                    // ESP_LOGD(BOT_TAG, "Unused key: %.*s", tkns[i].end - tkns[i].start, data + tkns[i].start);
                }
            }

            // BOT_receive_payload(data->data_ptr, data->data_len);
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
