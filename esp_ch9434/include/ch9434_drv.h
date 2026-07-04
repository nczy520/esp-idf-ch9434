/*
 * CH9434 底层寄存器读写辅助函数。
 *
 * 这些函数将 SPI 总线原语封装为命名的按 UART 寄存器访问器，
 * 以提高调用处的可读性。
 */
#ifndef CH9434_DRV_H
#define CH9434_DRV_H

#include <stdint.h>
#include "esp_err.h"
#include "ch9434_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单字节寄存器辅助函数（传入完整地址）。 */
esp_err_t ch9434_write_reg(uint8_t addr, uint8_t val);
esp_err_t ch9434_read_reg(uint8_t addr, uint8_t *val);

/* 修改寄存器（读-改-写），保留指定位不变。 */
esp_err_t ch9434_modify_reg(uint8_t addr, uint8_t clear_mask, uint8_t set_mask);

/* 按 UART 的便捷访问器。 */
esp_err_t ch9434_uart_write_scr(uint8_t uart, uint8_t val);
esp_err_t ch9434_uart_read_scr(uint8_t uart, uint8_t *val);

esp_err_t ch9434_uart_write_lcr(uint8_t uart, uint8_t val);
esp_err_t ch9434_uart_read_lcr(uint8_t uart, uint8_t *val);

esp_err_t ch9434_uart_write_mcr(uint8_t uart, uint8_t val);
esp_err_t ch9434_uart_read_mcr(uint8_t uart, uint8_t *val);

esp_err_t ch9434_uart_read_lsr(uint8_t uart, uint8_t *val);
esp_err_t ch9434_uart_read_iir(uint8_t uart, uint8_t *val);

esp_err_t ch9434_uart_write_dll(uint8_t uart, uint8_t val);
esp_err_t ch9434_uart_write_dlm(uint8_t uart, uint8_t val);
esp_err_t ch9434_uart_read_dll(uint8_t uart, uint8_t *val);
esp_err_t ch9434_uart_read_dlm(uint8_t uart, uint8_t *val);

/* 指定 UART 的 FIFO 计数（只读）。CH9434 将计数拆分为
 * 两个连续的 8 位寄存器（低 / 高）。
 * 注意：RX FIFO 返回已用字节数（可读数据量），
 * TX FIFO 返回空闲字节数（可写空间量）。 */
esp_err_t ch9434_uart_get_rx_fifo_len(uint8_t uart, uint8_t *len_lo, uint8_t *len_hi);
esp_err_t ch9434_uart_get_tx_fifo_len(uint8_t uart, uint8_t *len_lo, uint8_t *len_hi);

/* FIFO 数据输入输出。 */
esp_err_t ch9434_uart_write_fifo(uint8_t uart, const uint8_t *data, uint16_t len);
esp_err_t ch9434_uart_read_fifo(uint8_t uart, uint8_t *data, uint16_t len);

/* 全局时钟控制。 */
esp_err_t ch9434_write_clk_ctrl(uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* CH9434_DRV_H */
