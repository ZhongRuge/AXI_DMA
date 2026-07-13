# Week 2 stream_gen 接口规格

## 模块目标

`stream_gen` 是 FPGA 侧的最小 AXI-Stream 数据源。PS 通过 AXI-Lite 配置模块，
模块通过 AXI-Stream 输出递增的 32-bit 数据。后续计划连接 AXI DMA S2MM：

```text
stream_gen -> AXI DMA S2MM -> DDR -> Linux driver -> user tool
```

当前 AXI-Lite 和 AXI-Stream 共用 `s_axi_aclk`，复位信号
`s_axi_aresetn` 低有效。

## AXI-Lite Slave

数据宽度为 32 bit，地址宽度为 5 bit。

| 通道 | 信号 | 方向 | 位宽 |
| --- | --- | --- | --- |
| AW | `s_axi_awaddr` | input | 5 |
| AW | `s_axi_awvalid` | input | 1 |
| AW | `s_axi_awready` | output | 1 |
| W | `s_axi_wdata` | input | 32 |
| W | `s_axi_wstrb` | input | 4 |
| W | `s_axi_wvalid` | input | 1 |
| W | `s_axi_wready` | output | 1 |
| B | `s_axi_bresp` | output | 2 |
| B | `s_axi_bvalid` | output | 1 |
| B | `s_axi_bready` | input | 1 |
| AR | `s_axi_araddr` | input | 5 |
| AR | `s_axi_arvalid` | input | 1 |
| AR | `s_axi_arready` | output | 1 |
| R | `s_axi_rdata` | output | 32 |
| R | `s_axi_rresp` | output | 2 |
| R | `s_axi_rvalid` | output | 1 |
| R | `s_axi_rready` | input | 1 |

## AXI-Stream Master

| 信号 | 方向 | 位宽 | 说明 |
| --- | --- | --- | --- |
| `m_axis_tdata` | output | 32 | 递增 counter 数据 |
| `m_axis_tvalid` | output | 1 | 当前输出数据有效 |
| `m_axis_tready` | input | 1 | 下游能够接收数据 |
| `m_axis_tlast` | output | 1 | 当前 beat 是包尾 |
| `m_axis_tkeep` | output | 4 | 固定为 `4'b1111` |

只有 `TVALID && TREADY` 同时为 1 才完成一个 beat 的传输。背压期间，
`TDATA`、`TVALID` 和 `TLAST` 必须保持不变。

## 寄存器接口

| 偏移 | 名称 | 属性 | 复位值/固定值 |
| --- | --- | --- | --- |
| `0x00` | `CTRL` | RW | `0` |
| `0x04` | `STATUS` | RO | `0` |
| `0x08` | `PACKET_LEN` | RW | `16` |
| `0x0c` | `RATE_DIV` | RW | `0` |
| `0x10` | `WORD_COUNT` | RO | `0` |
| `0x14` | `PACKET_COUNT` | RO | `0` |
| `0x18` | `BACKPRESSURE_COUNT` | RO | `0` |
| `0x1c` | `VERSION` | RO | `32'h0001_0000` |

`CTRL[0]` 为 `ENABLE`，`CTRL[1]` 为软件复位命令。`STATUS[0]` 为
`RUNNING`，`STATUS[1]` 为 `ERROR`，`STATUS[2]` 为当前
`BACKPRESSURE`。

## 实现边界

当前已实现 AXI-Lite 单事务寄存器访问和最小 AXI-Stream counter 输出。
尚未连接 AXI DMA，尚未实现 Linux DMAEngine client、字符设备、Vivado block
design 和仿真测试。
