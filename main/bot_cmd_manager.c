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
#include <unordered_map>

#include "bot_commands.c"

#define COMMAND_QUEUE_SIZE CONFIG_WEBSOCKET_QUEUE_SIZE
#define COMMAND_MAX_TASK 5     // max number of concurrent tasks
#define COMMAND_TASK_SIZE 2048 // Amount of memory each command task has

static QueueHandle_t BOT_command_queue;
static TaskHandle_t handles[COMMAND_MAX_TASK];
static const char CMD_TAG[] = "BotCMD";

typedef struct BOT_basic_message {
    char *channel_id;
    char *guild_id;
    char *author;
    char *author_mention;
    char *author_id;
    char *content;
} BOT_basic_message_t;

#define msg_set_channel_id(msg, string) msg.channel_id = strdup(string);
#define msg_set_guild_id(msg, string) msg.guild_id = strdup(string);
#define msg_set_author(msg, string) msg.author = strdup(string);
#define msg_set_author_mention(msg, string) msg.author_mention = strdup(string);
#define msg_set_author_id(msg, string) msg.author_id = strdup(string);
#define msg_set_content(msg, string) msg.content = strdup(string);

static void destroy_basic_message(BOT_basic_message_t *msg) {
    free(msg->channel_id);
    free(msg->guild_id);
    free(msg->author);
    free(msg->author_id);
    free(msg->author_mention);
    free(msg->content);
}

static void BOT_command_queue_task(void *pvParameters) {
    BOT_basic_message_t user_message;

    for (;;) {
        ESP_LOGI(CMD_TAG, "Waiting for queue");
        xQueuePeek(BOT_command_queue, &user_message, portMAX_DELAY);
        ESP_LOGI(CMD_TAG, "Distilling command");
        xQueueReceive(BOT_command_queue, &user_message, portMAX_DELAY); // Wait for new message in queue

        // std::unordered_map<string, int> map;

        // TODO: match and call handler right here

        destroy_basic_message(BOT_command_queue);
    }
    vTaskDelete(NULL);
}

extern void BOT_queue_command_message(BOT_basic_message_t *message) {
    xQueueSendToBack(BOT_command_queue, message, 0);
}

extern esp_err_t BOT_init_cmd() {
    BOT_command_queue = xQueueCreate(COMMAND_QUEUE_SIZE, sizeof(struct BOT_basic_message));
    if (BOT_command_queue == NULL) {
        ESP_LOGE(CMD_TAG, "Failed to create queue");
        return ESP_FAIL;
    }
    if (xTaskCreate(BOT_command_queue_task, "BOT CMD", 4096, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(CMD_TAG, "Failed to start command manager task");
        return ESP_FAIL;
    }
    return ESP_OK;
}