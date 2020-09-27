#ifndef __DISCORD_H__
#define __DISCORD_H__

#include <stdio.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_log.h"

#define discord_send_text_message(content, channel_id) discord_send_message(content, NULL, NULL, NULL, NULL, NULL, NULL, channel_id)
#define discord_send_basic_embed(title, description, channel_id) discord_send_message(NULL, title, description, NULL, NULL, NULL, NULL, channel_id)

extern esp_err_t discord_init(const char *bot_token);

extern void discord_send_message(const char *content, const char *title, const char *description, const char *author,
                                 const char *author_icon_url, const char *footer, const char *footer_icon_url, const char *channel_id);

#endif // __DISCORD_H__