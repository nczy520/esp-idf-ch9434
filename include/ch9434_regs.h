/*
 * CH9434 寄存器映射（SPI 转 4 路 UART 桥接芯片所用子集）
 *
 * 参考：WCH CH9434A/CH9434D 寄存器定义（CH9434.H）。
 * CH9434M 遵循 CH9434A 风格的时序/协议。
 *
 * 每个子 UART 在寄存器空间内偏移 0x10。
 */
#ifndef CH9434_REGS_H
#define CH9434_REGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 操作码（地址字节的高位） ---------- */
#define CH9434_REG_OP_WRITE                 0x80
#define CH9434_REG_OP_READ                  0x00

/* ---------- 子 UART 寄存器偏移（步长 0x10） ---------- */
#define CH9434_UART0_REG_OFFSET             0x00
#define CH9434_UART1_REG_OFFSET             0x10
#define CH9434_UART2_REG_OFFSET             0x20
#define CH9434_UART3_REG_OFFSET             0x30

/* 每路 UART 寄存器偏移（相对于该 UART 的基地址） */
#define CH9434_UARTx_RBR                    0x00  /* 只读 - 接收缓冲 (DLAB=0) */
#define CH9434_UARTx_THR                    0x00  /* 只写 - 发送保持 (DLAB=0) */
#define CH9434_UARTx_IER                    0x01  /* 读写 - 中断使能 (DLAB=0) */
#define CH9434_UARTx_IIR                    0x02  /* 只读 - 中断标识 */
#define CH9434_UARTx_FCR                    0x02  /* 只写 - FIFO 控制 */
#define CH9434_UARTx_LCR                    0x03  /* 读写 - 线路控制 */
#define CH9434_UARTx_BIT_DLAB               (1 << 7)
#define CH9434_UARTx_MCR                    0x04  /* 读写 - 调制解调器控制 */
#define CH9434_UARTx_LSR                    0x05  /* 读   - 线路状态 */
#define CH9434_UARTx_MSR                    0x06  /* 读   - 调制解调器状态 */
#define CH9434_UARTx_SCR                    0x07  /* 读写 - 暂存（用户自定义） */
#define CH9434_UARTx_DLL                    0x00  /* 读写 - 除数锁存低 (DLAB=1) */
#define CH9434_UARTx_DLM                    0x01  /* 读写 - 除数锁存高 (DLAB=1) */

/* 线路状态寄存器 (LSR) 位 */
#define CH9434_LSR_DR                       (1 << 0)  /* 数据就绪 */
#define CH9434_LSR_OE                       (1 << 1)  /* 溢出错误 */
#define CH9434_LSR_PE                       (1 << 2)  /* 奇偶校验错误 */
#define CH9434_LSR_FE                       (1 << 3)  /* 帧错误 */
#define CH9434_LSR_BI                       (1 << 4)  /* 中止指示 */
#define CH9434_LSR_THRE                     (1 << 5)  /* 发送保持寄存器空 */
#define CH9434_LSR_TEMT                     (1 << 6)  /* 发送器空 */
#define CH9434_LSR_FERR                     (1 << 7)  /* FIFO 数据错误 */

/* 线路控制寄存器 (LCR) 位 */
#define CH9434_LCR_WLS_5                    0x00
#define CH9434_LCR_WLS_6                    0x01
#define CH9434_LCR_WLS_7                    0x02
#define CH9434_LCR_WLS_8                    0x03
#define CH9434_LCR_STOP                     (1 << 2)
#define CH9434_LCR_PARITY_EN                (1 << 3)
#define CH9434_LCR_PARITY_EVEN              (1 << 4)
#define CH9434_LCR_PARITY_STICK             (1 << 5)
#define CH9434_LCR_BREAK                    (1 << 6)

/* FIFO 控制寄存器 (FCR) 位 */
#define CH9434_FCR_ENABLE                   (1 << 0)
#define CH9434_FCR_RX_RESET                 (1 << 1)
#define CH9434_FCR_TX_RESET                 (1 << 2)
#define CH9434_FCR_TRIG_1                   (0 << 6)
#define CH9434_FCR_TRIG_4                   (1 << 6)
#define CH9434_FCR_TRIG_8                   (2 << 6)
#define CH9434_FCR_TRIG_14                  (3 << 6)

/* 调制解调器控制寄存器 (MCR) 位 */
#define CH9434_MCR_DTR                      (1 << 0)
#define CH9434_MCR_RTS                      (1 << 1)
#define CH9434_MCR_OUT1                     (1 << 2)
#define CH9434_MCR_OUT2                     (1 << 3)
#define CH9434_MCR_LOOPBACK                 (1 << 4)
#define CH9434_MCR_AFE                      (1 << 5)  /* 自动流控制使能 */

/* 中断使能寄存器 (IER) 位 */
#define CH9434_IER_RX                       (1 << 0)
#define CH9434_IER_TX                       (1 << 1)
#define CH9434_IER_LS                       (1 << 2)
#define CH9434_IER_MS                       (1 << 3)

/* 中断标识寄存器 (IIR) 位 */
#define CH9434_IIR_PEND                     (1 << 0)  /* 0=挂起, 1=无中断 */
#define CH9434_IIR_ID_MASK                  0x0E
#define CH9434_IIR_ID_MODEM                 0x00
#define CH9434_IIR_ID_TX_EMPTY              0x02
#define CH9434_IIR_ID_RX_AVAIL              0x04
#define CH9434_IIR_ID_LINE_STATUS           0x06
#define CH9434_IIR_ID_TIMEOUT               0x0C

/* ---------- 全局寄存器 ---------- */
#define CH9434_TNOW_CTRL_CFG                0x41
#define CH9434_TNOW_POLAR_MASK              0xF0
#define CH9434_TNOW_EN_MASK                 0x0F
#define CH9434_FIFO_CTRL                    0x42
#define CH9434_FIFO_CTRL_TR                 (1 << 4)
#define CH9434_FIFO_CTRL_UART_MASK          0x0F
#define CH9434_FIFO_CTRL_L                  0x43
#define CH9434_FIFO_CTRL_H                  0x44
#define CH9434_IO_SEL_FUN_CFG               0x45
/* 时钟控制寄存器 (0x48) 位布局，参考 WCH CH9434 文档 */
#define CH9434_CLK_FREQ_MUL_EN              (3 << 6)  /* 使能时钟倍频（1=1x, 3=15x 倍频） */
#define CH9434_CLK_XT_EN                    (1 << 5)  /* 使用外部晶振 */
#define CH9434_CLK_DIV_MASK                 0x1F
#define CH9434_CLK_CTRL_CFG                 0x48
#define CH9434_SLEEP_MOD_CFG                0x4A

/* UART 编号（也用于 FIFO 控制字节） */
#define CH9434_UART0                        0
#define CH9434_UART1                        1
#define CH9434_UART2                        2
#define CH9434_UART3                        3
#define CH9434_UART_COUNT                   4

/* 辅助宏：将 uart 编号转换为其基地址和每寄存器地址 */
#define CH9434_UART_BASE(idx)               ((idx) * 0x10)
#define CH9434_UART_REG(idx, reg)           (CH9434_UART_BASE(idx) + (reg))

/* 每路 UART 地址查找（完整地址，不含操作位） */
#define CH9434_ADDR_RBR(u)                  CH9434_UART_REG((u), CH9434_UARTx_RBR)
#define CH9434_ADDR_THR(u)                  CH9434_UART_REG((u), CH9434_UARTx_THR)
#define CH9434_ADDR_IER(u)                  CH9434_UART_REG((u), CH9434_UARTx_IER)
#define CH9434_ADDR_IIR(u)                  CH9434_UART_REG((u), CH9434_UARTx_IIR)
#define CH9434_ADDR_FCR(u)                  CH9434_UART_REG((u), CH9434_UARTx_FCR)
#define CH9434_ADDR_LCR(u)                  CH9434_UART_REG((u), CH9434_UARTx_LCR)
#define CH9434_ADDR_MCR(u)                  CH9434_UART_REG((u), CH9434_UARTx_MCR)
#define CH9434_ADDR_LSR(u)                  CH9434_UART_REG((u), CH9434_UARTx_LSR)
#define CH9434_ADDR_MSR(u)                  CH9434_UART_REG((u), CH9434_UARTx_MSR)
#define CH9434_ADDR_SCR(u)                  CH9434_UART_REG((u), CH9434_UARTx_SCR)
#define CH9434_ADDR_DLL(u)                  CH9434_UART_REG((u), CH9434_UARTx_DLL)
#define CH9434_ADDR_DLM(u)                  CH9434_UART_REG((u), CH9434_UARTx_DLM)

#ifdef __cplusplus
}
#endif

#endif /* CH9434_REGS_H */
