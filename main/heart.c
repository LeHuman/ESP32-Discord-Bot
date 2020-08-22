#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *PM_TAG = "Heart";
static const char *HB_STR = "{\"op\": 1,\"d\": \"%s\"}";
static TimerHandle_t pacemaker_handle;
typedef void (*pacmaker_message_handle)(const char *, int len); // function that will send the heartbeat messages
static pacmaker_message_handle message_handle;

extern void pacemaker_send_heartbeat() {
    ESP_LOGI(PM_TAG, "Sending Heartbeat");
    message_handle(HB_STR, 128);
}

extern void pacemaker_update_interval(int heartbeat) {
    ESP_LOGI(PM_TAG, "New Heart beat: %d", beat);                                  // log update
    pacemaker_send_heartbeat();                                                    // Send once as we are updating the heartbeat
    xTimerChangePeriod(pacemaker_handle, pdMS_TO_TICKS(heartbeat), portMAX_DELAY); // Ensure pacemaker is up to date
    xTimerReset(pacemaker_handle, portMAX_DELAY);                                  // Reset the pacemaker
}

extern void pacemaker_init(pacmaker_message_handle pacemaker_message_handler) {
    message_handle = pacemaker_message_handler;
    pacemaker_handle = xTimerCreate("Bot Pacemaker", portMAX_DELAY, pdFALSE, NULL, pacemaker_send_heartbeat);
}