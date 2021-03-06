#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <stdio.h>

#define BLINK_GPIO CONFIG_BLINK_GPIO

static char run_blink = 0;
static const char BNK_TAG[] = "Blink";
const TickType_t xDelay = portTICK_PERIOD_MS / 3;

extern void blink_mult(char a) {
    run_blink += a;
}

extern void blink() {
    run_blink += 1;
}

static void blink_task(void *pvParameters) {
    gpio_pad_select_gpio(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    for (;;) {
        if (run_blink > 0) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(xDelay);
            gpio_set_level(BLINK_GPIO, 0);
            run_blink--;
        }
        vTaskDelay(xDelay);
    }
    vTaskDelete(NULL);
}

extern esp_err_t start_blink_task() {
    if (xTaskCreate(blink_task, "blinker", 2048, NULL, 1, NULL) == pdPASS) { // 1576 is exact number of bytes actually needed
        ESP_LOGI(BNK_TAG, "Blink Task started");
        return ESP_OK;
    } else {
        ESP_LOGE(BNK_TAG, "Blink Task failed to start");
        return ESP_FAIL;
    }
}