/*
 * Application entry point: bring up the CH9434 over SPI, configure all four
 * sub-UARTs and run a continuous loopback / cross-link self test.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ch9434_uart.h"
#include "test_app.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "CH9434 SPI -> 4xUART bridge test starting ...");

    esp_err_t ret = ch9434_chip_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CH9434 init failed: %s. Check SPI wiring (MOSI=11 MISO=13 SCK=12 CS=10).",
                 esp_err_to_name(ret));
        /* Halt so the failure stays visible in the monitor. */
        vTaskDelay(pdMS_TO_TICKS(1000));
        abort();
    }

    /* Run the loopback / cross-link test forever. */
    test_app_run();
}
