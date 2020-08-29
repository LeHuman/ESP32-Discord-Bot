#define REST_PATH CONFIG_REST_PATH_PATTERN
#define REST_AUTH_PREFIX CONFIG_REST_AUTH_PREFIX

#include <stdlib.h>

#include "esp_log.h"
#include "http_post.c"
#include "jsonBuilder.c"

#define discord_send_text_message(content, channel_id) discord_send_message(content, NULL, NULL, NULL, NULL, NULL, NULL, channel_id)
#define discord_send_basic_embed(title, description, channel_id) discord_send_message(NULL, title, description, NULL, NULL, NULL, NULL, channel_id)

static const char DISC_TAG[] = "Discord";
// static const char MSG_STR[] = "{\"content\":\"%s\",\"tts\":false,\"embed\":{\"title\":\"%s\",\"description\":\"%s\"}}";

static void discord_rest_post(const char *json_content, const char *channel_id) {
    ESP_LOGI(DISC_TAG, "POSTing Message");
    int length = strlen(REST_PATH) + strlen(channel_id) + 1;
    char *path_buf = calloc(1, length);
    snprintf(path_buf, length, REST_PATH, channel_id);

    // strdup memory, as copied struct points to same elements
    http_post_data_t postData = {
        .jsonContent = strdup(json_content),
        .path = strdup(path_buf),
    };

    free(path_buf);

    http_queue_message(&postData);
}

// Currently only json content can be dynamiclly created
static char *discord_json_build_content(const char *content, const char *title, const char *description, const char *author,
                                        const char *author_icon_url, const char *footer, const char *footer_icon_url) {

    ESP_LOGI(DISC_TAG, "Building content JSON");

    json_object_t f_json = json_init();

    if (content != NULL) {
        json_key(&f_json, "content");
        json_string(&f_json, content);
    }

    if (title != NULL || description != NULL || author != NULL || footer != NULL) {
        json_key(&f_json, "embeds");
        json_open_array(&f_json);
        json_open_list(&f_json);

        if (title != NULL) {
            json_key(&f_json, "title");
            json_string(&f_json, title);
        }

        if (description != NULL) {
            json_key(&f_json, "description");
            json_string(&f_json, description);
        }

        if (author != NULL) {
            json_key(&f_json, "author");
            json_open_list(&f_json);
            json_key(&f_json, "name");
            json_string(&f_json, author);
            if (author_icon_url != NULL) {
                json_key(&f_json, "icon_url");
                json_string(&f_json, author_icon_url);
            }
            json_close_list(&f_json);
        }

        if (footer != NULL) {
            json_key(&f_json, "footer");
            json_open_list(&f_json);
            json_key(&f_json, "text");
            json_string(&f_json, footer);
            if (footer_icon_url != NULL) {
                json_key(&f_json, "icon_url");
                json_string(&f_json, footer_icon_url);
            }
            json_close_list(&f_json);
        }

        json_close_list(&f_json);
        json_close_array(&f_json);
    }

    return json_finish(&f_json);
}

extern esp_err_t discord_init(const char *bot_token) {
    ESP_LOGI(DISC_TAG, "Generating auth header");
    int length = strlen(REST_AUTH_PREFIX) + strlen(bot_token) + 1;
    char *authToken_buf = calloc(1, length);
    snprintf(authToken_buf, length, "%s%s", REST_AUTH_PREFIX, bot_token);

    ESP_LOGI(DISC_TAG, "Initalizing HTTP POST");
    return http_init(authToken_buf);
}

extern void discord_send_message(const char *content, const char *title, const char *description, const char *author,
                                 const char *author_icon_url, const char *footer, const char *footer_icon_url, const char *channel_id) {

    char *json_content = discord_json_build_content(content, title, description, author, author_icon_url, footer, footer_icon_url);
    discord_rest_post(json_content, channel_id);
    free(json_content);
}