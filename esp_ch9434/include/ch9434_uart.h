/*
 * CH9434 UART 高层 API：芯片初始化、子 UART 配置与 IO。
 *
 * 芯片型号：CH9434A 时序（CH9434M 是重新封装的 CH9434A）。
 *
 * 线程安全：此头文件中的所有入口函数（chip_init、set_config、
 * write、read、available）都可以从多个 FreeRTOS 任务中并发安全
 * 调用。SPI 访问由内部的专用服务任务串行化。
 */
#ifndef CH9434_UART_H
#define CH9434_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ch9434_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CH9434A 串口参考时钟（芯片默认时钟配置下）：
 * sys_frequency = 32 MHz。 */
#define CH9434_SYS_FREQ_HZ                  32000000UL

/* 公共波特率选择值。标准 PC 常用值。 */
typedef enum {
    CH9434_BAUD_1200    = 1200,
    CH9434_BAUD_2400    = 2400,
    CH9434_BAUD_4800    = 4800,
    CH9434_BAUD_9600    = 9600,
    CH9434_BAUD_19200   = 19200,
    CH9434_BAUD_38400   = 38400,
    CH9434_BAUD_57600   = 57600,
    CH9434_BAUD_115200  = 115200,
    CH9434_BAUD_230400  = 230400,
    CH9434_BAUD_460800  = 460800,
    CH9434_BAUD_921600  = 921600,
} ch9434_baud_t;

typedef enum {
    CH9434_PARITY_NONE = 0,
    CH9434_PARITY_ODD,
    CH9434_PARITY_EVEN,
} ch9434_parity_t;

typedef enum {
    CH9434_STOPBITS_1 = 1,
    CH9434_STOPBITS_2 = 2,
} ch9434_stopbits_t;

typedef enum {
    CH9434_DATABITS_5 = 5,
    CH9434_DATABITS_6 = 6,
    CH9434_DATABITS_7 = 7,
    CH9434_DATABITS_8 = 8,
} ch9434_databits_t;

typedef struct {
    ch9434_baud_t     baud;
    ch9434_databits_t data_bits;
    ch9434_stopbits_t stop_bits;
    ch9434_parity_t   parity;
    bool              use_fifo;       /* 使能硬件 FIFO（RX 256 字节，TX 1536 字节） */
    bool              hw_flow_ctrl;   /* 使能自动 RTS/CTS（已接线时） */
} ch9434_uart_config_t;

/**
 * 初始化 CH9434 芯片。
 *
 * 执行 SPI 总线初始化，然后运行推荐的启动序列
 * （时钟模式寄存器写入 + 延迟），并通过向 UART0 暂存寄存器
 * 写入 0x55 并回读的方式验证通信。
 *
 * @return 与 CH9434 的 SPI 通信正常返回 ESP_OK，否则返回错误。
 */
esp_err_t ch9434_chip_init(void);

/**
 * 配置一个子 UART（0..3）。如果通道处于休眠模式则唤醒。
 */
esp_err_t ch9434_uart_set_config(uint8_t uart, const ch9434_uart_config_t *cfg);

/**
 * 通过子 UART 发送字节。自动将负载分块以适配 TX FIFO
 * （分块大小可通过 Kconfig 配置）。
 */
esp_err_t ch9434_uart_write(uint8_t uart, const uint8_t *data, uint16_t len);

/**
 * 从子 UART RX FIFO 读取最多 `max_len` 字节。
 * 通过 `out_len` 返回实际读取的字节数。
 */
esp_err_t ch9434_uart_read(uint8_t uart, uint8_t *data, uint16_t max_len, uint16_t *out_len);

/**
 * 便捷函数：RX FIFO 中可读取的字节数。
 */
esp_err_t ch9434_uart_available(uint8_t uart, uint16_t *count);

/**
 * 查询 TX FIFO 中剩余的空闲空间（可写入的字节数）。
 */
esp_err_t ch9434_uart_tx_free(uint8_t uart, uint16_t *count);

/**
 * 打印 LSR 寄存器（用于调试帧错误等）。
 */
void ch9434_uart_dump_lsr(uint8_t uart, uint8_t lsr);

#ifdef __cplusplus
}
#endif

#endif /* CH9434_UART_H */
