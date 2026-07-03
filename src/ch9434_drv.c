/*
 * CH9434 底层寄存器辅助函数 - 实现。
 */
#include <string.h>
#include "esp_log.h"
#include "ch9434_drv.h"
#include "ch9434_spi.h"

#define TAG "ch9434_drv"

esp_err_t ch9434_write_reg(uint8_t addr, uint8_t val)
{
    return ch9434_spi_write_reg(addr, val);
}

esp_err_t ch9434_read_reg(uint8_t addr, uint8_t *val)
{
    return ch9434_spi_read_reg(addr, val);
}

esp_err_t ch9434_modify_reg(uint8_t addr, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t cur = 0;
    esp_err_t ret = ch9434_read_reg(addr, &cur);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t new_val = (uint8_t)((cur & ~clear_mask) | set_mask);
    if (new_val == cur) {
        return ESP_OK;
    }
    return ch9434_write_reg(addr, new_val);
}

esp_err_t ch9434_uart_write_scr(uint8_t uart, uint8_t val)
{
    return ch9434_write_reg(CH9434_ADDR_SCR(uart), val);
}

esp_err_t ch9434_uart_read_scr(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_SCR(uart), val);
}

esp_err_t ch9434_uart_write_lcr(uint8_t uart, uint8_t val)
{
    return ch9434_write_reg(CH9434_ADDR_LCR(uart), val);
}

esp_err_t ch9434_uart_read_lcr(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_LCR(uart), val);
}

esp_err_t ch9434_uart_write_mcr(uint8_t uart, uint8_t val)
{
    return ch9434_write_reg(CH9434_ADDR_MCR(uart), val);
}

esp_err_t ch9434_uart_read_mcr(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_MCR(uart), val);
}

esp_err_t ch9434_uart_read_lsr(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_LSR(uart), val);
}

esp_err_t ch9434_uart_read_iir(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_IIR(uart), val);
}

esp_err_t ch9434_uart_write_dll(uint8_t uart, uint8_t val)
{
    return ch9434_write_reg(CH9434_ADDR_DLL(uart), val);
}

esp_err_t ch9434_uart_write_dlm(uint8_t uart, uint8_t val)
{
    return ch9434_write_reg(CH9434_ADDR_DLM(uart), val);
}

esp_err_t ch9434_uart_read_dll(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_DLL(uart), val);
}

esp_err_t ch9434_uart_read_dlm(uint8_t uart, uint8_t *val)
{
    return ch9434_read_reg(CH9434_ADDR_DLM(uart), val);
}

esp_err_t ch9434_uart_get_rx_fifo_len(uint8_t uart, uint8_t *len_lo, uint8_t *len_hi)
{
    esp_err_t ret;
    /* 选择 FIFO 计数对应的 UART。 */
    ret = ch9434_write_reg(CH9434_FIFO_CTRL, (uint8_t)(uart & CH9434_FIFO_CTRL_UART_MASK));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ch9434_read_reg(CH9434_FIFO_CTRL_L, len_lo);
    if (ret != ESP_OK) {
        return ret;
    }
    return ch9434_read_reg(CH9434_FIFO_CTRL_H, len_hi);
}

esp_err_t ch9434_uart_get_tx_fifo_len(uint8_t uart, uint8_t *len_lo, uint8_t *len_hi)
{
    esp_err_t ret;
    ret = ch9434_write_reg(CH9434_FIFO_CTRL,
                           (uint8_t)(CH9434_FIFO_CTRL_TR | (uart & CH9434_FIFO_CTRL_UART_MASK)));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ch9434_read_reg(CH9434_FIFO_CTRL_L, len_lo);
    if (ret != ESP_OK) {
        return ret;
    }
    return ch9434_read_reg(CH9434_FIFO_CTRL_H, len_hi);
}

esp_err_t ch9434_uart_write_fifo(uint8_t uart, const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    return ch9434_spi_write_bytes(CH9434_ADDR_THR(uart), data, len);
}

esp_err_t ch9434_uart_read_fifo(uint8_t uart, uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    return ch9434_spi_read_bytes(CH9434_ADDR_RBR(uart), data, len);
}

esp_err_t ch9434_write_clk_ctrl(uint8_t val)
{
    return ch9434_write_reg(CH9434_CLK_CTRL_CFG, val);
}
