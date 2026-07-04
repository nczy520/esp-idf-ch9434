# ch9434 - WCH CH9434 SPI 转 4 路 UART 桥接芯片 ESP-IDF 驱动

[![Component](https://img.shields.io/badge/ESP--IDF-6.0%2B-blue)](https://docs.espressif.com/projects/esp-idf/)
[![License](https://img.shields.io/badge/license-Apache--2.0-green)](LICENSE)
[![Target](https://img.shields.io/badge/target-ESP32--S3-orange)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Version](https://img.shields.io/badge/version-1.0.2-blueviolet)](idf_component.yml)

WCH（沁恒微电子）**CH9434** 芯片的 ESP-IDF 生产级驱动。该芯片通过一路 SPI 总线扩展出
**4 路独立 UART 通道**。驱动完全线程安全：由专用的 SPI 服务任务消费请求队列，任意数量的
FreeRTOS 任务可并发调用 API 而不会争用 SPI 外设。

- 支持芯片：**CH9434A** / **CH9434M** / **CH9434D**（协议层功能一致）
- 支持目标：ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C2 / C3 / C5 / C6 / ESP32-H2 / ESP32-P4
- 主测平台：**ESP32-S3**（使用 SPI2 或 SPI3 主机）
- SPI 时钟：最高 80 MHz（默认 10 MHz）
- UART 波特率：1200 - 921600 bps
- 每路 UART FIFO：RX 256 字节 / TX 1536 字节（CH9434A）
- 并发模型：队列 + 服务任务（无互斥锁，无锁竞争）

## 目录

- [项目结构](#项目结构)
- [安装方式](#安装方式)
  - [方式 A：Git 子模块（本地项目推荐）](#方式-agit-子模块本地项目推荐)
  - [方式 B：ESP Component Manager（idf.py add-dependency）](#方式-besp-component-manageridf-py-add-dependency)
  - [方式 C：手动拷贝](#方式-c手动拷贝)
- [硬件连接](#硬件连接)
- [配置选项](#配置选项)
- [快速入门](#快速入门)
- [示例项目](#示例项目)
- [API 参考](#api-参考)
  - [高层 UART API（ch9434_uart.h）](#高层-uart-apich9434_uarth)
  - [SPI 层（ch9434_spi.h）](#spi-层ch9434_spih)
  - [寄存器层（ch9434_drv.h）](#寄存器层ch9434_drvh)
  - [寄存器定义（ch9434_regs.h）](#寄存器定义ch9434_regsh)
- [并发模型](#并发模型)
- [技术参数](#技术参数)
- [许可证](#许可证)

## 项目结构

```
esp-idf-ch9434/                   ← 根目录即为 ch9434 组件
├── include/                       ← 头文件（对外 API）
│   ├── ch9434_uart.h              ← UART 高层 API
│   ├── ch9434_spi.h               ← SPI 总线抽象层
│   ├── ch9434_drv.h               ← 寄存器访问层
│   └── ch9434_regs.h              ← 寄存器映射定义
├── src/                           ← 实现源码
│   ├── ch9434_uart.c              ← UART 配置与收发
│   ├── ch9434_spi.c               ← SPI 时序与串行化
│   └── ch9434_drv.c               ← 寄存器辅助函数
├── examples/
│   └── sample_project/            ← 端到端测试示例（4 路 UART 并发）
│       ├── main/
│       │   ├── main.c
│       │   ├── test_app.c
│       │   └── test_app.h
│       ├── CMakeLists.txt
│       └── sdkconfig.defaults
├── CMakeLists.txt                 ← 组件级 CMake（idf_component_register）
├── idf_component.yml              ← 组件管理器清单
├── Kconfig                        ← 配置菜单
└── README.md
```

## 安装方式

### 方式 A：Git 子模块（本地项目推荐）

在你的 ESP-IDF 项目根目录执行：

```bash
git submodule add https://github.com/nczy520/esp-idf-ch9434.git components/ch9434
```

ESP-IDF 会自动发现 `components/` 目录下的组件，无需额外配置。在你的
`main/CMakeLists.txt` 中声明依赖：

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ch9434
)
```

### 方式 B：ESP Component Manager（idf.py add-dependency）

在你需要的组件（如 `main/`）的 `idf_component.yml` 中添加：

```yaml
dependencies:
  ch9434:
    git: https://github.com/nczy520/esp-idf-ch9434.git
    version: main   # 也可指定 tag，如 v1.0.1
```

执行 `idf.py reconfigure`，组件管理器会自动将组件克隆到
`managed_components/ch9434/` 目录。

### 方式 C：手动拷贝

将本仓库全部文件拷贝到 `<你的项目>/components/ch9434/`，并在你的组件的
`REQUIRES` 列表中添加 `ch9434`。

## 硬件连接

### ESP32-S3 <-> CH9434 SPI 接线

| ESP32-S3 GPIO | CH9434 引脚 | 功能         | 默认值（Kconfig）       |
|---------------|-------------|--------------|-------------------------|
| 11            | MOSI        | 主出从入     | `CONFIG_CH9434_PIN_MOSI` |
| 13            | MISO        | 主入从出     | `CONFIG_CH9434_PIN_MISO` |
| 12            | SCK         | SPI 时钟     | `CONFIG_CH9434_PIN_SCK`  |
| 10            | CS          | 片选（低有效）| `CONFIG_CH9434_PIN_CS`   |
| 3V3           | VCC         | 电源（3.3V） | -                       |
| GND           | GND         | 地线         | -                       |

所有引脚分配可通过 `idf.py menuconfig` -> **Component config -> CH9434 配置** 修改。

### CH9434 UART 引脚

芯片对外提供 4 路独立 UART，每路包含 TX/RX/RTS/CTS：

| UART 编号 | TX  | RX  | RTS | CTS |
|-----------|-----|-----|-----|-----|
| 0         | TX0 | RX0 | RTS0| CTS0|
| 1         | TX1 | RX1 | RTS1| CTS1|
| 2         | TX2 | RX2 | RTS2| CTS2|
| 3         | TX3 | RX3 | RTS3| CTS3|

## 配置选项

所有选项位于 `menuconfig` 的 **Component config -> CH9434 配置** 菜单下：

| Kconfig 选项                    | 默认值 | 说明                                |
|--------------------------------|--------|-------------------------------------|
| `CONFIG_CH9434_SPI_HOST`       | 2      | SPI 外设（SPI2_HOST / SPI3_HOST）   |
| `CONFIG_CH9434_PIN_MOSI`       | 11     | MOSI GPIO                           |
| `CONFIG_CH9434_PIN_MISO`       | 13     | MISO GPIO                           |
| `CONFIG_CH9434_PIN_SCK`        | 12     | SCK GPIO                            |
| `CONFIG_CH9434_PIN_CS`         | 10     | CS GPIO                             |
| `CONFIG_CH9434_SPI_CLOCK_HZ`   | 10000000 | SPI 时钟（Hz，50k-80M）              |
| `CONFIG_CH9434_SPI_QUEUE_SIZE` | 16     | SPI 请求队列深度                    |
| `CONFIG_CH9434_SPI_TASK_STACK` | 3072   | SPI 服务任务栈大小（字节）          |
| `CONFIG_CH9434_SPI_TASK_PRIO`  | 10     | SPI 服务任务优先级（1-24）          |
| `CONFIG_CH9434_TX_CHUNK_SIZE`  | 128    | 每次 TX FIFO 写入字节数             |

## 快速入门

```c
#include "ch9434_uart.h"

void app_main(void)
{
    /* 初始化 SPI 总线并通过 SCR 回环校验芯片通信。 */
    esp_err_t ret = ch9434_chip_init();
    if (ret != ESP_OK) {
        ESP_LOGE("app", "CH9434 init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 将 4 路 UART 全部配置为 115200 8N1 并启用 FIFO。 */
    const ch9434_uart_config_t cfg = {
        .baud         = CH9434_BAUD_115200,
        .data_bits    = CH9434_DATABITS_8,
        .stop_bits    = CH9434_STOPBITS_1,
        .parity       = CH9434_PARITY_NONE,
        .use_fifo     = true,
        .hw_flow_ctrl = false,
    };
    for (uint8_t u = 0; u < CH9434_UART_COUNT; u++) {
        ch9434_uart_set_config(u, &cfg);
    }

    /* 在 UART0 发送数据（测试时把 TX0 接到 RX0 形成回环）。 */
    const uint8_t msg[] = "Hello from CH9434!\r\n";
    ch9434_uart_write(CH9434_UART0, msg, sizeof(msg));

    /* 读取 RX FIFO 中到达的数据。 */
    uint8_t rx_buf[128];
    uint16_t rx_len = 0;
    vTaskDelay(pdMS_TO_TICKS(100));
    ch9434_uart_read(CH9434_UART0, rx_buf, sizeof(rx_buf), &rx_len);
    ESP_LOGI("app", "RX %u bytes", rx_len);
}
```

## 示例项目

完整的端到端测试程序位于 [examples/sample_project/](examples/sample_project/) 目录，
演示了在多 FreeRTOS 任务中同时使用 4 路 UART 的场景，包括：

- **UART0 回环测试**：随机长度（1-255 字节）随机数据回环验证
- **UART1 ↔ UART2 交叉测试**：双向交叉链路数据收发验证
- **UART3 后台任务**：独立的 TX/RX 后台任务验证并发访问安全

### 编译运行示例

```bash
cd examples/sample_project
idf.py build
idf.py flash monitor
```

测试硬件连接：
- TX0 ↔ RX0（UART0 回环）
- TX1 → RX2、TX2 → RX1（UART1 与 UART2 交叉）

## API 参考

### 高层 UART API（ch9434_uart.h）

线程安全。可从多个 FreeRTOS 任务并发调用。

| 函数 | 说明 |
|------|------|
| `ch9434_chip_init()` | 初始化 SPI 总线 + 通过 SCR 回环测试校验芯片通信 |
| `ch9434_uart_set_config(uart, cfg)` | 配置波特率/数据位/停止位/校验/FIFO/流控 |
| `ch9434_uart_write(uart, data, len)` | 发送字节（带 TX FIFO 流控 + 自动分块 + 超时重试） |
| `ch9434_uart_read(uart, data, max, out)` | 从 RX FIFO 读取最多 `max` 字节（单次队列切换优化） |
| `ch9434_uart_available(uart, count)` | 查询 RX FIFO 中等待读取的字节数 |
| `ch9434_uart_tx_free(uart, count)` | 查询 TX FIFO 剩余空闲字节数 |
| `ch9434_uart_dump_lsr(uart, lsr)` | 人可读的 LSR 寄存器转储（调试用） |

**配置结构：**

```c
typedef struct {
    ch9434_baud_t     baud;           // 波特率：1200 ~ 921600
    ch9434_databits_t data_bits;      // 数据位：5/6/7/8
    ch9434_stopbits_t stop_bits;      // 停止位：1/2
    ch9434_parity_t   parity;         // 校验：无/奇/偶
    bool              use_fifo;       // 使能 256 字节 RX FIFO
    bool              hw_flow_ctrl;   // 使能自动 RTS/CTS
} ch9434_uart_config_t;
```

**波特率枚举：** `CH9434_BAUD_1200` / `2400` / `4800` / `9600` / `19200` / `38400` / `57600` / `115200` / `230400` / `460800` / `921600`

### SPI 层（ch9434_spi.h）

底层 SPI 原语。公共 API 已线程安全（内部队列串行化）。

| 函数 | 说明 |
|------|------|
| `ch9434_spi_bus_init()` | 初始化 SPI 总线（`ch9434_chip_init` 会自动调用） |
| `ch9434_spi_bus_deinit()` | 释放 SPI 总线 |
| `ch9434_spi_write_reg(reg, val)` | 写入单个寄存器字节 |
| `ch9434_spi_read_reg(reg, val)` | 读取单个寄存器字节 |
| `ch9434_spi_write_bytes(reg, data, len)` | 批量 FIFO 写入 |
| `ch9434_spi_read_bytes(reg, data, len)` | 批量 FIFO 读取 |

### 寄存器层（ch9434_drv.h）

按 UART 命名的寄存器访问辅助函数，基于 SPI 原语构建。
当高层 UART API 没有暴露你需要的功能时，可使用这些函数进行底层寄存器操作。

| 函数 | 说明 |
|------|------|
| `ch9434_write_reg(addr, val)` | 写入 8 位寄存器（完整地址） |
| `ch9434_read_reg(addr, val)` | 读取 8 位寄存器（完整地址） |
| `ch9434_modify_reg(addr, clear, set)` | 读-改-写寄存器位 |
| `ch9434_uart_write_scr/read_scr(uart, ...)` | SCR 暂存寄存器 |
| `ch9434_uart_write_lcr/read_lcr(uart, ...)` | LCR 线路控制寄存器 |
| `ch9434_uart_write_mcr/read_mcr(uart, ...)` | MCR 调制解调器控制寄存器 |
| `ch9434_uart_read_lsr(uart, val)` | LSR 线路状态寄存器（只读） |
| `ch9434_uart_read_iir(uart, val)` | IIR 中断标识寄存器（只读） |
| `ch9434_uart_write_dll/dlm(uart, val)` | DLL/DLM 除数锁存寄存器 |
| `ch9434_uart_get_rx_fifo_len(uart, lo, hi)` | RX FIFO 数据计数 |
| `ch9434_uart_get_tx_fifo_len(uart, lo, hi)` | TX FIFO 数据计数 |
| `ch9434_uart_write_fifo(uart, data, len)` | 写入 TX FIFO |
| `ch9434_uart_read_fifo(uart, data, len)` | 读取 RX FIFO |
| `ch9434_write_clk_ctrl(val)` | 全局时钟控制寄存器 |

### 寄存器定义（ch9434_regs.h）

完整的 CH9434 寄存器映射，包括：

- 子 UART 寄存器基址偏移（每路 0x10 步长）
- RBR/THR、IER、IIR/FCR、LCR、MCR、LSR、MSR、SCR、DLL/DLM 寄存器定义
- LSR、LCR、FCR、MCR、IER、IIR 等位定义
- 全局寄存器：TNOW_CTRL、FIFO_CTRL、CLK_CTRL、SLEEP_MOD 等

## 并发模型

所有公共 API 调用都经过相同的流程：

1. 调用方构造 `spi_req_t` 并推入 FreeRTOS 队列。
2. 调用方在 `ulTaskNotifyTake` 上阻塞（轻量同步，无互斥锁）。
3. 单一专用的 **SPI 服务任务**（`spi_svc`，默认优先级 10）从队列取出请求
   并原子地执行。
4. 服务任务完成后通过 `xTaskNotifyGive` 通知调用方。

由于只有这一个任务会接触 SPI 硬件，因此永远不会出现并发
`spi_device_transmit` 调用 —— 早期基于互斥锁方案中出现的断言失败在结构上
即不可能发生。批量 FIFO 传输作为单个队列条目提交，因此执行过程不会与
其他 UART 的数据流交错。

### 请求类型

队列支持 6 种请求类型：

| 类型 | 说明 | 队列切换次数 | SPI 传输次数 |
|------|------|:------------:|:------------:|
| `WRITE_REG` | 写入单个寄存器 | 1 | 1 |
| `READ_REG` | 读取单个寄存器 | 1 | 1 |
| `WRITE_BYTES` | FIFO 批量写入（多字节） | 1 | N（每字节 1 次） |
| `READ_BYTES` | FIFO 批量读取（指定长度） | 1 | N（每字节 1 次） |
| `GET_FIFO_LEN` | 查询 RX/TX FIFO 长度（合并 3 步） | 1 | 3 |
| `READ_FIFO` | 查长度 + 读数据（合并 4 步） | 1 | 3 + N |

**优化亮点**：`GET_FIFO_LEN` 和 `READ_FIFO` 是合并请求，将原本需要多次队列切换
的操作压缩为单次，在多任务大数据量场景下显著减少上下文切换开销。
对比优化前：
- `uget_rx_fifo_len`：3 次队列切换 → **1 次**（减少 67%）
- `uart_read`：4 次队列切换 → **1 次**（减少 75%）

### TX 流控

`ch9434_uart_write()` 内置 TX FIFO 流控：
- 每次写入前查询 TX FIFO 剩余空间
- FIFO 满时自动等待并重试（可配置等待间隔和最大重试次数）
- 超时返回 `ESP_ERR_TIMEOUT`，避免数据丢失或死等

## 技术参数

| 参数 | 值 |
|------|-----|
| 系统时钟（默认） | 32 MHz |
| 波特率范围 | 1200 ~ 921600 bps |
| 除数公式 | `DLL_DLM = sys_frequency / 8 / baud` |
| UART 通道数 | 4 |
| RX FIFO 深度 | 256 字节 / 通道 |
| TX FIFO 深度 | 1536 字节 / 通道（CH9434A） |
| FIFO 触发电平 | 1 / 4 / 8 / 14 字节（默认 8） |
| SPI 模式 | 模式 0（CPOL=0, CPHA=0） |
| SPI 时钟范围 | 50 kHz ~ 80 MHz（默认 10 MHz） |
| 硬件流控 | 支持（自动 RTS/CTS） |
| 支持数据位 | 5 / 6 / 7 / 8 |
| 支持停止位 | 1 / 2 |
| 支持校验 | 无 / 奇 / 偶 |

## 许可证

Apache License 2.0，详见 [LICENSE](LICENSE)。
