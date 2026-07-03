/*
 * CH9434 SPI low-level implementation.
 *
 * The CH9434A protocol uses 1 CS-low pulse per register access, with
 * 2 bytes inside that pulse (address byte, then 1us delay, then data
 * byte, then 3us delay, then CS high). For "bulk" FIFO transfers,
 * each byte is its OWN (CS-low) transaction - the WCH reference
 * driver loops ch9434_write_reg per byte.
 *
 *   WRITE: CS low, [addr], 1us, [data], 3us, CS high.
 *   READ : CS low, [addr], 3us, [0xFF dummy -> data on MISO], 1us, CS high.
 *
 * We send both bytes of a single access in ONE spi_device_transmit call
 * (2 bytes = 16 bits, CS held low for the whole transaction), then add
 * the required post-CS delay.
 *
 * --- Concurrency model ---
 * All public API functions enqueue a request to a FreeRTOS queue and block
 * on xTaskNotify until the dedicated SPI service task completes the
 * transaction.  This guarantees that only one task touches the SPI
 * peripheral at a time without any explicit mutex.
 */

#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ch9434_spi.h"
#include "ch9434_regs.h"

#define TAG "ch9434_spi"

/* Pin assignment (ESP32-S3 default SPI2 pins) */
#define PIN_NUM_MOSI    11
#define PIN_NUM_MISO    13
#define PIN_NUM_SCK     12
#define PIN_NUM_CS      10

/* SPI host used on ESP32-S3. SPI0/1 are reserved for flash/PSRAM. */
#define SPI_HOST_CH9434 SPI2_HOST

/* Per-spec delays (microseconds). */
#define CH9434A_DELAY_ADDR_TO_DATA_US  1   /* WRITE: address -> data    */
#define CH9434A_DELAY_DATA_TO_CS_US     3   /* WRITE: data    -> CS high */
#define CH9434A_DELAY_READ_ADDR_US     3   /* READ:  address -> read    */
#define CH9434A_DELAY_READ_DONE_US     1   /* READ:  read    -> CS high */

/* ---------- Queue-based SPI serialisation ---------- */
#define SPI_QUEUE_SIZE     16
#define SPI_TASK_STACK     3072
#define SPI_TASK_PRIO      10   /* high priority to minimise SPI latency */

typedef enum {
    SPI_REQ_WRITE_REG,
    SPI_REQ_READ_REG,
    SPI_REQ_WRITE_BYTES,
    SPI_REQ_READ_BYTES,
} spi_req_type_t;

typedef struct {
    spi_req_type_t  type;
    uint8_t         reg;
    uint8_t         value;          /* WRITE_REG: byte to write            */
    uint8_t        *out_value;      /* READ_REG:  output pointer           */
    const uint8_t  *wdata;          /* WRITE_BYTES: source buffer          */
    uint8_t        *rdata;          /* READ_BYTES: destination buffer      */
    uint16_t        len;            /* BYTES: number of bytes              */
    esp_err_t       result;         /* filled by service task              */
    TaskHandle_t    caller;         /* task to notify on completion        */
} spi_req_t;

static spi_device_handle_t s_dev = NULL;
static bool s_initialized = false;
static QueueHandle_t s_spi_queue = NULL;
static TaskHandle_t s_spi_task_handle = NULL;

/* Forward declaration — defined after bus_init. */
static void spi_service_task(void *arg);

/* -------------------------------------------------------------------------- */
/*                          SPI bus init / deinit                             */
/* -------------------------------------------------------------------------- */

esp_err_t ch9434_spi_bus_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = PIN_NUM_MISO,
        .sclk_io_num     = PIN_NUM_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 16,
    };

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CH9434_SPI_CLOCK_HZ,
        .mode           = 0,                   /* SPI mode 0 (CPOL=0, CPHA=0) per WCH EVT */
        .spics_io_num   = PIN_NUM_CS,
        .queue_size     = 4,
        .flags          = 0,
    };

    esp_err_t ret = spi_bus_initialize(SPI_HOST_CH9434, &bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = spi_bus_add_device(SPI_HOST_CH9434, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI_HOST_CH9434);
        return ret;
    }

    s_spi_queue = xQueueCreate(SPI_QUEUE_SIZE, sizeof(spi_req_t));
    if (s_spi_queue == NULL) {
        ESP_LOGE(TAG, "failed to create SPI request queue");
        spi_bus_remove_device(s_dev);
        spi_bus_free(SPI_HOST_CH9434);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(spi_service_task, "spi_svc", SPI_TASK_STACK,
                    NULL, SPI_TASK_PRIO, &s_spi_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "failed to create SPI service task");
        vQueueDelete(s_spi_queue);
        s_spi_queue = NULL;
        spi_bus_remove_device(s_dev);
        spi_bus_free(SPI_HOST_CH9434);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "CH9434 SPI bus ready (MOSI=%d MISO=%d SCK=%d CS=%d @%d Hz, queue=%d)",
             PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_SCK, PIN_NUM_CS,
             CH9434_SPI_CLOCK_HZ, SPI_QUEUE_SIZE);
    return ESP_OK;
}

void ch9434_spi_bus_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    if (s_spi_task_handle) {
        vTaskDelete(s_spi_task_handle);
        s_spi_task_handle = NULL;
    }
    if (s_spi_queue) {
        vQueueDelete(s_spi_queue);
        s_spi_queue = NULL;
    }
    spi_bus_remove_device(s_dev);
    spi_bus_free(SPI_HOST_CH9434);
    s_dev = NULL;
    s_initialized = false;
}

/* -------------------------------------------------------------------------- */
/*                       Low-level SPI transfer (no locking)                   */
/*                                                                            */
/* Called only from spi_service_task, so no synchronisation is needed here.   */
/* -------------------------------------------------------------------------- */

static esp_err_t ch9434_spi_xfer2(uint8_t op, uint8_t reg, uint8_t data_byte,
                                  uint8_t *rx_byte, uint8_t post_delay_us)
{
    uint8_t tx[2] = { (uint8_t)(op | reg), data_byte };
    uint8_t rx[2] = { 0, 0 };

    spi_transaction_t t = {
        .length    = 16,            /* 2 bytes = 16 bits, CS held low */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t ret = spi_device_transmit(s_dev, &t);
    if (ret != ESP_OK) {
        return ret;
    }
    if (rx_byte) {
        *rx_byte = rx[1];
    }
    if (post_delay_us) {
        ets_delay_us(post_delay_us);
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          SPI service task                                   */
/*                                                                            */
/* Single consumer that drains the request queue and executes each            */
/* transaction atomically.  Because only this task touches the SPI            */
/* hardware, there can never be concurrent access.                            */
/* -------------------------------------------------------------------------- */

static void spi_service_task(void *arg)
{
    (void)arg;
    spi_req_t req;

    ESP_LOGI(TAG, "SPI service task started (prio=%d, queue=%d)",
             SPI_TASK_PRIO, SPI_QUEUE_SIZE);

    while (1) {
        if (xQueueReceive(s_spi_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (req.type) {
        case SPI_REQ_WRITE_REG:
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_WRITE, req.reg,
                                          req.value, NULL,
                                          CH9434A_DELAY_DATA_TO_CS_US);
            break;

        case SPI_REQ_READ_REG:
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, req.reg,
                                          0xFF, req.out_value,
                                          CH9434A_DELAY_DATA_TO_CS_US);
            break;

        case SPI_REQ_WRITE_BYTES:
            req.result = ESP_OK;
            for (uint16_t i = 0; i < req.len; i++) {
                req.result = ch9434_spi_xfer2(CH9434_REG_OP_WRITE, req.reg,
                                              req.wdata[i], NULL,
                                              CH9434A_DELAY_DATA_TO_CS_US);
                if (req.result != ESP_OK) {
                    break;
                }
            }
            break;

        case SPI_REQ_READ_BYTES:
            req.result = ESP_OK;
            for (uint16_t i = 0; i < req.len; i++) {
                req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, req.reg,
                                              0xFF, &req.rdata[i],
                                              CH9434A_DELAY_DATA_TO_CS_US);
                if (req.result != ESP_OK) {
                    break;
                }
            }
            break;
        }

        /* Notify the caller that the transaction is complete.
         * req.result is read by the caller from its stack copy. */
        if (req.caller) {
            xTaskNotifyGive(req.caller);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                       Public API — enqueue & wait                           */
/* -------------------------------------------------------------------------- */

/* Helper: enqueue a request and block until the service task completes it. */
static esp_err_t spi_submit(spi_req_t *req)
{
    req->caller = xTaskGetCurrentTaskHandle();
    /* Clear any stale notification before waiting. */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    if (xQueueSend(s_spi_queue, req, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    /* Block until the SPI service task notifies us. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return req->result;
}

esp_err_t ch9434_spi_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_req_t req = {
        .type  = SPI_REQ_WRITE_REG,
        .reg   = reg,
        .value = val,
    };
    return spi_submit(&req);
}

esp_err_t ch9434_spi_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_req_t req = {
        .type      = SPI_REQ_READ_REG,
        .reg       = reg,
        .out_value = val,
    };
    return spi_submit(&req);
}

/* -------------------------------------------------------------------------- */
/*                          Bulk FIFO transfers                                */
/*                                                                            */
/* The CH9434A does not have a true burst mode; each byte of a FIFO is        */
/* transferred as a separate (CS-low) 2-byte (address+data) transaction.       */
/* The entire bulk operation is submitted as ONE queue entry so that it       */
/* executes atomically inside the SPI service task without interleaving.      */
/* -------------------------------------------------------------------------- */

esp_err_t ch9434_spi_write_bytes(uint8_t reg, const uint8_t *data, uint16_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_req_t req = {
        .type  = SPI_REQ_WRITE_BYTES,
        .reg   = reg,
        .wdata = data,
        .len   = len,
    };
    return spi_submit(&req);
}

esp_err_t ch9434_spi_read_bytes(uint8_t reg, uint8_t *data, uint16_t len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_req_t req = {
        .type  = SPI_REQ_READ_BYTES,
        .reg   = reg,
        .rdata = data,
        .len   = len,
    };
    return spi_submit(&req);
}
