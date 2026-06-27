# Week 2 AXI-Lite 寄存器读写说明

## 目标

本阶段给 `stream_gen` 增加最小 AXI-Lite 寄存器读写逻辑，让 PS/Linux 后续可以通过 MMIO 访问控制寄存器。

当前只实现寄存器读写，不实现 AXI-Stream 数据输出状态机，不连接 AXI DMA，不编写 Linux 驱动。

## 当前实现边界

当前 AXI-Lite slave 是最小寄存器 bring-up 版本：

- 写通道要求 `AWVALID` 和 `WVALID` 同拍有效。
- 不单独缓存提前到达的写地址或写数据。
- 读通道收到 `ARVALID` 后立即返回对应寄存器值。
- 不支持多个 outstanding transaction。
- `BRESP` 和 `RRESP` 均固定返回 `2'b00`。

这个版本适合当前阶段验证寄存器访问，不是完整通用 AXI-Lite IP 模板。

## AXI-Lite 通道角色

在本项目中：

```text
PS/Linux/AXI interconnect = AXI-Lite master
stream_gen                = AXI-Lite slave
```

因此：

```text
AXI-Lite write = PS 写 stream_gen 寄存器
AXI-Lite read  = PS 读 stream_gen 寄存器
```

## Write 行为

写事务使用三个通道：

| Channel | Direction | Function |
| --- | --- | --- |
| `AW` | PS -> stream_gen | 写地址 |
| `W` | PS -> stream_gen | 写数据 |
| `B` | stream_gen -> PS | 写响应 |

当前实现只在以下条件成立时接收一次写：

```text
!s_axi_bvalid_reg && s_axi_awvalid && s_axi_wvalid
```

接收写事务时：

- `s_axi_awready_reg` 拉高一拍。
- `s_axi_wready_reg` 拉高一拍。
- 根据 `s_axi_awaddr` 写内部寄存器。
- `s_axi_bvalid_reg` 拉高，表示写响应有效。
- `s_axi_bresp_reg` 返回 `2'b00`。

当前只接受整字 32-bit 写：

```text
s_axi_wstrb == 4'b1111
```

如果 `s_axi_wstrb != 4'b1111`，本次写事务仍返回正常写响应 `2'b00`，但不会修改任何内部寄存器。

当以下条件成立时，写响应被 PS 接收：

```text
s_axi_bvalid_reg && s_axi_bready
```

随后 `s_axi_bvalid_reg` 清 0。

## Read 行为

读事务使用两个通道：

| Channel | Direction | Function |
| --- | --- | --- |
| `AR` | PS -> stream_gen | 读地址 |
| `R` | stream_gen -> PS | 读数据和读响应 |

当前实现只在以下条件成立时接收一次读：

```text
!s_axi_rvalid_reg && s_axi_arvalid
```

接收读事务时：

- `s_axi_arready_reg` 拉高一拍。
- 根据 `s_axi_araddr` 选择返回数据。
- `s_axi_rvalid_reg` 拉高，表示读数据有效。
- `s_axi_rresp_reg` 返回 `2'b00`。

当以下条件成立时，读数据被 PS 接收：

```text
s_axi_rvalid_reg && s_axi_rready
```

随后 `s_axi_rvalid_reg` 清 0。

## 寄存器访问属性

| Offset | Name | Access | Reset / Fixed Value | Description |
| --- | --- | --- | --- | --- |
| `0x00` | `CTRL` | RW | `0` | 控制寄存器，bit0 为 enable，bit1 为 reset |
| `0x04` | `STATUS` | RO | `0` | 状态寄存器，bit0 running，bit1 error，bit2 backpressure |
| `0x08` | `PACKET_LEN` | RW | `16` | 每包包含的 32-bit word 数量 |
| `0x0c` | `RATE_DIV` | RW | `0` | 输出速率分频配置 |
| `0x10` | `WORD_COUNT` | RO | `0` | 已输出 word 总数 |
| `0x14` | `PACKET_COUNT` | RO | `0` | 已输出 packet 总数 |
| `0x18` | `BACKPRESSURE_COUNT` | RO | `0` | 因下游未 ready 导致阻塞的计数 |
| `0x1c` | `VERSION` | RO | `32'h0001_0000` | 固定版本号 |

## CTRL 行为

`CTRL` 只保留 bit0 和 bit1，高位写入会被屏蔽。

| Bit | Name | Access | Description |
| --- | --- | --- | --- |
| 0 | `ENABLE` | RW | 数据源使能位，当前阶段只保存该配置，不驱动 AXI-Stream 状态机 |
| 1 | `RESET` | WO / self-clear | 软件复位触发位，写 1 后立即触发内部复位并自清 |

写 `CTRL[1] = 1` 时，硬件执行软件复位：

- `ctrl_reg` 清 0。
- `status_reg` 清 0。
- `packet_len_reg` 恢复为 `32'd16`。
- `rate_div_reg` 清 0。
- `word_count_reg` 清 0。
- `packet_count_reg` 清 0。
- `backpressure_count_reg` 清 0。
- AXI-Stream 输出保持安全静态值：`tvalid=0`、`tdata=0`、`tlast=0`、`tkeep=4'b1111`。

软件复位不会取消当前 AXI-Lite 写响应，写响应仍正常返回 `2'b00`。

## 可写寄存器

当前软件只允许写：

- `CTRL`
- `PACKET_LEN`
- `RATE_DIV`

写其他地址不会修改内部寄存器，但仍返回正常写响应 `2'b00`。

如果写事务的 `WSTRB` 不是 `4'b1111`，即使地址属于可写寄存器，也不会修改寄存器。

## 只读寄存器

以下寄存器当前只读：

- `STATUS`
- `WORD_COUNT`
- `PACKET_COUNT`
- `BACKPRESSURE_COUNT`
- `VERSION`

这些寄存器不会被 AXI-Lite write 逻辑修改。

## PACKET_LEN 特殊处理

`PACKET_LEN` 不允许保存为 0。

如果软件写：

```text
PACKET_LEN = 0
```

硬件实际保存：

```text
PACKET_LEN = 1
```

这样可以避免后续 AXI-Stream packet 长度为 0 的无效配置。

## 当前未实现内容

当前阶段尚未实现：

- AXI-Stream counter 数据输出。
- `tvalid/tready/tlast` 状态机。
- `WORD_COUNT`、`PACKET_COUNT`、`BACKPRESSURE_COUNT` 的运行时累加。
- AXI DMA 连接。
- Linux DMAEngine 或字符设备逻辑。
- Vivado block design 连接和仿真。
