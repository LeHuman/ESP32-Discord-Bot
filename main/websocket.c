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
static const char *LOGINSTR = "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"$os\":\"FreeRTOS\",\"$browser\":\"ESP32\",\"$device\":\"ESP32\"},\"compress\":true,\"large_threshold\":50,\"shard\":[0,1],\"presence\":{\"status\":\"online\",\"afk\":false},\"guild_subscriptions\":false,\"intents\":512}}";

static SemaphoreHandle_t shutdown_sema;
static esp_websocket_client_handle_t client;
jsmn_parser parser;
jsmntok_t tkns[256]; // IMPROVE: use dynamic token buffer

/*
    Reconnecting should attempt to resume the session
    Resetting should attempt to re-identify and start new session
*/

enum payload_event { // What event did we receive
    EVENT_NULL,
    EVENT_READY,
    EVENT_GUILD_OBJ,
}

struct session {
    TimerHandle_t pacemaker;
    payload_event event;
    bool pacemaker_init;
    char *session_id;
    char *token;
    char *seq; // format as integer, "null" otherwise
    int heartbeat_int;
    int lastOP;
    char *activeGuild;
    bool ready;
    bool ACK;
} session;

static void init_session() {
    session.pacemaker_init = false;
    session.ACK = false;
    session.activeGuild = "null";
    session.ready = false;
    session.seq = "null";
    session.event = EVENT_NULL;
}

static void BOT_heartbeat(TimerHandle_t xTimer) {
    if (!session.ACK) { // confirmation of heartbeat was not received in time
        ESP_LOGE(BOT_TAG, "Did not receive heartbeat confirmation in time, reconnecting");
    } else {
        ESP_LOGD(PM_TAG, "ba");
        session.ACK = false; // Expecting ACK to return and set to true before next heartbeat
        // TODO: send heartbeat here
    }
    xTimerChangePeriod(session.pacemaker, pdMS_TO_TICKS(session.heartbeat_int), 0); // ensure pacemaker is up to date
    xTimerReset(session.pacemaker, 0);                                              // Make callback reset the pacemaker
}

static void BOT_start_pacemaker() {
    ESP_LOGI(BOT_TAG, "Starting Pacemaker");
    session.pacemaker = xTimerCreate("Bot Pacemaker", pdMS_TO_TICKS(session.heartbeat_int), pdFALSE, NULL, BOT_heartbeat);
    session.pacemaker_init = true;
}

static void BOT_set_heartbeat_int(int beat) {
    ESP_LOGI(BOT_TAG, "New Heart beat: %d", beat);
    session.heartbeat_int = beat;
    session.ACK = true;
    if (!session.pacemaker_init) {
        BOT_start_pacemaker();
    }
}

static void BOT_set_session_id(char *new_id) {
    ESP_LOGI(BOT_TAG, "New Session ID: %s", new_id);
    session.session_id = new_id;
}

static void BOT_set_sequence(char *new_seq) {
    ESP_LOGI(BOT_TAG, "Sequence: %s", new_seq);
    session.seq = new_seq;
}

static void BOT_set_event(payload_event event) {
    session.event = event;
}

static void BOT_event(char *event) {
    ESP_LOGI(BOT_TAG, "Message event: %s", event);
    switch (event) {
    case "READY":
        BOT_set_event(EVENT_READY);
        break;
    case "GUILD_CREATE":
        BOT_set_event(EVENT_GUILD_OBJ);
        break;
    default:
        // ESP_LOGW(BOT_TAG, "Unknown Event: %s", event);
        BOT_set_event(EVENT_NULL);
        break;
    }
}

static void BOT_op_code(int op) {
    session.lastOP = op;
    switch (op) {
    case 0:
        ESP_LOGD(BOT_TAG, "Received op code: Dispatch");
        break;
    case 1:
        ESP_LOGD(BOT_TAG, "Received op code: Heartbeat");
        session.ACK = true; // ensure proper heartbeat
        BOT_heartbeat(session.pacemaker);
        break;
    case 7:
        ESP_LOGD(BOT_TAG, "Received op code: Reconnect");
        break;
    case 9:
        ESP_LOGD(BOT_TAG, "Received op code: Invalid Session");
        break;
    case 10:
        ESP_LOGD(BOT_TAG, "Received op code: Hello");
        BOT_do_login();
        break;
    case 11:
        ESP_LOGD(BOT_TAG, "Received op code: Heartbeat ACK");
        session.ACK = true;
        ESP_LOGD(PM_TAG, "dum");
        break;
    case 2, 3, 4, 6, 8: // We should only be sending these op codes
        ESP_LOGW(BOT_TAG, "Received bad op code: %d", op);
        break;
    default:
        ESP_LOGW(BOT_TAG, "Received unknown op code: %d", op);
        break;
    }
}

static bool json_equal(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start && strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return true;
    }
    return false;
}

static void BOT_send_payload(char *data) {
    if (esp_websocket_client_is_connected(client)) {
        vTaskDelay(pdMS_TO_TICKS(550)); // Wait a half second to prevent sending data too fast
        ESP_LOGI(WS_TAG, "Sending %s", data);
        esp_websocket_client_send_text(client, data, strlen(data), portMAX_DELAY);
    }
}

static void BOT_do_login() {
    ESP_LOGI(BOT_TAG, "Logging in...");
    char buffer[290];
    snprintf(buffer, 290, LOGINSTR, BOT_TOKEN);
    BOT_send_payload(buffer);
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
            if (json_equal(data->data_ptr, &tkns[k], "t")) { // Event name
                char *event = (char *)(data->data_ptr + tkns[i + 1].start);
                BOT_event(event);
                i++;                                                // Skip token that we just read
            } else if (json_equal(data->data_ptr, &tkns[k], "s")) { // Sequence
                char *new_seq = (char *)(data->data_ptr + tkns[i + 1].start);
                BOT_set_sequence(new_seq);
                i++; // Skip token that we just read
            } else if (json_equal(data->data_ptr, &tkns[i], "op")) {
                int op = atoi((char *)(data->data_ptr + tkns[i + 1].start));
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
                    switch (session.event) { // Depends on the message event being identified beforehand
                    case EVENT_GUILD_OBJ:
                        if (json_equal(data->data_ptr, &tkns[k], "name")) {
                            char *new_id = (char *)(data->data_ptr + tkns[k + 1].start);
                            BOT_set_session_id(new_id);
                            j++; // Skip token that we just read
                        }
                        break;
                    case EVENT_READY:
                        if (json_equal(data->data_ptr, &tkns[k], "session_id")) {
                            char *new_id = (char *)(data->data_ptr + tkns[k + 1].start);
                            BOT_set_session_id(new_id);
                            j++; // Skip token that we just read
                        }
                        break;
                    default:
                        if (json_equal(data->data_ptr, &tkns[k], "heartbeat_interval")) {
                            int beat = atoi((char *)(data->data_ptr + tkns[k + 1].start));
                            BOT_set_heartbeat_int(beat);
                            j++; // Skip token that we just read
                        }
                        break;
                    }
                }
                i += tkns[i + 1].size + 1; // Skip the tokens that were in the data block
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
