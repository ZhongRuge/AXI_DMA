# Week 2 stream_gen 接口规格

## 目标

本阶段冻结 `stream_gen` 的最小 RTL 接口骨架。

`stream_gen` 是 FPGA 侧最小数据源，后续用于向 AXI DMA S2MM 输出 AXI-Stream 数据。当前阶段只定义接口、寄存器地址和内部寄存器名称，不实现 AXI-Lite 读写逻辑，不实现 AXI-Stream 数据发送状态机。

最终数据方向：

```text
stream_gen -> AXI DMA S2MM -> DDR -> Linux driver -> user tool
```

## 当前边界

本阶段包含：

- AXI-Lite slave 控制接口
- AXI-Stream master 输出接口
- 32-bit 寄存器地址表
- 最小内部寄存器声明

本阶段不包含：

- AXI-Lite 读写时序逻辑
- AXI-Stream 递增计数器输出逻辑
- `tvalid/tready/tlast` 状态机
- Vivado block design 连接
- AXI DMA 配置
- Linux 驱动或字符设备代码
- 仿真测试

## 时钟和复位

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `s_axi_aclk` | input | 1 | AXI-Lite 和 AXI-Stream 当前共用时钟 |
| `s_axi_aresetn` | input | 1 | 低有效复位 |

## AXI-Lite slave 接口

AXI-Lite 数据宽度为 32 bit，地址宽度当前使用 5 bit。

### Write address channel

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `s_axi_awaddr` | input | 5 | 写地址 |
| `s_axi_awvalid` | input | 1 | 写地址有效 |
| `s_axi_awready` | output | 1 | 从设备可接收写地址 |

### Write data channel

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `s_axi_wdata` | input | 32 | 写数据 |
| `s_axi_wstrb` | input | 4 | 字节写使能 |
| `s_axi_wvalid` | input | 1 | 写数据有效 |
| `s_axi_wready` | output | 1 | 从设备可接收写数据 |

### Write response channel

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `s_axi_bresp` | output | 2 | 写响应 |
| `s_axi_bvalid` | output | 1 | 写响应有效 |
| `s_axi_bready` | input | 1 | 主设备可接收写响应 |

### Read address channel

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `s_axi_araddr` | input | 5 | 读地址 |
| `s_axi_arvalid` | input | 1 | 读地址有效 |
| `s_axi_arready` | output | 1 | 从设备可接收读地址 |

### Read data channel

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `s_axi_rdata` | output | 32 | 读数据 |
| `s_axi_rresp` | output | 2 | 读响应 |
| `s_axi_rvalid` | output | 1 | 读数据有效 |
| `s_axi_rready` | input | 1 | 主设备可接收读数据 |

## AXI-Stream master 接口

AXI-Stream 数据宽度为 32 bit。

| Signal | Direction | Width | Description |
| --- | --- | --- | --- |
| `m_axis_tdata` | output | 32 | 输出数据，后续用于递增 counter |
| `m_axis_tvalid` | output | 1 | 输出数据有效 |
| `m_axis_tready` | input | 1 | 下游可接收数据 |
| `m_axis_tlast` | output | 1 | 当前 packet 最后一个 word |
| `m_axis_tkeep` | output | 4 | 字节有效标记，目标固定为 `4'b1111` |

## 寄存器表

| Offset | Name | Access | Reset / Fixed Value | Description |
| --- | --- | --- | --- | --- |
| `0x00` | `CTRL` | RW | TBD | 控制寄存器 |
| `0x04` | `STATUS` | RO | TBD | 状态寄存器 |
| `0x08` | `PACKET_LEN` | RW | `16` | 每包包含的 32-bit word 数 |
| `0x0c` | `RATE_DIV` | RW | `0` | 输出速率分频配置 |
| `0x10` | `WORD_COUNT` | RO | `0` | 已输出 word 计数 |
| `0x14` | `PACKET_COUNT` | RO | `0` | 已输出 packet 计数 |
| `0x18` | `BACKPRESSURE_COUNT` | RO | `0` | 下游未 ready 导致阻塞的计数 |
| `0x1c` | `VERSION` | RO | `32'h0001_0000` | 固定版本号 |

## CTRL bits

| Bit | Name | Description |
| --- | --- | --- |
| 0 | `ENABLE` | 使能数据源 |
| 1 | `RESET` | 软件复位请求 |

## STATUS bits

| Bit | Name | Description |
| --- | --- | --- |
| 0 | `RUNNING` | 数据源正在运行 |
| 1 | `ERROR` | 错误状态 |
| 2 | `BACKPRESSURE` | 当前或最近出现下游背压 |

## RTL 当前文件

当前 RTL 文件：

```text
AXI_DMA/Vivado_project/Vivado_project.srcs/sources_1/new/stream_gen.v
```

当前 RTL 只包含：

- `stream_gen` module 端口列表
- `REG_*` 寄存器偏移 `localparam`
- `VERSION_VALUE`
- 7 个 32-bit 内部寄存器声明

后续实现逻辑应在单独阶段逐步加入。
