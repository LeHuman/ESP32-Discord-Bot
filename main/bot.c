#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "jsmn.h"
#include "nvs_flash.h"

#include "bot_cmd_manager.c"
#include "discord.h"
#include "heart.c"
#include "helper.h"

#define JSMN_TOKEN_LENGTH 256
#define BOT_TOKEN CONFIG_BOT_TOKEN
#define BOT_PREFIX CONFIG_BOT_PREFIX
#define BOT_PREFIX_LENGTH strlen(BOT_PREFIX)
#define BOT_BUFFER_SIZE CONFIG_WEBSOCKET_BUFFER_SIZE
#define BOT_CASE_SENSITIVE CONFIG_BOT_CASE_SENSITIVE
#ifdef CONFIG_BOT_HELP
#define BOT_HELP_STRING "Use any of the following after " BOT_PREFIX "\\n```help: Show this message\\n" CONFIG_BOT_HELP_STRING "```"
#ifdef CONFIG_BOT_BASIC_HELP
#define BOT_BASIC_HELP "If you need my help, use the following command\\n```" BOT_PREFIX " help```"
#endif
#endif

// IMPROVE: reconnect bot every now and then to reset sequence number to avoid huge seq numbers

typedef void (*BOT_payload_handler)(char *); // function that will send the bot payloads

static const char BOT_TAG[] = "Bot";
static const char JSM_TAG[] = "JSMN";

// No reason to build the login json
static const char LOGIN_STR[] = "{\"op\":2,\"d\":{\"token\":\"%s\",\"properties\":{\"$os\":\"FreeRTOS\",\"$browser\":\"ESP_HTTP_CLIENT\",\"$device\":\"ESP32\"},\"compress\":true,\"large_threshold\":50,\"shard\":[0,1],\"presence\":{\"status\":\"online\",\"afk\":false},\"guild_subscriptions\":true,\"intents\":512}}";
static const char HB_STR[] = "{\"op\": 1,\"d\": \"%s\"}";
static const char BOT_MENTION_PATTERN[] = "<@%s>";

static jsmn_parser parser;
static jsmntok_t tkns[JSMN_TOKEN_LENGTH]; // IMPROVE: use dynamic token buffer
static char data_ptr[BOT_BUFFER_SIZE];
static char payload_ptr[BOT_BUFFER_SIZE]; // IMPROVE: use semaphore instead of double buffer
SemaphoreHandle_t xPayload_sema;

enum payload_event { // What event did we receive
    EVENT_NULL,
    EVENT_READY,
    EVENT_GUILD_OBJ,
    MESSAGE_CREATE,
};
typedef enum payload_event payload_event;

static QueueHandle_t BOT_message_queue;
static BOT_payload_handler BOT_payload_handle;
static payload_event BOT_event = EVENT_NULL;
static char *BOT_session_id;
// static char *BOT_token = "null";
static char *BOT_seq; // format as integer, "null" otherwise
static int BOT_lastOP = -1;
// static char *BOT_activeGuild = "null";
// static bool BOT_ready = false;
static bool BOT_ACK = false;

#define BOT_send_payload(data, len, ...)               \
    {                                                  \
        ESP_LOGD(BOT_TAG, "Payload waiting");          \
        vTaskDelay(pdMS_TO_TICKS(550));                \
        xSemaphoreTake(xPayload_sema, portMAX_DELAY);  \
        snprintf(payload_ptr, len, data, __VA_ARGS__); \
        BOT_payload_handle(payload_ptr);               \
        xSemaphoreGive(xPayload_sema);                 \
        ESP_LOGD(BOT_TAG, "Payload done");             \
    }

static void BOT_set_session_id(char *new_id) {
    ESP_LOGI(BOT_TAG, "New Session ID: %s", new_id);
    free(BOT_session_id);
    BOT_session_id = strdup(new_id); // IMPROVE: Does this need to be freed?
}

static void BOT_set_sequence(char *new_seq) {
    ESP_LOGI(BOT_TAG, "Sequence: %s", new_seq);
    free(BOT_seq);
    BOT_seq = strdup(new_seq);
}

static void BOT_set_event(payload_event event) {
    BOT_event = event;
}

static void BOT_heartbeat_task(void *pvParameters) {
    if (BOT_ACK == false) { // confirmation of heartbeat was not received in time
        ESP_LOGE(BOT_TAG, "Did not receive heartbeat confirmation in time, reconnecting");
        esp_restart();
    } else {
        BOT_ACK = false; // Expecting ACK to return and set to true before next heartbeat
        int len = strlen(HB_STR) + strlen(BOT_seq) + 1;
        BOT_send_payload(HB_STR, len, BOT_seq); // send with sequence number
    }
    vTaskDelete(NULL);
}

static void BOT_set_heartbeat_int(int beat) {
    BOT_ACK = true;
    pacemaker_update_interval(beat);
}

static void BOT_do_login_task(void *pvParameters) {
    ESP_LOGI(BOT_TAG, "Sending login info");
    BOT_send_payload(LOGIN_STR, 480, BOT_TOKEN);
    vTaskDelete(NULL);
}

static void BOT_new_event(char *event) { // TODO: set all the events that we care about
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

static void BOT_op_code(int op) {
    BOT_lastOP = op;
    switch (op) {
    case 0:
        ESP_LOGI(BOT_TAG, "Received op code: Dispatch");
        break;
    case 1:
        ESP_LOGI(BOT_TAG, "Received op code: Heartbeat");
        BOT_ACK = true; // ensure proper heartbeat
        pacemaker_send_heartbeat();
        break;
    case 7:
        ESP_LOGI(BOT_TAG, "Received op code: Reconnect");
        esp_restart();
        break;
    case 9:
        ESP_LOGI(BOT_TAG, "Received op code: Invalid Session");
        esp_restart();
        break;
    case 10:
        ESP_LOGI(BOT_TAG, "Received op code: Hello");
        xTaskCreate(BOT_do_login_task, "BOTLOGIN", 2048, NULL, 12, NULL);
        break;
    case 11:
        ESP_LOGI(BOT_TAG, "Received op code: Heartbeat ACK");
        BOT_ACK = true;
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

static bool json_equal(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start && strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return true;
    }
    return false;
}

static bool json_null(const char *json, jsmntok_t *tok) {
    return strncmp(json + tok->start, "null", tok->end - tok->start) == 0;
}

static int jsmn_get_token_size(jsmntok_t *token) {
    unsigned int i, j;
    jsmntok_t *key;
    int result = 0;

    if (token->type == JSMN_PRIMITIVE) {
        result = 1;
    } else if (token->type == JSMN_STRING) {
        result = 1;
    } else if (token->type == JSMN_OBJECT) {
        j = 0;
        for (i = 0; i < token->size; i++) {
            key = token + 1 + j;
            j += jsmn_get_token_size(key);
            if (key->size > 0) {
                j += jsmn_get_token_size(token + 1 + j);
            }
        }
        result = j + 1;
    } else if (token->type == JSMN_ARRAY) {
        j = 0;
        for (i = 0; i < token->size; i++) {
            j += jsmn_get_token_size(token + 1 + j);
        }
        result = j + 1;
    }

    return result;
}

static inline int jsmn_get_total_size(jsmntok_t *token) {
    int res = jsmn_get_token_size(token + 1) + 1;
    return res;
}

static void BOT_payload_task(void *pvParameters) {
    for (;;) {
        ESP_LOGI(BOT_TAG, "Waiting for queue");                    // IMPROVE: Only use one queue for BOT task
        xQueueReceive(BOT_message_queue, data_ptr, portMAX_DELAY); // Wait for new message in queue
        int data_len = strlen(data_ptr);

        jsmn_init(&parser); // IG we gotta reinit everytime?
        int r = jsmn_parse(&parser, data_ptr, data_len, tkns, JSMN_TOKEN_LENGTH);

#ifdef CONFIG_BOT_BASIC_HELP
        bool basic_help = false; // send basic help
#endif

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
            BOT_basic_message_t bot_message;
            msg_set_content(bot_message, "");
            bool voided = false;
            for (size_t i = 1; i < r; i++) {
                if (voided) { // break if there was an issue with reading this payload
                    ESP_LOGW(BOT_TAG, "Unable to continue reading payload");
                    break;
                } else if (json_equal(data_ptr, &tkns[i], "t")) { // Event name
                    ESP_LOGD(BOT_TAG, "Get event name");
                    int len = (tkns[i + 1].end - tkns[i + 1].start) + 1;
                    char *event = malloc(len);
                    snprintf(event, len, (char *)(data_ptr + tkns[i + 1].start));
                    BOT_new_event(event);
                    free(event);
                    i++; // Skip tokens that we just read
                } else if (json_equal(data_ptr, &tkns[i], "s")) {
                    ESP_LOGD(BOT_TAG, "Get sequence");
                    if (!json_null(data_ptr, &tkns[i + 1])) {
                        int len = (tkns[i + 1].end - tkns[i + 1].start) + 1;
                        char *new_seq = malloc(len);
                        snprintf(new_seq, len, (char *)(data_ptr + tkns[i + 1].start));
                        BOT_set_sequence(new_seq);
                        free(new_seq);
                    }
                    i++; // Skip tokens that we just read
                } else if (json_equal(data_ptr, &tkns[i], "op")) {
                    ESP_LOGD(BOT_TAG, "Get op code");
                    if (!json_null(data_ptr, &tkns[i + 1])) {
                        int len = (tkns[i + 1].end - tkns[i + 1].start) + 1;
                        char *opStr = malloc(len);
                        snprintf(opStr, len, (char *)(data_ptr + tkns[i + 1].start));
                        BOT_op_code(atoi(opStr));
                        free(opStr);
                    }
                    i++; // Skip tokens that we just read
                } else if (json_equal(data_ptr, &tkns[i], "d")) {

                    if (tkns[i + 1].type != JSMN_OBJECT) {
                        voided = true;
                        continue; /* We expect data to be an object */
                    }

                    ESP_LOGI(BOT_TAG, "Reading payload data");

                    int j;
                    int k = i + 2;
                    for (j = 0; j < tkns[i + 1].size; j++) {
                        if (voided) { // break if there was an issue with reading this payload's data
                            ESP_LOGW(BOT_TAG, "Unable to continue reading message");
                            break;
                        }

                        switch (BOT_event) { // Depends on the message event being identified beforehand
                        case MESSAGE_CREATE:
                            if (json_equal(data_ptr, &tkns[k], "member")) {
                                ESP_LOGD(BOT_TAG, "data: member");
                                ESP_LOGI(BOT_TAG, "Not checking for caster role"); // TODO: check for caster role
                                k += jsmn_get_total_size(&tkns[k]);                // Skip the tokens that were in this data block
                            } else if (json_equal(data_ptr, &tkns[k], "author")) { // Get author data
                                ESP_LOGD(BOT_TAG, "data: author");
                                int l;
                                int _k = k + 2;
                                for (l = 0; l < tkns[k + 1].size; l++) {
                                    if (json_equal(data_ptr, &tkns[_k], "username")) {
                                        ESP_LOGD(BOT_TAG, "data: username");
                                        int len = (tkns[_k + 1].end - tkns[_k + 1].start) + 1;
                                        char *data = malloc(len);
                                        snprintf(data, len, (char *)(data_ptr + tkns[_k + 1].start));
                                        msg_set_author(bot_message, data);
                                        free(data);
                                        _k += 2;
                                    } else if (json_equal(data_ptr, &tkns[_k], "id")) {
                                        ESP_LOGD(BOT_TAG, "data: id");
                                        int len = (tkns[_k + 1].end - tkns[_k + 1].start) + 1;
                                        char *data = malloc(len);
                                        snprintf(data, len, (char *)(data_ptr + tkns[_k + 1].start));
                                        msg_set_author_id(bot_message, data);
                                        free(data);

                                        len = len + strlen(BOT_MENTION_PATTERN);
                                        data = malloc(len);
                                        snprintf(data, len, BOT_MENTION_PATTERN, bot_message.author_id);
                                        msg_set_author_mention(bot_message, data);
                                        free(data);
                                        _k += 2;
                                    } else {
                                        _k += jsmn_get_total_size(&tkns[_k]);
                                    }
                                }
                                k += jsmn_get_total_size(&tkns[k]); // Skip the tokens that were in this data block
                            } else if (json_equal(data_ptr, &tkns[k], "channel_id")) {
                                ESP_LOGD(BOT_TAG, "data: channel_id");
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char *data = malloc(len);
                                snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                msg_set_channel_id(bot_message, data);
                                free(data);
                                k += jsmn_get_total_size(&tkns[k]);
                            } else if (json_equal(data_ptr, &tkns[k], "content")) { // Only accept prefixed content
                                ESP_LOGD(BOT_TAG, "data: content");
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char *data = malloc(len);
                                snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                if (!string_match(data, BOT_PREFIX)) { // Void if prefix does not exist
#ifdef CONFIG_BOT_BASIC_HELP
                                    if (string_match(data, "!help")) {
                                        ESP_LOGI(BOT_TAG, "!help detected, queueing basic help string");
                                        basic_help = true;
                                        msg_set_content(bot_message, data);
                                        free(data);
                                        k += 2;
                                        continue;
                                    } else {
#endif
                                        ESP_LOGW(BOT_TAG, "Message does not have prefix, ignoring");
#ifdef CONFIG_BOT_BASIC_HELP
                                    }
#endif
                                    voided = true;
                                    free(data);
                                    continue;
                                }
                                msg_set_content(bot_message, data + BOT_PREFIX_LENGTH); // ignore the prefix
                                free(data);
                                k += jsmn_get_total_size(&tkns[k]);
                            } else if (json_equal(data_ptr, &tkns[k], "guild_id")) {
                                ESP_LOGD(BOT_TAG, "data: guild_id");
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char *data = malloc(len);
                                snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                msg_set_guild_id(bot_message, data);
                                free(data);
                                k += jsmn_get_total_size(&tkns[k]);
                            } else if (json_equal(data_ptr, &tkns[k], "type")) {
                                ESP_LOGD(BOT_TAG, "data: type");
                                int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                char *data = malloc(len);
                                snprintf(data, len, (char *)(data_ptr + tkns[k + 1].start));
                                if (strcmp(data, "0") != 0) {
                                    voided = true;
                                }
                                free(data);
                                k += jsmn_get_total_size(&tkns[k]);
                            } else if (json_equal(data_ptr, &tkns[k], "webhook_id")) { // Don't read webhook messages
                                ESP_LOGD(BOT_TAG, "data: webhook_id");
                                voided = true;
                                k += jsmn_get_total_size(&tkns[k]);
                            } else {
                                ESP_LOGD(BOT_TAG, "data: unread token");
                                k += jsmn_get_total_size(&tkns[k]);
                            }
                            break;
                        case EVENT_GUILD_OBJ:
                            if (json_equal(data_ptr, &tkns[k], "name")) {
                                if (!json_null(data_ptr, &tkns[k + 1])) {
                                    int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                    char *name = malloc(len);
                                    snprintf(name, len, (char *)(data_ptr + tkns[k + 1].start));
                                    free(name);
                                }
                                k += jsmn_get_total_size(&tkns[k]);
                            }
                            break;
                        case EVENT_READY:
                            if (json_equal(data_ptr, &tkns[k], "session_id")) {
                                if (!json_null(data_ptr, &tkns[k + 1])) {
                                    int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                    char *new_id = malloc(len);
                                    snprintf(new_id, len, (char *)(data_ptr + tkns[k + 1].start));
                                    BOT_set_session_id(new_id);
                                    free(new_id);
                                }
                                k += jsmn_get_total_size(&tkns[k]);
                            }
                            break;
                        default:
                            if (json_equal(data_ptr, &tkns[k], "heartbeat_interval")) {
                                if (!json_null(data_ptr, &tkns[k + 1])) {
                                    int len = (tkns[k + 1].end - tkns[k + 1].start) + 1;
                                    char *beatStr = malloc(len);
                                    snprintf(beatStr, len, (char *)(data_ptr + tkns[k + 1].start));
                                    int beat = atoi(beatStr);
                                    BOT_set_heartbeat_int(beat);
                                    free(beatStr);
                                }
                                k += jsmn_get_total_size(&tkns[k]);
                            } else {
                                k++;
                            }
                            break;
                        }
                    }

                    i += jsmn_get_total_size(&tkns[i]); // Skip the tokens that were in the data block
                }
            }
            if (BOT_event == MESSAGE_CREATE) {
                if (voided || bot_message.content == NULL) {
                    ESP_LOGW(BOT_TAG, "Last message was voided or empty");
                    destroy_basic_message(&bot_message);
                } else {
                    ESP_LOGI(BOT_TAG, "Message: %s", bot_message.content);
                    ESP_LOGI(BOT_TAG, "Author: %s", bot_message.author);
                    ESP_LOGI(BOT_TAG, "Guild ID: %s", bot_message.guild_id);
                    ESP_LOGI(BOT_TAG, "Channel ID: %s", bot_message.channel_id);
#ifdef CONFIG_BOT_BASIC_HELP
                    if (basic_help) {
                        discord_send_text_message(BOT_BASIC_HELP, bot_message.channel_id);
                        destroy_basic_message(&bot_message);
                    } else {
#endif
                        BOT_queue_command_message(&bot_message);
#ifdef CONFIG_BOT_BASIC_HELP
                    }
#endif
                }
            } else {
                destroy_basic_message(&bot_message);
            }
        }
    }
    vTaskDelete(NULL);
}

extern esp_err_t BOT_init(BOT_payload_handler payload_handle, QueueHandle_t message_queue_handle) {
    BOT_payload_handle = payload_handle;
    BOT_message_queue = message_queue_handle;

    ESP_LOGI(BOT_TAG, "Initalizing vars");
    BOT_session_id = strdup("null");
    BOT_seq = strdup("null");
    xPayload_sema = xSemaphoreCreateBinary();
    xSemaphoreGive(xPayload_sema);

    ESP_LOGI(BOT_TAG, "Initalizing discord rest api");
    ESP_ERROR_CHECK(discord_init(BOT_TOKEN));

    ESP_LOGI(BOT_TAG, "Initalizing BOT command manager");
    ESP_ERROR_CHECK(BOT_init_cmd());

    ESP_LOGI(BOT_TAG, "Starting BOT task");
    if (xTaskCreate(BOT_payload_task, "BOT task", 8192, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(BOT_TAG, "Failed to start BOT task");
        return ESP_FAIL;
    }

    ESP_LOGI(BOT_TAG, "Initalizing gateway pacemaker");
    ESP_ERROR_CHECK(pacemaker_init(BOT_heartbeat_task));

    return ESP_OK;
}