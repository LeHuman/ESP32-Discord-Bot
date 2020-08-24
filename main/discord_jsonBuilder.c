#include "esp_log.h"
#include "jsonBuilder.c"

// static const char DJB_TAG[] = "DiscJB";

extern char *discord_json_build_content(const char *content, const char *title, const char *description, const char *author,
                                        const char *author_icon_url, const char *footer, const char *footer_icon_url) {

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