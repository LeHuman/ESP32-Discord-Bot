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

#include "jsmn.h"

#define BOT_TOKEN CONFIG_BOT_TOKEN

typedef void (*BOT_payload_handler)(char *); // function that will send the bot payloads

static const char *LOGIN_STR = "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"$os\":\"FreeRTOS\",\"$browser\":\"ESP32\",\"$device\":\"ESP32\"},\"compress\":true,\"large_threshold\":50,\"shard\":[0,1],\"presence\":{\"status\":\"online\",\"afk\":false},\"guild_subscriptions\":true,\"intents\":512}}";
static const char *HB_STR = "{\"op\": 1,\"d\": \"%s\"}";
static const char *BOT_TAG = "Bot";
static const char *PM_TAG = "Heart";
static const char *JSM_TAG = "JSMN";

jsmn_parser parser;
jsmntok_t tkns[256]; // IMPROVE: use dynamic token buffer

static bool json_equal(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start && strncmp(json + tok->start, s, tok->end - tok->start) == 0 && strncmp(json + tok->start, "null", tok->end - tok->start) != 0) {
        return true;
    }
    return false;
}

enum payload_event { // What event did we receive
    EVENT_NULL,
    EVENT_READY,
    EVENT_GUILD_OBJ,
};
typedef enum payload_event payload_event;

struct BOT { // TODO: reconnect bot every now and then to reset sequence number to avoid huge seq numbers
    TimerHandle_t pacemaker;
    payload_event event;
    BOT_payload_handler payload_handle;
    bool pacemaker_init;
    char *session_id;
    char *token;
    char *seq; // format as integer, "null" otherwise
    int heartbeat_int;
    int lastOP;
    char *activeGuild;
    bool ready;
    bool ACK;
} BOT;

static void BOT_init(BOT_payload_handler payload_handle) {
    BOT.payload_handle = payload_handle;
    BOT.pacemaker_init = false;
    BOT.ACK = false;
    BOT.activeGuild = "null";
    BOT.ready = false;
    BOT.seq = "null";
    BOT.event = EVENT_NULL;
}

static void BOT_set_session_id(char *new_id) {
    ESP_LOGI(BOT_TAG, "New Session ID: %s", new_id);
    BOT.session_id = new_id;
}

static void BOT_set_sequence(char *new_seq) {
    ESP_LOGI(BOT_TAG, "Sequence: %s", new_seq);
    BOT.seq = new_seq;
}

static void BOT_set_event(payload_event event) {
    BOT.event = event;
}

static void BOT_send_payload(char *data) {
    vTaskDelay(pdMS_TO_TICKS(550)); // Wait a half second to prevent sending data too fast
    BOT.payload_handle(data);
}

static void BOT_heartbeat(TimerHandle_t xTimer) {
    if (!BOT.ACK) { // confirmation of heartbeat was not received in time
        ESP_LOGE(BOT_TAG, "Did not receive heartbeat confirmation in time, reconnecting");
    } else {
        ESP_LOGD(PM_TAG, "ba");
        BOT.ACK = false; // Expecting ACK to return and set to true before next heartbeat
        char buffer[128];
        snprintf(buffer, 128, HB_STR, BOT.seq);
        BOT_send_payload(buffer);
    }
    xTimerChangePeriod(BOT.pacemaker, pdMS_TO_TICKS(BOT.heartbeat_int), 0); // ensure pacemaker is up to date
    xTimerReset(BOT.pacemaker, 0);                                          // Make callback reset the pacemaker
}

static void BOT_start_pacemaker() {
    ESP_LOGI(BOT_TAG, "Starting Pacemaker");
    BOT.pacemaker = xTimerCreate("Bot Pacemaker", pdMS_TO_TICKS(BOT.heartbeat_int), pdFALSE, NULL, BOT_heartbeat);
    BOT.pacemaker_init = true;
}

static void BOT_set_heartbeat_int(int beat) {
    ESP_LOGI(BOT_TAG, "New Heart beat: %d", beat);
    BOT.heartbeat_int = beat;
    BOT.ACK = true;
    if (!BOT.pacemaker_init) {
        BOT_start_pacemaker();
    }
}

static void BOT_do_login() {
    ESP_LOGI(BOT_TAG, "Logging in...");
    char buffer[480];
    snprintf(buffer, 480, LOGIN_STR, BOT_TOKEN);
    BOT_send_payload(buffer);
}

static void BOT_event(char *event) {
    ESP_LOGI(BOT_TAG, "Message event: %s", event);

    if (strcmp(event, "READY") == 0) {
        BOT_set_event(EVENT_READY);
    } else if (strcmp(event, "GUILD_CREATE") == 0) {
        BOT_set_event(EVENT_GUILD_OBJ);
    } else {
        BOT_set_event(EVENT_NULL);
    }
}

static void BOT_op_code(int op) {
    BOT.lastOP = op;
    switch (op) {
    case 0:
        ESP_LOGD(BOT_TAG, "Received op code: Dispatch");
        break;
    case 1:
        ESP_LOGD(BOT_TAG, "Received op code: Heartbeat");
        BOT.ACK = true; // ensure proper heartbeat
        BOT_heartbeat(BOT.pacemaker);
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
        BOT.ACK = true;
        ESP_LOGD(PM_TAG, "dum");
        break;
    case 2: // We should only be sending these op codes
    case 3:
    case 4:
    case 6:
    case 8:
        ESP_LOGW(BOT_TAG, "Received bad op code: %d", op);
        break;
    default:
        ESP_LOGW(BOT_TAG, "Received unknown op code: %d", op);
        break;
    }
}

static char *json_ttoa(const char *data, jsmntok_t *tok) {
    int len = tok->end - tok->start + 1;
    char *str = malloc(len * sizeof(char));
    snprintf(str, len, (char *)(data + tok->start));
    // strncpy(str, data, len);
    // str[len - 1] = '\0';
    return str;
}

static void BOT_receive_payload(const char *data, int len) {
    int r = jsmn_parse(&parser, data, len, tkns, 256);

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
        if (json_equal(data, &tkns[i], "t")) { // Event name
            // char *event = (char *)(data + tkns[i + 1].start);
            char *event = json_ttoa(data, &tkns[i + 1]);
            BOT_event(event);
            i++;                                      // Skip token that we just read
        } else if (json_equal(data, &tkns[i], "s")) { // Sequence
            char *new_seq = json_ttoa(data, &tkns[i + 1]);
            BOT_set_sequence(new_seq);
            i++; // Skip token that we just read
        } else if (json_equal(data, &tkns[i], "op")) {
            int op = atoi(json_ttoa(data, &tkns[i + 1]));
            BOT_op_code(op);
            i++; // Skip token that we just read
        } else if (json_equal(data, &tkns[i], "d")) {
            int j;
            ESP_LOGD(BOT_TAG, "Reading packet data");
            if (tkns[i + 1].type != JSMN_OBJECT) {
                continue; /* We expect data to be an object */
            }
            for (j = 0; j < tkns[i + 1].size; j++) {
                int k = i + j + 2;
                switch (BOT.event) { // Depends on the message event being identified beforehand
                case EVENT_GUILD_OBJ:
                    if (json_equal(data, &tkns[k], "name")) {
                        char *new_id = json_ttoa(data, &tkns[k + 1]);
                        BOT_set_session_id(new_id);
                        j++; // Skip token that we just read
                    }
                    break;
                case EVENT_READY:
                    if (json_equal(data, &tkns[k], "session_id")) {
                        char *new_id = json_ttoa(data, &tkns[k + 1]);
                        BOT_set_session_id(new_id);
                        j++; // Skip token that we just read
                    }
                    break;
                default:
                    if (json_equal(data, &tkns[k], "heartbeat_interval")) {
                        int beat = atoi(json_ttoa(data, &tkns[k + 1]));
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
}