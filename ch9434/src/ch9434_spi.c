/*
 * CH9434 SPI 底层实现。
 *
 * CH9434A 协议每次寄存器访问使用一个 CS 低脉冲，脉冲内包含 2 字节
 *（地址字节 + 1us 延迟 + 数据字节 + 3us 延迟 + CS 拉高）。
 * 对于"批量" FIFO 传输，每个字节都是独立的（CS 低）事务 ——
 * WCH 参考驱动就是每个字节循环调用 ch9434_write_reg。
 *
 *   写操作：CS 低 -> [地址] -> 1us -> [数据] -> 3us -> CS 高。
 *   读操作：CS 低 -> [地址] -> 3us -> [0xFF 空发 -> MISO 上的数据] -> 1us -> CS 高。
 *
 * 我们在一次 spi_device_transmit 调用中发送单次访问的两个字节
 * （2 字节 = 16 位，CS 保持低电平），然后添加所需的 CS 后延迟。
 *
 * --- 并发模型 ---
 * 所有公共 API 函数都将请求入队到 FreeRTOS 队列，并阻塞在
 * xTaskNotify 上，直到专用的 SPI 服务任务完成事务。
 * 这样保证了同一时间只有一个任务访问 SPI 外设，无需显式互斥锁。
 */

#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "ch9434_spi.h"
#include "ch9434_regs.h"

#define TAG "ch9434_spi"

/* 引脚分配 - 通过 Kconfig 配置（menuconfig -> CH9434 配置）。 */
#define PIN_NUM_MOSI        CONFIG_CH9434_PIN_MOSI
#define PIN_NUM_MISO        CONFIG_CH9434_PIN_MISO
#define PIN_NUM_SCK         CONFIG_CH9434_PIN_SCK
#define PIN_NUM_CS          CONFIG_CH9434_PIN_CS
#define SPI_HOST_CH9434     ((spi_host_device_t)CONFIG_CH9434_SPI_HOST)

/* 按规格的延迟（微秒）。 */
#define CH9434A_DELAY_ADDR_TO_DATA_US  1   /* 写操作：地址 -> 数据 */
#define CH9434A_DELAY_DATA_TO_CS_US    3   /* 写操作：数据 -> CS 高 */
#define CH9434A_DELAY_READ_ADDR_US     3   /* 读操作：地址 -> 读数据 */
#define CH9434A_DELAY_READ_DONE_US     1   /* 读操作：读完成 -> CS 高 */

/* FIFO_CTRL 写入后，芯片锁存所选 UART/方向计数器到 FIFO_CTRL_L/H
 * 所需的等待时间。原多次 spi_submit 路径在调用之间天然存在 1+ ms 的
 * RTOS 调度延时，合并请求消去了这一延时，因此此处显式补足。 */
#define CH9434_FIFO_CTRL_SETTLE_US     5

/* ---------- 基于队列的 SPI 串行化 ---------- */
#define SPI_QUEUE_SIZE      CONFIG_CH9434_SPI_QUEUE_SIZE
#define SPI_TASK_STACK      CONFIG_CH9434_SPI_TASK_STACK
#define SPI_TASK_PRIO       CONFIG_CH9434_SPI_TASK_PRIO

/* SPI 时钟 - 通过 Kconfig 配置。头文件仍然导出 CH9434_SPI_CLOCK_HZ
 * 宏供客户代码读取；内部始终使用 Kconfig 值以保持两者同步。 */
#undef  CH9434_SPI_CLOCK_HZ
#define CH9434_SPI_CLOCK_HZ CONFIG_CH9434_SPI_CLOCK_HZ

typedef enum {
    SPI_REQ_WRITE_REG,
    SPI_REQ_READ_REG,
    SPI_REQ_WRITE_BYTES,
    SPI_REQ_READ_BYTES,
    SPI_REQ_GET_FIFO_LEN,
    SPI_REQ_READ_FIFO,
} spi_req_type_t;

typedef struct {
    spi_req_type_t  type;
    uint8_t         reg;
    uint8_t         value;          /* WRITE_REG: 待写入字节          */
    uint8_t        *out_value;      /* READ_REG:  输出指针            */
    const uint8_t  *wdata;          /* WRITE_BYTES: 源数据缓冲区      */
    uint8_t        *rdata;          /* READ_BYTES / READ_FIFO: 目的缓冲区 */
    uint16_t        len;            /* BYTES: 字节数量 / READ_FIFO: max_len */
    uint16_t       *out_len;        /* READ_FIFO: 实际读取字节数      */
    uint16_t       *fifo_len;       /* GET_FIFO_LEN: FIFO 长度输出    */
    uint8_t         uart;           /* GET_FIFO_LEN / READ_FIFO: UART 编号 */
    bool            is_tx;          /* GET_FIFO_LEN: true=TX, false=RX */
    esp_err_t       result;         /* 由服务任务填充                 */
    TaskHandle_t    caller;         /* 完成后通知的任务               */
} spi_req_t;

static spi_device_handle_t s_dev = NULL;
static bool s_initialized = false;
static QueueHandle_t s_spi_queue = NULL;
static TaskHandle_t s_spi_task_handle = NULL;
static SemaphoreHandle_t s_init_mutex = NULL;

/* 前置声明 - 在 bus_init 之后定义。 */
static void spi_service_task(void *arg);

/* -------------------------------------------------------------------------- */
/*                          SPI 总线初始化 / 反初始化                         */
/* -------------------------------------------------------------------------- */

esp_err_t ch9434_spi_bus_init(void)
{
    if (s_init_mutex == NULL) {
        s_init_mutex = xSemaphoreCreateMutex();
        if (s_init_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_init_mutex, portMAX_DELAY);

    if (s_initialized) {
        xSemaphoreGive(s_init_mutex);
        return ESP_OK;
    }

    gpio_config_t cs_gpio_cfg = {
        .pin_bit_mask = (1ULL << PIN_NUM_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_gpio_cfg);
    gpio_set_level(PIN_NUM_CS, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = PIN_NUM_MISO,
        .sclk_io_num     = PIN_NUM_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CH9434_SPI_CLOCK_HZ,
        .mode           = 0,                   /* SPI 模式 0 (CPOL=0, CPHA=0) 参考 WCH EVT */
        .spics_io_num   = -1,                  /* 手动控制 CS，以便在地址和数据之间插入延时 */
        .queue_size     = 4,
        .flags          = 0,
    };

    esp_err_t ret = spi_bus_initialize(SPI_HOST_CH9434, &bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize 失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_init_mutex);
        return ret;
    }

    ret = spi_bus_add_device(SPI_HOST_CH9434, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device 失败: %s", esp_err_to_name(ret));
        spi_bus_free(SPI_HOST_CH9434);
        xSemaphoreGive(s_init_mutex);
        return ret;
    }

    s_spi_queue = xQueueCreate(SPI_QUEUE_SIZE, sizeof(spi_req_t));
    if (s_spi_queue == NULL) {
        ESP_LOGE(TAG, "创建 SPI 请求队列失败");
        spi_bus_remove_device(s_dev);
        spi_bus_free(SPI_HOST_CH9434);
        s_dev = NULL;
        xSemaphoreGive(s_init_mutex);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(spi_service_task, "spi_svc", SPI_TASK_STACK,
                    NULL, SPI_TASK_PRIO, &s_spi_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "创建 SPI 服务任务失败");
        vQueueDelete(s_spi_queue);
        s_spi_queue = NULL;
        spi_bus_remove_device(s_dev);
        spi_bus_free(SPI_HOST_CH9434);
        s_dev = NULL;
        xSemaphoreGive(s_init_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    xSemaphoreGive(s_init_mutex);
    ESP_LOGI(TAG, "CH9434 SPI 总线就绪 (MOSI=%d MISO=%d SCK=%d CS=%d @%d Hz, 队列=%d)",
             PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_SCK, PIN_NUM_CS,
             CH9434_SPI_CLOCK_HZ, SPI_QUEUE_SIZE);
    return ESP_OK;
}

void ch9434_spi_bus_deinit(void)
{
    if (s_init_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_init_mutex, portMAX_DELAY);

    if (!s_initialized) {
        xSemaphoreGive(s_init_mutex);
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
    xSemaphoreGive(s_init_mutex);
}

/* -------------------------------------------------------------------------- */
/*                       底层 SPI 传输（无锁）                                 */
/*                                                                            */
/* 仅由 spi_service_task 调用，因此此处不需要任何同步。                       */
/* -------------------------------------------------------------------------- */

static esp_err_t ch9434_spi_xfer2(uint8_t op, uint8_t reg, uint8_t data_byte,
                                  uint8_t *rx_byte, uint8_t post_delay_us)
{
    uint8_t addr = (uint8_t)(op | reg);
    uint8_t rx = 0;

    gpio_set_level(PIN_NUM_CS, 0);

    spi_transaction_t t_addr = {
        .length = 8,
        .flags  = SPI_TRANS_USE_TXDATA,
    };
    t_addr.tx_data[0] = addr;

    esp_err_t ret = spi_device_transmit(s_dev, &t_addr);
    if (ret != ESP_OK) {
        gpio_set_level(PIN_NUM_CS, 1);
        return ret;
    }

    uint8_t inter_delay = (op == CH9434_REG_OP_WRITE)
                          ? CH9434A_DELAY_ADDR_TO_DATA_US
                          : CH9434A_DELAY_READ_ADDR_US;
    ets_delay_us(inter_delay);

    spi_transaction_t t_data = {
        .length = 8,
        .flags  = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
    };
    t_data.tx_data[0] = data_byte;

    ret = spi_device_transmit(s_dev, &t_data);
    if (ret != ESP_OK) {
        gpio_set_level(PIN_NUM_CS, 1);
        return ret;
    }
    if (rx_byte) {
        *rx_byte = t_data.rx_data[0];
    }

    if (post_delay_us) {
        ets_delay_us(post_delay_us);
    }

    gpio_set_level(PIN_NUM_CS, 1);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          SPI 服务任务                                       */
/*                                                                            */
/* 唯一的消费者，从请求队列中取出并原子地执行每个事务。                       */
/* 因为只有此任务接触 SPI 硬件，所以永远不会出现并发访问。                    */
/* -------------------------------------------------------------------------- */

static void spi_service_task(void *arg)
{
    (void)arg;
    spi_req_t req;

    ESP_LOGI(TAG, "SPI 服务任务已启动 (优先级=%d, 队列=%d)",
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
                                          CH9434A_DELAY_READ_DONE_US);
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
                                              CH9434A_DELAY_READ_DONE_US);
                if (req.result != ESP_OK) {
                    break;
                }
            }
            break;

        case SPI_REQ_GET_FIFO_LEN: {
            uint8_t fifo_ctrl = (uint8_t)(req.uart & CH9434_FIFO_CTRL_UART_MASK);
            if (req.is_tx) {
                fifo_ctrl |= CH9434_FIFO_CTRL_TR;
            }
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_WRITE, CH9434_FIFO_CTRL,
                                          fifo_ctrl, NULL,
                                          CH9434A_DELAY_DATA_TO_CS_US);
            if (req.result != ESP_OK) {
                break;
            }
            /* 等待芯片根据 FIFO_CTRL 选定的 UART/方向更新 FIFO_CTRL_L/H 计数器。
             * 合并请求省去了原本多次 spi_submit 之间的 RTOS 调度延时，
             * 但芯片需要约 5us 才能将所选 FIFO 计数器锁存到 L/H 寄存器。 */
            ets_delay_us(CH9434_FIFO_CTRL_SETTLE_US);
            uint8_t lo = 0, hi = 0;
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, CH9434_FIFO_CTRL_L,
                                          0xFF, &lo,
                                          CH9434A_DELAY_READ_DONE_US);
            if (req.result != ESP_OK) {
                break;
            }
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, CH9434_FIFO_CTRL_H,
                                          0xFF, &hi,
                                          CH9434A_DELAY_READ_DONE_US);
            if (req.result == ESP_OK && req.fifo_len) {
                *req.fifo_len = (uint16_t)((hi << 8) | lo);
            }
            break;
        }

        case SPI_REQ_READ_FIFO: {
            uint8_t fifo_ctrl = (uint8_t)(req.uart & CH9434_FIFO_CTRL_UART_MASK);
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_WRITE, CH9434_FIFO_CTRL,
                                          fifo_ctrl, NULL,
                                          CH9434A_DELAY_DATA_TO_CS_US);
            if (req.result != ESP_OK) {
                break;
            }
            /* 同上：等待 FIFO_CTRL_L/H 更新（见 SPI_REQ_GET_FIFO_LEN）。 */
            ets_delay_us(CH9434_FIFO_CTRL_SETTLE_US);
            uint8_t lo = 0, hi = 0;
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, CH9434_FIFO_CTRL_L,
                                          0xFF, &lo,
                                          CH9434A_DELAY_READ_DONE_US);
            if (req.result != ESP_OK) {
                break;
            }
            req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, CH9434_FIFO_CTRL_H,
                                          0xFF, &hi,
                                          CH9434A_DELAY_READ_DONE_US);
            if (req.result != ESP_OK) {
                break;
            }
            uint16_t available = (uint16_t)((hi << 8) | lo);
            if (available > req.len) {
                available = req.len;
            }
            ESP_LOGD(TAG, "READ_FIFO uart=%u RX lo=0x%02X hi=0x%02X -> available=%u (cap=%u)",
                     (unsigned)req.uart, lo, hi, (unsigned)available, (unsigned)req.len);
            if (req.out_len) {
                *req.out_len = available;
            }
            if (available == 0) {
                req.result = ESP_OK;
                break;
            }
            uint8_t rbr_addr = CH9434_ADDR_RBR(req.uart);
            req.result = ESP_OK;
            for (uint16_t i = 0; i < available; i++) {
                req.result = ch9434_spi_xfer2(CH9434_REG_OP_READ, rbr_addr,
                                              0xFF, &req.rdata[i],
                                              CH9434A_DELAY_READ_DONE_US);
                if (req.result != ESP_OK) {
                    if (req.out_len) {
                        *req.out_len = i;
                    }
                    break;
                }
            }
            break;
        }
        }

        /* 通知调用者事务已完成。
         * req.result 由调用者从其栈副本中读取。 */
        if (req.caller) {
            xTaskNotifyGive(req.caller);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                       公共 API — 入队并等待                                 */
/* -------------------------------------------------------------------------- */

/* 辅助函数：将请求入队并阻塞，直到服务任务完成它。 */
static esp_err_t spi_submit(spi_req_t *req)
{
    req->caller = xTaskGetCurrentTaskHandle();
    /* 在等待前清除任何过期的通知。 */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    if (xQueueSend(s_spi_queue, req, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    /* 阻塞直到 SPI 服务任务通知我们。 */
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
/*                          批量 FIFO 传输                                     */
/*                                                                            */
/* CH9434A 没有真正的突发模式；FIFO 的每个字节都作为单独的（CS 低）          */
/* 2 字节（地址+数据）事务传输。整个批量操作作为单个队列条目提交，            */
/* 这样它在 SPI 服务任务内原子地执行，不会被其他任务交错。                    */
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

esp_err_t ch9434_spi_get_fifo_len(uint8_t uart, bool is_tx, uint16_t *fifo_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uart >= CH9434_UART_COUNT || fifo_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_req_t req = {
        .type     = SPI_REQ_GET_FIFO_LEN,
        .uart     = uart,
        .is_tx    = is_tx,
        .fifo_len = fifo_len,
    };
    return spi_submit(&req);
}

esp_err_t ch9434_spi_read_fifo(uint8_t uart, uint8_t *data, uint16_t max_len, uint16_t *out_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uart >= CH9434_UART_COUNT || data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;
    if (max_len == 0) {
        return ESP_OK;
    }
    spi_req_t req = {
        .type    = SPI_REQ_READ_FIFO,
        .uart    = uart,
        .rdata   = data,
        .len     = max_len,
        .out_len = out_len,
    };
    return spi_submit(&req);
}
