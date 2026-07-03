/*
 * End-to-end test logic.
 *
 * Wires assumed:
 *   - UART0: TX0 -> RX0 (loopback on the breakout)
 *   - UART1: TX1 -> UART2: RX2
 *   - UART2: TX2 -> UART1: RX1
 *
 * Test modes:
 *   - Random data test: sends random length (1-255) random bytes, runs 100 groups
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ch9434_uart.h"
#include "ch9434_drv.h"
#include "ch9434_regs.h"
#include "test_app.h"

#define TAG "test"

/* ---------- UART3 background task configuration ---------- */
#define UART3_TX_INTERVAL_MS      3000   /* send every 3 seconds */
#define UART3_RX_POLL_INTERVAL_MS 50     /* poll RX every 50ms */
#define UART3_RX_BUF_SIZE         256    /* RX buffer size */

static TaskHandle_t s_uart3_tx_task_handle = NULL;
static TaskHandle_t s_uart3_rx_task_handle = NULL;
static volatile uint32_t s_uart3_tx_count = 0;
static volatile uint32_t s_uart3_rx_count = 0;

static const ch9434_uart_config_t k_default_cfg = {
    .baud        = CH9434_BAUD_115200,
    .data_bits   = CH9434_DATABITS_8,
    .stop_bits   = CH9434_STOPBITS_1,
    .parity      = CH9434_PARITY_NONE,
    .use_fifo    = true,
    .hw_flow_ctrl = false,
};

#define ROUND_TRIP_WAIT_MS  100
#define TEST_GROUP_COUNT    100

/* -------------------------------------------------------------------------- */
/*                          Random data generation                            */
/* -------------------------------------------------------------------------- */

static void generate_random_data(uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)esp_random();
    }
}

/* -------------------------------------------------------------------------- */
/*                          Per-channel random test                           */
/* -------------------------------------------------------------------------- */

static bool test_loopback_random(uint8_t uart, uint16_t len, uint32_t group)
{
    uint8_t tx_buf[256];
    uint8_t rx_buf[256];

    if (len > sizeof(tx_buf)) {
        len = sizeof(tx_buf);
    }

    generate_random_data(tx_buf, len);

    ESP_LOGI(TAG, "[%3u/%3u] UART%u loopback: sending %4u bytes",
             (unsigned)group, (unsigned)TEST_GROUP_COUNT, uart, (unsigned)len);

    esp_err_t ret = ch9434_uart_write(uart, tx_buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%3u/%3u] UART%u loopback: WRITE FAILED: %s",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, uart, esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(ROUND_TRIP_WAIT_MS));

    ret = ch9434_uart_read_fifo(uart, rx_buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%3u/%3u] UART%u loopback: READ FAILED: %s",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, uart, esp_err_to_name(ret));
        return false;
    }

    if (memcmp(rx_buf, tx_buf, len) != 0) {
        ESP_LOGE(TAG, "[%3u/%3u] UART%u loopback: VERIFY FAILED",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, uart);
        ESP_LOG_BUFFER_HEX(TAG, tx_buf, len > 16 ? 16 : len);
        ESP_LOG_BUFFER_HEX(TAG, rx_buf, len > 16 ? 16 : len);
        uint8_t lsr = 0;
        ch9434_uart_read_lsr(uart, &lsr);
        ch9434_uart_dump_lsr(uart, lsr);
        return false;
    }

    ESP_LOGI(TAG, "[%3u/%3u] UART%u loopback: OK (%4u bytes verified)",
             (unsigned)group, (unsigned)TEST_GROUP_COUNT, uart, (unsigned)len);
    return true;
}

static bool test_cross_random(uint8_t tx_uart, uint8_t rx_uart, uint16_t len, uint32_t group)
{
    uint8_t tx_buf[256];
    uint8_t rx_buf[256];

    if (len > sizeof(tx_buf)) {
        len = sizeof(tx_buf);
    }

    generate_random_data(tx_buf, len);

    ESP_LOGI(TAG, "[%3u/%3u] UART%u->UART%u cross: sending %4u bytes",
             (unsigned)group, (unsigned)TEST_GROUP_COUNT, tx_uart, rx_uart, (unsigned)len);

    esp_err_t ret = ch9434_uart_write(tx_uart, tx_buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%3u/%3u] UART%u->UART%u cross: WRITE FAILED: %s",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, tx_uart, rx_uart, esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(ROUND_TRIP_WAIT_MS));

    ret = ch9434_uart_read_fifo(rx_uart, rx_buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%3u/%3u] UART%u->UART%u cross: READ FAILED: %s",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, tx_uart, rx_uart, esp_err_to_name(ret));
        return false;
    }

    if (memcmp(rx_buf, tx_buf, len) != 0) {
        ESP_LOGE(TAG, "[%3u/%3u] UART%u->UART%u cross: VERIFY FAILED",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, tx_uart, rx_uart);
        ESP_LOG_BUFFER_HEX(TAG, tx_buf, len > 16 ? 16 : len);
        ESP_LOG_BUFFER_HEX(TAG, rx_buf, len > 16 ? 16 : len);
        uint8_t lsr = 0;
        ch9434_uart_read_lsr(rx_uart, &lsr);
        ch9434_uart_dump_lsr(rx_uart, lsr);
        return false;
    }

    ESP_LOGI(TAG, "[%3u/%3u] UART%u->UART%u cross: OK (%4u bytes verified)",
             (unsigned)group, (unsigned)TEST_GROUP_COUNT, tx_uart, rx_uart, (unsigned)len);
    return true;
}

/* -------------------------------------------------------------------------- */
/*                          UART3 background tasks                             */
/* -------------------------------------------------------------------------- */

static void uart3_tx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UART3 TX task started (interval=%d ms)", UART3_TX_INTERVAL_MS);

    char tx_buf[128];

    while (1) {
        int len = snprintf(tx_buf, sizeof(tx_buf),
                           "[UART3] Hello from CH9434! seq=%lu tick=%lu\r\n",
                           (unsigned long)s_uart3_tx_count,
                           (unsigned long)xTaskGetTickCount());

        if (len > 0 && len < sizeof(tx_buf)) {
            esp_err_t ret = ch9434_uart_write(CH9434_UART3, (const uint8_t *)tx_buf, (uint16_t)len);
            if (ret == ESP_OK) {
                s_uart3_tx_count++;
                ESP_LOGI(TAG, "UART3 TX: sent %d bytes (seq=%lu)", len, (unsigned long)(s_uart3_tx_count - 1));
            } else {
                ESP_LOGE(TAG, "UART3 TX: write failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(UART3_TX_INTERVAL_MS));
    }
}

static void uart3_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "UART3 RX task started (poll interval=%d ms)", UART3_RX_POLL_INTERVAL_MS);

    uint8_t rx_buf[UART3_RX_BUF_SIZE];

    while (1) {
        uint16_t available = 0;
        esp_err_t ret = ch9434_uart_available(CH9434_UART3, &available);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "UART3 RX: available query failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(UART3_RX_POLL_INTERVAL_MS));
            continue;
        }

        if (available > 0) {
            if (available > sizeof(rx_buf)) {
                available = sizeof(rx_buf);
            }

            uint16_t read_len = 0;
            ret = ch9434_uart_read(CH9434_UART3, rx_buf, available, &read_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "UART3 RX: read failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(UART3_RX_POLL_INTERVAL_MS));
                continue;
            }

            if (read_len > 0) {
                s_uart3_rx_count += read_len;

                ESP_LOGI(TAG, "UART3 RX: received %u bytes (total=%lu):",
                         read_len, (unsigned long)s_uart3_rx_count);
                ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buf, read_len, ESP_LOG_INFO);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(UART3_RX_POLL_INTERVAL_MS));
    }
}

/* -------------------------------------------------------------------------- */
/*                          Main test entry                                    */
/* -------------------------------------------------------------------------- */

void test_app_run(void)
{
    ESP_LOGI(TAG, "====== CH9434 SPI->4xUART TEST START ======");
    ESP_LOGI(TAG, "Test configuration:");
    ESP_LOGI(TAG, "  - UART0: TX0 <-> RX0 (loopback)");
    ESP_LOGI(TAG, "  - UART1: TX1 -> UART2: RX2 (cross-link)");
    ESP_LOGI(TAG, "  - UART2: TX2 -> UART1: RX1 (cross-link reverse)");
    ESP_LOGI(TAG, "  - UART3: background TX/RX (TX every %d ms, RX poll %d ms)",
             UART3_TX_INTERVAL_MS, UART3_RX_POLL_INTERVAL_MS);
    ESP_LOGI(TAG, "  - Baud rate: 115200 8N1");
    ESP_LOGI(TAG, "  - SPI clock: 200kHz");
    ESP_LOGI(TAG, "  - Test groups: %u", (unsigned)TEST_GROUP_COUNT);
    ESP_LOGI(TAG, "  - Random data length: 1-255 bytes per test");
    ESP_LOGI(TAG, "---------------------------------------------");

    ESP_LOGI(TAG, "Configuring all four sub-UARTs ...");
    for (uint8_t u = 0; u < CH9434_UART_COUNT; u++) {
        esp_err_t ret = ch9434_uart_set_config(u, &k_default_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "FAILED to configure UART%u: %s", u, esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "  UART%u configured: 115200 8N1", u);
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Starting UART3 background tasks ...");
    xTaskCreate(uart3_rx_task, "uart3_rx", 2048, NULL, 5, &s_uart3_rx_task_handle);
    xTaskCreate(uart3_tx_task, "uart3_tx", 2048, NULL, 4, &s_uart3_tx_task_handle);
    ESP_LOGI(TAG, "  UART3 RX task started");
    ESP_LOGI(TAG, "  UART3 TX task started (interval=%d ms)", UART3_TX_INTERVAL_MS);
    ESP_LOGI(TAG, "---------------------------------------------");

    ESP_LOGI(TAG, "Starting tests ...");
    ESP_LOGI(TAG, "---------------------------------------------");

    uint32_t pass_count = 0;
    uint32_t fail_count = 0;

    for (uint32_t group = 1; group <= TEST_GROUP_COUNT; group++) {
        uint16_t len0 = (uint16_t)((esp_random() % 255) + 1);
        uint16_t len12 = (uint16_t)((esp_random() % 255) + 1);
        uint16_t len21 = (uint16_t)((esp_random() % 255) + 1);

        ESP_LOGI(TAG, "[%3u/%3u] ====== GROUP %3u ======",
                 (unsigned)group, (unsigned)TEST_GROUP_COUNT, (unsigned)group);

        bool ok0 = test_loopback_random(CH9434_UART0, len0, group);
        bool ok12 = test_cross_random(CH9434_UART1, CH9434_UART2, len12, group);
        bool ok21 = test_cross_random(CH9434_UART2, CH9434_UART1, len21, group);

        if (ok0 && ok12 && ok21) {
            pass_count++;
            ESP_LOGI(TAG, "[%3u/%3u] RESULT: PASS", (unsigned)group, (unsigned)TEST_GROUP_COUNT);
        } else {
            fail_count++;
            ESP_LOGE(TAG, "[%3u/%3u] RESULT: FAIL", (unsigned)group, (unsigned)TEST_GROUP_COUNT);
        }

        if (group % 10 == 0 || group == TEST_GROUP_COUNT) {
            uint32_t total = pass_count + fail_count;
            uint32_t percent = total > 0 ? (pass_count * 100) / total : 0;
            ESP_LOGI(TAG, "[%3u/%3u] ====== PROGRESS ======",
                     (unsigned)group, (unsigned)TEST_GROUP_COUNT);
            ESP_LOGI(TAG, "[%3u/%3u] Total: %3u | PASS: %3u | FAIL: %3u | Rate: %3u%%",
                     (unsigned)group, (unsigned)TEST_GROUP_COUNT,
                     (unsigned)total, (unsigned)pass_count, (unsigned)fail_count, (unsigned)percent);
            ESP_LOGI(TAG, "[%3u/%3u] Last sizes: U0=%4uB U1->U2=%4uB U2->U1=%4uB",
                     (unsigned)group, (unsigned)TEST_GROUP_COUNT,
                     (unsigned)len0, (unsigned)len12, (unsigned)len21);
            ESP_LOGI(TAG, "[%3u/%3u] ---------------------------------------------",
                     (unsigned)group, (unsigned)TEST_GROUP_COUNT);
        }

        if (fail_count >= 5) {
            ESP_LOGE(TAG, "[%3u/%3u] ====== ABORT ======",
                     (unsigned)group, (unsigned)TEST_GROUP_COUNT);
            ESP_LOGE(TAG, "[%3u/%3u] Too many failures (%u), stopping test early",
                     (unsigned)group, (unsigned)TEST_GROUP_COUNT, (unsigned)fail_count);
            break;
        }
    }

    ESP_LOGI(TAG, "====== TEST COMPLETE ======");
    ESP_LOGI(TAG, "Summary:");
    ESP_LOGI(TAG, "  Total groups:    %3u", (unsigned)TEST_GROUP_COUNT);
    ESP_LOGI(TAG, "  Passed:          %3u", (unsigned)pass_count);
    ESP_LOGI(TAG, "  Failed:          %3u", (unsigned)fail_count);
    ESP_LOGI(TAG, "  Pass rate:       %3u%%", (unsigned)(pass_count * 100 / (pass_count + fail_count)));
    ESP_LOGI(TAG, "---------------------------------------------");

    if (fail_count == 0) {
        ESP_LOGI(TAG, "RESULT: ALL TESTS PASSED ✓");
    } else {
        ESP_LOGE(TAG, "RESULT: SOME TESTS FAILED ✗");
    }
}
