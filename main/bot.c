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

#include "esp_log.h"
#include "esp_websocket_client.h"

#include "heart.c"
#include "lib/jsmn-valueless-keys/jsmn.h"

#define WEBSOCKET_BUFFER_SIZE CONFIG_WEBSOCKET_BUFFER_SIZE
#define BOT_TOKEN CONFIG_BOT_TOKEN
#define JSMN_TOKEN_LENGTH 256

// TODO: add special "!help" case that prints what the bot's keyword prefix is, just in case

typedef void (*BOT_payload_handler)(char *); // function that will send the bot payloads

static const char *LOGIN_STR = "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"$os\":\"FreeRTOS\",\"$browser\":\"ESP32\",\"$device\":\"ESP32\"},\"compress\":true,\"large_threshold\":50,\"shard\":[0,1],\"presence\":{\"status\":\"online\",\"afk\":false},\"guild_subscriptions\":true,\"intents\":512}}";
static const char *BOT_TAG = "Bot";
static const char *JSM_TAG = "JSMN";

jsmn_parser parser;
jsmntok_t tkns[JSMN_TOKEN_LENGTH]; // IMPROVE: use dynamic token buffer
static char data_ptr[WEBSOCKET_BUFFER_SIZE];
static int data_len = 0;

enum payload_event { // What event did we receive
    EVENT_NULL,
    EVENT_READY,
    EVENT_GUILD_OBJ,
    MESSAGE_CREATE,
};
typedef enum payload_event payload_event;

static QueueHandle_t BOT_message_queue;
static QueueHandle_t BOT_message_length_queue;
static BOT_payload_handler BOT_payload_handle;

struct BOT { // TODO: reconnect bot every now and then to reset sequence number to avoid huge seq numbers
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
} BOT;

typedef struct BOT_basic_message {
    char *channel_id;
    char *author;
    char *discriminator; // @User#1337 <--
    char *content;
} BOT_basic_message_t;

#define BOT_send_payload(data, len, ...)            \
    {                                               \
        snprintf(data_ptr, len, data, __VA_ARGS__); \
        vTaskDelay(pdMS_TO_TICKS(550));             \
        BOT_payload_handle(data_ptr);               \
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

static void BOT_heartbeat_handle(const char *data, int len) {
    if (!BOT.ACK) {                                                                        // confirmation of heartbeat was not received in time
        ESP_LOGE(BOT_TAG, "Did not receive heartbeat confirmation in time, reconnecting"); // TODO: throw error?
    } else {
        BOT.ACK = false;                      // Expecting ACK to return and set to true before next heartbeat
        BOT_send_payload(data, len, BOT.seq); // send with sequence number
    }
}

static void BOT_set_heartbeat_int(int beat) {
    pacemaker_update_interval(beat);
    BOT.ACK = true;
}

static void BOT_do_login() {
    ESP_LOGI(BOT_TAG, "Logging in...");
    BOT_send_payload(LOGIN_STR, 480, BOT_TOKEN);
}

static void BOT_event(char *event) { // TODO: set all the events that we care about
    ESP_LOGI(BOT_TAG, "Message event: %s", event);

    if (strcmp(event, "READY") == 0) {
        BOT_set_event(EVENT_READY);
    } else if (strcmp(event, "GUILD_CREATE") == 0) {
        BOT_set_event(EVENT_GUILD_OBJ);
    } else if (strcmp(event, "MESSAGE_CREATE") == 0) {
        BOT_set_event(MESSAGE_CREATE);
    } else {
        BOT_set_event(EVENT_NULL);
    }
}

static bool json_equal(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start && strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return true;
    }
    return false;
}

static bool json_null(const char *json, jsmntok_t *tok) {
    return strncmp(json + tok->start, "null", tok->end - tok->start) == 0;
}

static void BOT_op_code(int op) {
    BOT.lastOP = op;
    switch (op) {
    case 0:
        ESP_LOGI(BOT_TAG, "Received op code: Dispatch");
        break;
    case 1:
        ESP_LOGI(BOT_TAG, "Received op code: Heartbeat");
        BOT.ACK = true; // ensure proper heartbeat
        pacemaker_send_heartbeat();
        break;
    case 7:
        ESP_LOGI(BOT_TAG, "Received op code: Reconnect");
        break;
    case 9:
        ESP_LOGI(BOT_TAG, "Received op code: Invalid Session");
        break;
    case 10:
        ESP_LOGI(BOT_TAG, "Received op code: Hello");
        BOT_do_login();
        break;
    case 11:
        ESP_LOGI(BOT_TAG, "Received op code: Heartbeat ACK");
        BOT.ACK = true;
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

static void BOT_payload_task(void *pvParameters) {
    for (;;) {
        ESP_LOGI(BOT_TAG, "Waiting for queue");
        xQueueReceive(BOT_message_queue, data_ptr, portMAX_DELAY);         // Wait for new message in queue
        xQueueReceive(BOT_message_length_queue, &data_len, portMAX_DELAY); // Should not have to wait long, if at all, but here ya go ig

        jsmn_init(&parser); // IG we gotta reinit everytime?
        int r = jsmn_parse(&parser, data_ptr, data_len, tkns, JSMN_TOKEN_LENGTH);

        ESP_LOGD(BOT_TAG, "Received=%.*s Size=%d", data_len, data_ptr, data_len);
        int msg_left = uxQueueMessagesWaiting(BOT_message_queue);
        if (msg_left > 0)
            ESP_LOGI(BOT_TAG, "Messages queued: %d", msg_left);

        if (r < 1 || tkns[0].type != JSMN_OBJECT) { // Assume the top-level element is an object
            if (r < 0) {
                ESP_LOGE(JSM_TAG, "Failed to parse JSON: %d", r);
            } else {
                ESP_LOGE(JSM_TAG, "Object expected in JSON");
            }
        } else {
            BOT_basic_message_t message = {}; // initialize new payload
            bool voided = false;
            for (size_t i = 1; i < r; i++) {
                if (voided) // break if there was an issue with reading this payload
                    break;
                if (json_equal(data_ptr, &tkns[i], "t")) { // Event name
                    int len = (tkns[i + 1].end - tkns[i + 1].start) + 1;
                    char event[len];
                    snprintf(event, len, (char *)(data_ptr + tkns[i + 1].start));
                    BOT_event(event);
                    i++;                                                                                // Skip token that we just read
                } else if (json_equal(data_ptr, &tkns[i], "s") && !json_null(data_ptr, &tkns[i + 1])) { // Sequence
                    int len = (tkns[i + 1].end - tkns[i + 1].start) + 1;
                    char new_seq[len];
                    snprintf(new_seq, len, (char *)(data_ptr + tkns[i + 1].start));
                    BOT_set_sequence(new_seq);
                    i++; // Skip token that we just read
                } else if (json_equal(data_ptr, &tkns[i], "op") && !json_null(data_ptr, &tkns[i + 1])) {
                    int len = (tkns[i + 1].end - tkns[i + 1].start) + 1;
                    char opStr[len];
                    snprintf(opStr, len, (char *)(data_ptr + tkns[i + 1].start));
                    BOT_op_code(atoi(opStr));
                    i++; // Skip token that we just read
                } else if (json_equal(data_ptr, &tkns[i], "d")) {
                    int j;
                    if (tkns[i + 1].type != JSMN_OBJECT) {
                        continue; /* We expect data to be an object */
                    }
                    ESP_LOGI(BOT_TAG, "Reading payload data");
                    for (j = 0; j < tkns[i + 1].size; j++) {
                        if (voided) // break if there was an issue with reading this payload's data
                            break;
                        int k = i + j + 2;
                        switch (BOT.event) { // Depends on the message event being identified beforehand
                        case MESSAGE_CREATE:
                            if (json_equal(data_ptr, &tkns[k], "member")) {
                                ESP_LOGI(BOT_TAG, "Not checking for caster role"); // TODO: check for caster role
                            } else if (json_equal(data_ptr, &tkns[k], "author")) {
                                ESP_LOGI(BOT_TAG, "Reading author data");
                                int l;
                                for (l = 0; l < tkns[k + 1].size; l++) {
                                    int _k = k + l;
                                    if (json_equal(data_ptr, &tkns[_k], "username")) {
                                        int len = (tkns[_k + 1].end - tkns[_k + 1].start) + 1;
                                        char data[len];
                                        snprintf(data, len, (char *)(data_ptr + tkns[_k + 1].start));
                                        ESP_LOGI(BOT_TAG, "Author: %s", data);
                                        message.author = data;
                                        l++;
                                    } else if (json_equal(data_ptr, &tkns[_k], "discriminator")) {
                                        int len = (tkns[_k + 1].end - tkns[_k + 1].start) + 1;
                                        char data[len];
                                        snprintf(data, len, (char *)(data_ptr + tkns[_k + 1].start));
                                        ESP_LOGI(BOT_TAG, "Discriminator: %s", data);
                                        message.discriminator = data;
                                        l++;
                                    }
                                }
                                j += tkns[j + 1].size + 1; // Skip the tokens that were in the data block
                            } else if (json_equal(data_ptr, &tkns[k], "channel_id")) {
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char data[len];
                                snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                ESP_LOGI(BOT_TAG, "Channel: %s", data);
                                message.channel_id = data;
                                j++;
                            } else if (json_equal(data_ptr, &tkns[k], "content")) {
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1; // TODO: only validate messages that start with !cast
                                if (len < 5) {                                       // TODO: replace with actual bot keyword length
                                    voided = true;
                                } else {
                                    char data[len];
                                    snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                    ESP_LOGI(BOT_TAG, "Message: %s", data);
                                    message.content = data;
                                }
                                j++;
                            } else if (json_equal(data_ptr, &tkns[k], "type")) {
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char data[len];
                                snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                if (strcmp(data, "0") != 0) {
                                    voided = true;
                                }
                                j++;
                            } else if (json_equal(data_ptr, &tkns[k], "webhook_id")) { // Don't read webhook messages
                                voided = true;
                                j++;
                            }
                            break;
                        case EVENT_GUILD_OBJ:
                            if (json_equal(data_ptr, &tkns[k], "name") && !json_null(data_ptr, &tkns[k + 1])) {
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char name[len];
                                snprintf(name, len, (char *)(data_ptr + tkns[k + 1].start));
                                ESP_LOGI(BOT_TAG, "Guild name: %s", name);
                                j++; // Skip token that we just read
                            }
                            break;
                        case EVENT_READY:
                            if (json_equal(data_ptr, &tkns[k], "session_id") && !json_null(data_ptr, &tkns[k + 1])) {
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char new_id[len];
                                snprintf(new_id, len, (char *)(data_ptr + tkns[k + 1].start));
                                BOT_set_session_id(new_id);
                                j++; // Skip token that we just read
                            }
                            break;
                        default:
                            if (json_equal(data_ptr, &tkns[k], "heartbeat_interval") && !json_null(data_ptr, &tkns[k + 1])) {
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char beatStr[len];
                                snprintf(beatStr, len, (char *)(data_ptr + tkns[k + 1].start));
                                int beat = atoi(beatStr);
                                BOT_set_heartbeat_int(beat);
                                j++; // Skip token that we just read
                            }
                            break;
                        }
                    }
                    i += tkns[i + 1].size + 1; // Skip the tokens that were in the data block
                }
            }
            if (voided) {
                ESP_LOGW(BOT_TAG, "Last message was voided");
            } else {
                ESP_LOGI(BOT_TAG, "Basic message received");
                // TODO: do somthing with new message
            }
        }
    }
    vTaskDelete(NULL);
}

extern void BOT_init(BOT_payload_handler payload_handle, QueueHandle_t message_queue_handle, QueueHandle_t message_length_queue_handle) {
    ESP_LOGI(JSM_TAG, "Initalizing Json parser");

    BOT_payload_handle = payload_handle;
    BOT_message_queue = message_queue_handle;
    BOT_message_length_queue = message_length_queue_handle;

    BOT.ACK = false;
    BOT.activeGuild = "null";
    BOT.ready = false;
    BOT.seq = "null";
    BOT.event = EVENT_NULL;

    ESP_LOGI(BOT_TAG, "Starting BOT task");
    if (xTaskCreate(BOT_payload_task, "BOT task", 8192, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(BOT_TAG, "Failed to start BOT task!");
    }

    ESP_LOGI(BOT_TAG, "Starting Pacemaker timer");
    pacemaker_init(BOT_heartbeat_handle);
}