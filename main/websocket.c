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

#include "blink.c"
#include "wifi_interface.c"

#define NO_DATA_TIMEOUT_SEC CONFIG_WEBSOCKET_TIMEOUT_SEC
#define BOT_TOKEN CONFIG_BOT_TOKEN

static const char *WS_TAG = "WebSocket";
static const char *PM_TAG = "Heart";
static const char *BOT_TAG = "Bot";
static const char *JSM_TAG = "JSMN";
static const char *LOGINSTR = "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"$os\":\"FreeRTOS\",\"$browser\":\"ESP32\",\"$device\":\"ESP32\"}}}";

static SemaphoreHandle_t shutdown_sema;
static esp_websocket_client_handle_t client;
jsmn_parser parser;
jsmntok_t tkns[256];

/*
    Reconnecting should attempt to resume the session
    Resetting should attempt to reidentify and start new session
*/

struct session {
    TimerHandle_t pacemaker;
    bool pacemaker_init;
    char *session_id;
    char *token;
    int seq;
    int heartbeat_int;
    int lastOP;
    bool ACK;
} session;

static void init_session() {
    session.pacemaker_init = false;
    session.ACK = false;
}

static void heartbeat(TimerHandle_t xTimer) {
    if (!session.ACK) { // confirmation of heartbeat was not received in time
        ESP_LOGE(PM_TAG, "Did not receive heartbeat confirmation in time, reconnecting");
    } else {
        ESP_LOGD(PM_TAG, "ba");
        session.ACK = false; // Expecting ACK to return and set to true before next heartbeat
    }
    xTimerChangePeriod(session.pacemaker, pdMS_TO_TICKS(session.heartbeat_int),0); // ensure pacemaker is up to date
    xTimerReset(session.pacemaker,0);                                              // Make callback reset the pacemaker
}

static void start_pacemaker() {
    ESP_LOGI(PM_TAG, "Starting Pacemaker");
    session.pacemaker = xTimerCreate("Bot Pacemaker", pdMS_TO_TICKS(session.heartbeat_int), pdFALSE, NULL, heartbeat);
    session.pacemaker_init = true;
}

static void set_heartbeat_int(int beat) {
    ESP_LOGI(PM_TAG, "New Heart beat: %d", beat);
    session.heartbeat_int = beat;
    session.ACK = true;
    if (!session.pacemaker_init) {
        start_pacemaker();
    }
}

static int json_equal(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start && strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

static void sendMsg(char *data) {
    if (esp_websocket_client_is_connected(client)) {
        ESP_LOGI(WS_TAG, "Sending %s", data);
        esp_websocket_client_send_text(client, data, strlen(data), portMAX_DELAY);
        vTaskDelay(1000 / portTICK_RATE_MS); // Delay a bit to prevent sending data too fast
    }
}

static void doLogin() {
    ESP_LOGI(BOT_TAG, "Logging in...");
    char buffer[160];
    snprintf(buffer, 160, LOGINSTR, BOT_TOKEN);
    sendMsg(buffer);
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

        int r = jsmn_parse(&parser, data->data_ptr, data->data_len, tkns, 256);

        if (r < 0) {
            ESP_LOGE(JSM_TAG, "Failed to parse JSON: %d", r);
            goto CLEAN;
        }

        /* Assume the top-level element is an object */
        if (r < 1 || tkns[0].type != JSMN_OBJECT) {
            ESP_LOGE(JSM_TAG, "Object expected in JSON");
            goto CLEAN;
        }

        for (size_t i = 1; i < r; i++) {
            if (json_equal(data->data_ptr, &tkns[i], "op") == 0) {
                int op = atoi((char *)(data->data_ptr + tkns[i + 1].start));
                session.lastOP = op;
                switch (op) {
                case 0:
                    ESP_LOGD(BOT_TAG, "Received code: Dispatch");
                    break;
                case 1:
                    ESP_LOGD(BOT_TAG, "Received code: Heartbeat");
                    session.ACK = true; // ensure proper heartbeat
                    heartbeat(session.pacemaker);
                    break;
                case 7:
                    ESP_LOGD(BOT_TAG, "Received code: Reconnect");
                    break;
                case 9:
                    ESP_LOGD(BOT_TAG, "Received code: Invalid Session");
                    break;
                case 10:
                    ESP_LOGD(BOT_TAG, "Received code: Hello");
                    doLogin();
                    break;
                case 11:
                    ESP_LOGD(BOT_TAG, "Received code: Heartbeat ACK");
                    session.ACK = true;
                    ESP_LOGD(PM_TAG, "dum");
                    break;
                default:
                    ESP_LOGW(BOT_TAG, "Received code: Unknown %d", op);
                    break;
                }
                i++;
            } else if (json_equal(data->data_ptr, &tkns[i], "d") == 0) {
                int j;
                ESP_LOGD(BOT_TAG, "Reading packet data");
                if (tkns[i + 1].type != JSMN_OBJECT) {
                    continue; /* We expect data to be an object */
                }
                for (j = 0; j < tkns[i + 1].size; j++) {
                    int k = i + j + 2;
                    if (json_equal(data->data_ptr, &tkns[k], "heartbeat_interval") == 0) {
                        int beat = atoi((char *)(data->data_ptr + tkns[k + 1].start));
                        set_heartbeat_int(beat);
                        j++; // Skip token that we just read
                    } else {
                        ESP_LOGD(BOT_TAG, "Unused key: %.*s", tkns[k].end - tkns[k].start, data->data_ptr + tkns[k].start);
                    }
                }
                i += tkns[i + 1].size + 1;
            } else {
                ESP_LOGD(BOT_TAG, "Unused key: %.*s", tkns[i].end - tkns[i].start, data->data_ptr + tkns[i].start);
            }
        }

    CLEAN:
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
    init_session();

    websocket_app_start();
}
