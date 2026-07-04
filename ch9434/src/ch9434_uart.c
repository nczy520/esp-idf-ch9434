/*
 * CH9434 UART 高层 API - 实现。
 *
 * 芯片型号：CH9434A 时序（CH9434M 在功能上等效）。
 * 参考时钟默认为 32 MHz。除数按 WCH 公式计算：
 * DLL_DLM = sys_frequency / 8 / baud
 * （芯片在应用 16 位除数前，内部先将参考时钟除以 8）。
 */
#include <string.h>
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ch9434_uart.h"
#include "ch9434_drv.h"
#include "ch9434_spi.h"

#define TAG "ch9434_uart"

/* TX 分块大小 - 通过 Kconfig 配置。 */
#ifndef CONFIG_CH9434_TX_CHUNK_SIZE
#define CH9434_TX_CHUNK_SIZE 128
#else
#define CH9434_TX_CHUNK_SIZE CONFIG_CH9434_TX_CHUNK_SIZE
#endif

/* TX FIFO 容量（CH9434A 为 1536 字节）。 */
#define CH9434_TX_FIFO_SIZE       1536U
/* RX FIFO 容量（CH9434A 为 256 字节）。 */
#define CH9434_RX_FIFO_SIZE       256U

/* TX 流控等待的最小间隔（毫秒）。当 TX FIFO 空间不足时，
 * 每次重试前等待的时间。 */
#ifndef CONFIG_CH9434_TX_WAIT_MS
#define CH9434_TX_WAIT_MS         5
#else
#define CH9434_TX_WAIT_MS         CONFIG_CH9434_TX_WAIT_MS
#endif

/* TX 写入最大重试次数。超过则返回失败，避免死等。 */
#ifndef CONFIG_CH9434_TX_MAX_RETRIES
#define CH9434_TX_MAX_RETRIES     200
#else
#define CH9434_TX_MAX_RETRIES     CONFIG_CH9434_TX_MAX_RETRIES
#endif

/* -------------------------------------------------------------------------- */
/*                          芯片启动初始化                                     */
/* -------------------------------------------------------------------------- */

esp_err_t ch9434_chip_init(void)
{
    esp_err_t ret = ch9434_spi_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    /* WCH 调试注意：上电后芯片需要时间完成完全复位，
     * 在此之前任何寄存器访问都可能导致时钟配置
     * （以及后续的 UART 设置）无法生效。 */
    ets_delay_us(100000);  /* 100 毫秒 */

    /* （时钟配置暂时保留芯片默认值；确认基本 SPI 通信正常后
     * 再重新启用倍频器。） */
    ret = ch9434_write_clk_ctrl(0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入 CLK_CTRL_CFG 失败");
        return ret;
    }
    ets_delay_us(50000);

    /* SPI 快速自检：向 UART0 SCR 写入 0x55 并回读。 */
    ret = ch9434_uart_write_scr(CH9434_UART0, 0x55);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t v = 0;
    ret = ch9434_uart_read_scr(CH9434_UART0, &v);
    if (ret != ESP_OK) {
        return ret;
    }
    if (v != 0x55) {
        ESP_LOGE(TAG, "SCR 回环测试失败: 读取 0x%02X，期望 0x55", v);
        return ESP_FAIL;
    }

    /* 再换一个值确认。 */
    ret = ch9434_uart_write_scr(CH9434_UART0, 0xA3);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ch9434_uart_read_scr(CH9434_UART0, &v);
    if (ret != ESP_OK) {
        return ret;
    }
    if (v != 0xA3) {
        ESP_LOGE(TAG, "SCR 回环测试失败: 读取 0x%02X，期望 0xA3", v);
        return ESP_FAIL;
    }

    /* 排空 SCR 写入可能残留在 RX FIFO 中的任何垃圾数据
     * （例如芯片启动时的陈旧数据，或 SCR 写入被芯片
     * 意外回显到 FIFO 中的数据）。 */
    for (int u = 0; u < CH9434_UART_COUNT; u++) {
        uint8_t junk[64];
        uint16_t got = 0;
        ch9434_uart_read(u, junk, sizeof(junk), &got);
        ESP_LOGI(TAG, "排空 UART%d: %u 字节", u, got);
    }

    ESP_LOGI(TAG, "CH9434 通信正常（SCR 测试通过）");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          单路 UART 配置                                     */
/* -------------------------------------------------------------------------- */

esp_err_t ch9434_uart_set_config(uint8_t uart, const ch9434_uart_config_t *cfg)
{
    if (uart >= CH9434_UART_COUNT || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 重新配置期间禁用所有中断。 */
    esp_err_t ret = ch9434_write_reg(CH9434_ADDR_IER(uart), 0x00);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 构造 LCR 值。 */
    uint8_t lcr = 0;
    switch (cfg->data_bits) {
    case CH9434_DATABITS_5: lcr |= CH9434_LCR_WLS_5; break;
    case CH9434_DATABITS_6: lcr |= CH9434_LCR_WLS_6; break;
    case CH9434_DATABITS_7: lcr |= CH9434_LCR_WLS_7; break;
    case CH9434_DATABITS_8: lcr |= CH9434_LCR_WLS_8; break;
    default:                lcr |= CH9434_LCR_WLS_8; break;
    }
    if (cfg->stop_bits == CH9434_STOPBITS_2) {
        lcr |= CH9434_LCR_STOP;
    }
    switch (cfg->parity) {
    case CH9434_PARITY_ODD:  lcr |= CH9434_LCR_PARITY_EN;                 break;
    case CH9434_PARITY_EVEN: lcr |= CH9434_LCR_PARITY_EN | CH9434_LCR_PARITY_EVEN; break;
    case CH9434_PARITY_NONE: default: break;
    }

    /* 设置 DLAB 以访问 DLL/DLM。 */
    ret = ch9434_write_reg(CH9434_ADDR_LCR(uart), (uint8_t)(lcr | CH9434_UARTx_BIT_DLAB));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 除数 = sys_frequency / 8 / baud。使用四舍五入。 */
    uint32_t div = (10UL * CH9434_SYS_FREQ_HZ / 8U / (uint32_t)cfg->baud + 5U) / 10U;
    if (div == 0) {
        div = 1;
    }
    if (div > 0xFFFF) {
        div = 0xFFFF;
    }
    ret = ch9434_uart_write_dll(uart, (uint8_t)(div & 0xFF));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ch9434_uart_write_dlm(uart, (uint8_t)((div >> 8) & 0xFF));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 清除 DLAB 并写入最终 LCR。 */
    ret = ch9434_write_reg(CH9434_ADDR_LCR(uart), lcr);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 复位并启用 FIFO，使用中等触发电平。 */
    uint8_t fcr = CH9434_FCR_RX_RESET | CH9434_FCR_TX_RESET;
    if (cfg->use_fifo) {
        fcr |= CH9434_FCR_ENABLE | CH9434_FCR_TRIG_8;
    }
    ret = ch9434_write_reg(CH9434_ADDR_FCR(uart), fcr);
    if (ret != ESP_OK) {
        return ret;
    }
    /* WCH 注意：FIFO 复位位后，等待 50us 再进行下一操作。 */
    ets_delay_us(50);

    /* MCR: 使能 OUT2 以便 IRQ 引脚（若使用）正确门控，置 RTS/DTR 为高。 */
    uint8_t mcr = CH9434_MCR_RTS | CH9434_MCR_DTR | CH9434_MCR_OUT2;
    if (cfg->hw_flow_ctrl) {
        mcr |= CH9434_MCR_AFE;
    }
    ret = ch9434_write_reg(CH9434_ADDR_MCR(uart), mcr);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 使能 RX 中断，以便芯片可以标记收到的数据。 */
    ret = ch9434_write_reg(CH9434_ADDR_IER(uart), CH9434_IER_RX);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "UART%u 配置完成: %u bps, %u%c%u (除数=%lu, 系统时钟=%lu Hz)",
             uart,
             (unsigned)cfg->baud,
             (unsigned)cfg->data_bits,
             (cfg->parity == CH9434_PARITY_NONE) ? 'N' :
             (cfg->parity == CH9434_PARITY_ODD)  ? 'O' : 'E',
             (cfg->stop_bits == CH9434_STOPBITS_1) ? 1 : 2,
             (unsigned long)div,
             (unsigned long)CH9434_SYS_FREQ_HZ);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          读写辅助函数                                       */
/* -------------------------------------------------------------------------- */

esp_err_t ch9434_uart_write(uint8_t uart, const uint8_t *data, uint16_t len)
{
    if (uart >= CH9434_UART_COUNT || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    uint16_t offset = 0;
    uint32_t retries = 0;

    while (offset < len) {
        uint16_t remaining = len - offset;

        /* 查询 TX FIFO 剩余空闲空间。
         * 注意：CH9434 的 TX FIFO 长度寄存器返回的是空闲字节数，
         * 而不是已用字节数（与 RX FIFO 方向相反）。 */
        uint16_t tx_free = 0;
        esp_err_t ret = ch9434_spi_get_fifo_len(uart, true, &tx_free);
        if (ret != ESP_OK) {
            return ret;
        }
        if (tx_free > CH9434_TX_FIFO_SIZE) {
            tx_free = CH9434_TX_FIFO_SIZE;
        }

        if (tx_free == 0) {
            /* TX FIFO 满，等待并重试。 */
            if (retries >= CH9434_TX_MAX_RETRIES) {
                ESP_LOGW(TAG, "UART%u TX FIFO 超时，已写入 %u/%u 字节",
                         uart, (unsigned)offset, (unsigned)len);
                return ESP_ERR_TIMEOUT;
            }
            retries++;
            vTaskDelay(pdMS_TO_TICKS(CH9434_TX_WAIT_MS));
            continue;
        }
        retries = 0;

        /* 计算本次写入的字节数，不超过分块大小和 FIFO 剩余空间。 */
        uint16_t chunk = remaining;
        if (chunk > tx_free) {
            chunk = tx_free;
        }
        if (chunk > CH9434_TX_CHUNK_SIZE) {
            chunk = CH9434_TX_CHUNK_SIZE;
        }

        ret = ch9434_uart_write_fifo(uart, &data[offset], chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        offset = (uint16_t)(offset + chunk);
    }
    return ESP_OK;
}

esp_err_t ch9434_uart_read(uint8_t uart, uint8_t *data, uint16_t max_len, uint16_t *out_len)
{
    if (uart >= CH9434_UART_COUNT || data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_len = 0;
    if (max_len == 0) {
        return ESP_OK;
    }

    return ch9434_spi_read_fifo(uart, data, max_len, out_len);
}

esp_err_t ch9434_uart_available(uint8_t uart, uint16_t *count)
{
    if (uart >= CH9434_UART_COUNT || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ch9434_spi_get_fifo_len(uart, false, count);
}

esp_err_t ch9434_uart_tx_free(uint8_t uart, uint16_t *count)
{
    if (uart >= CH9434_UART_COUNT || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* CH9434 TX FIFO 长度寄存器直接返回空闲字节数。 */
    uint16_t tx_free = 0;
    esp_err_t ret = ch9434_spi_get_fifo_len(uart, true, &tx_free);
    if (ret != ESP_OK) {
        return ret;
    }
    *count = (tx_free < CH9434_TX_FIFO_SIZE) ? tx_free : CH9434_TX_FIFO_SIZE;
    return ESP_OK;
}

void ch9434_uart_dump_lsr(uint8_t uart, uint8_t lsr)
{
    ESP_LOGW(TAG, "UART%u LSR=0x%02X%s%s%s%s%s%s%s",
             uart, lsr,
             (lsr & CH9434_LSR_DR)   ? " DR"   : "",
             (lsr & CH9434_LSR_OE)   ? " OE"   : "",
             (lsr & CH9434_LSR_PE)   ? " PE"   : "",
             (lsr & CH9434_LSR_FE)   ? " FE"   : "",
             (lsr & CH9434_LSR_BI)   ? " BI"   : "",
             (lsr & CH9434_LSR_THRE) ? " THRE" : "",
             (lsr & CH9434_LSR_TEMT) ? " TEMT" : "",
             (lsr & CH9434_LSR_FERR) ? " FERR" : "");
}
