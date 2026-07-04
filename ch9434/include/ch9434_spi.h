/*
 * CH9434 的 SPI 总线抽象层。
 *
 * CH9434 需要 SPI 模式 0（CPOL=0, CPHA=0）主机。每次 CS 低事务
 * 携带一个命令字节和一个数据字节，两者之间有 >1us 的间隔。
 * 本模块使用 ESP-IDF SPI 主机驱动实现该时序。
 *
 * 引脚分配和 SPI 时钟可通过 Kconfig 配置
 * （menuconfig -> CH9434 配置）。默认值：
 *   MOSI = GPIO 11
 *   MISO = GPIO 13
 *   SCK  = GPIO 12
 *   CS   = GPIO 10
 *   CLK  = 2 MHz
 */
#ifndef CH9434_SPI_H
#define CH9434_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CH9434 的默认 SPI 时钟（2 MHz - 为保证通信稳定性采用的频率；
 * CH9434 数据手册标称最大支持 16 MHz）。
 *
 * 运行时实际使用的值来自 CONFIG_CH9434_SPI_CLOCK_HZ（Kconfig）。
 * 保留此宏是为了向后兼容编译时读取它的代码。 */
#ifndef CH9434_SPI_CLOCK_HZ
#ifdef CONFIG_CH9434_SPI_CLOCK_HZ
#define CH9434_SPI_CLOCK_HZ CONFIG_CH9434_SPI_CLOCK_HZ
#else
#define CH9434_SPI_CLOCK_HZ (2 * 1000 * 1000)
#endif
#endif

/**
 * 初始化与 CH9434 通信的 SPI 总线。
 *
 * 引脚和时钟取自 Kconfig（CONFIG_CH9434_PIN_* 和
 * CONFIG_CH9434_SPI_CLOCK_HZ）。可安全多次调用；
 * 总线已初始化后后续调用为无操作。
 *
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_bus_init(void);

/**
 * 反初始化 SPI 总线。可安全多次调用。
 */
void ch9434_spi_bus_deinit(void);

/**
 * 写入单个寄存器字节。
 *
 * @param reg   8 位寄存器地址（不含操作位）。
 * @param val   要写入的值。
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_write_reg(uint8_t reg, uint8_t val);

/**
 * 读取单个寄存器字节。
 *
 * @param reg   8 位寄存器地址（不含操作位）。
 * @param val   接收读取值的输出指针。
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_read_reg(uint8_t reg, uint8_t *val);

/**
 * 向寄存器写入多字节（FIFO 批量写入）。
 *
 * @param reg      首寄存器地址（不含操作位）。
 * @param data     待发送字节指针。
 * @param len      待发送字节数。
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_write_bytes(uint8_t reg, const uint8_t *data, uint16_t len);

/**
 * 从寄存器读取多字节（FIFO 批量读取）。
 *
 * @param reg      寄存器地址（不含操作位）。
 * @param data     输出缓冲区。
 * @param len      待读取字节数。
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_read_bytes(uint8_t reg, uint8_t *data, uint16_t len);

/**
 * 合并请求：获取指定 UART 的 RX/TX FIFO 长度。
 *
 * 在一次队列请求内完成：写 FIFO_CTRL + 读 FIFO_CTRL_L + 读 FIFO_CTRL_H，
 * 比三次独立调用减少 2 次队列切换开销。
 *
 * 注意：RX FIFO 返回已用字节数（可读数据量），
 * TX FIFO 返回空闲字节数（可写空间量）。
 *
 * @param uart      UART 编号（0..3）。
 * @param is_tx     true=查询 TX FIFO, false=查询 RX FIFO。
 * @param fifo_len  输出 FIFO 长度（RX=已用字节, TX=空闲字节）。
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_get_fifo_len(uint8_t uart, bool is_tx, uint16_t *fifo_len);

/**
 * 合并请求：获取 RX FIFO 长度并读取数据，单次队列切换完成。
 *
 * 在一次队列请求内完成：写 FIFO_CTRL 选 UART + 读 FIFO 长度 + 读 FIFO 数据，
 * 比先查长度再读数据减少 1 次队列切换开销。
 *
 * @param uart      UART 编号（0..3）。
 * @param data      输出缓冲区。
 * @param max_len   最大读取字节数（缓冲区大小）。
 * @param out_len   实际读取的字节数输出。
 * @return 成功返回 ESP_OK，否则返回 esp_err_t 错误码。
 */
esp_err_t ch9434_spi_read_fifo(uint8_t uart, uint8_t *data, uint16_t max_len, uint16_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* CH9434_SPI_H */
