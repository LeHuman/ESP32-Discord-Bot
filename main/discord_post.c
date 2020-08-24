#define REST_PATH CONFIG_REST_PATH_PATTERN
#define REST_AUTH_PREFIX CONFIG_REST_AUTH_PREFIX

#include "esp_log.h"
#include "http_post.c"
#include <stdlib.h>

static const char REST_TAG[] = "Rest";
static const char MSG_STR[] = "{\"content\":\"%s\",\"tts\":false,\"embed\":{\"title\":\"%s\",\"description\":\"%s\"}}";

extern void discord_rest_post(const char *content, const char *title, const char *description, const char *channel_id) {
    int length = strlen(REST_PATH) + strlen(channel_id) + 1;
    char *path_buf = malloc(length);
    snprintf(path_buf, length, REST_PATH, channel_id);

    length = strlen(MSG_STR) + strlen(content) + strlen(title) + strlen(description) + 1;
    char *jsonContent_buf = malloc(length);
    snprintf(jsonContent_buf, length, MSG_STR, content, title, description);

    // strdup memory, as copied struct points to same elements
    http_post_data_t postData = {
        .jsonContent = strdup(jsonContent_buf),
        .path = strdup(path_buf),
    };

    free(path_buf);
    free(jsonContent_buf);

    ESP_LOGI(REST_TAG, "Queuing allocated message");
    xQueueSendToBack(HTTP_POST_Queue, &postData, 0);
}

extern QueueHandle_t discord_rest_init(const char *bot_token) {
    ESP_LOGI(REST_TAG, "Initalizing discord POST");
    int length = strlen(REST_AUTH_PREFIX) + strlen(bot_token) + 1;
    char *authToken_buf = malloc(length);
    snprintf(authToken_buf, length, "%s%s", REST_AUTH_PREFIX, bot_token);
    authHeader = strdup(authToken_buf);
    return http_init(bot_token);
}