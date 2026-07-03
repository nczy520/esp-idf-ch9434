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
 *   CLK  = 200 kHz
 */
#ifndef CH9434_SPI_H
#define CH9434_SPI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CH9434 的默认 SPI 时钟（200 kHz - 对于 ESP32-S3 上的 MISO 时序
 * 是安全值；芯片数据手册列出 16 MHz 最大值，但实际上此 MCU 的
 * SPI 外设在约 250 kHz 以上 MISO 边沿就太慢了）。
 *
 * 运行时实际使用的值来自 CONFIG_CH9434_SPI_CLOCK_HZ（Kconfig）。
 * 保留此宏是为了向后兼容编译时读取它的代码。 */
#ifndef CH9434_SPI_CLOCK_HZ
#ifdef CONFIG_CH9434_SPI_CLOCK_HZ
#define CH9434_SPI_CLOCK_HZ CONFIG_CH9434_SPI_CLOCK_HZ
#else
#define CH9434_SPI_CLOCK_HZ (200 * 1000)
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

#ifdef __cplusplus
}
#endif

#endif /* CH9434_SPI_H */
