# Week 2 AXI-Lite 寄存器读写说明

## 总线角色

```text
PS / AXI interconnect = AXI-Lite master
stream_gen            = AXI-Lite slave
```

AXI-Lite 写表示 PS 修改 `stream_gen` 寄存器，AXI-Lite 读表示 PS 读取
`stream_gen` 寄存器。

接口数据宽度为 32 bit，地址宽度为 5 bit。当前实现每次只处理一笔事务，
不支持多个 outstanding transaction。`BRESP` 和 `RRESP` 均返回
`2'b00`（OKAY）。

## 写事务

写事务由 AW、W、B 三个独立通道组成：

| 通道 | 方向 | 作用 |
| --- | --- | --- |
| AW | PS -> stream_gen | 传输写地址 |
| W | PS -> stream_gen | 传输写数据和字节使能 |
| B | stream_gen -> PS | 返回写响应 |

AW 和 W 不要求同拍到达。`stream_gen` 分别锁存写地址和写数据，二者都接收
完成后才修改寄存器并产生 B 响应。`BVALID` 会保持到 PS 拉高 `BREADY` 完成
握手。

当前只接受整字写，即 `WSTRB=4'b1111`。其他 `WSTRB` 值不会修改任何寄存器，
但仍返回 OKAY。

## 读事务

读事务由 AR、R 两个独立通道组成：

| 通道 | 方向 | 作用 |
| --- | --- | --- |
| AR | PS -> stream_gen | 传输读地址 |
| R | stream_gen -> PS | 返回读数据和读响应 |

AR 握手成功时，硬件根据地址生成读数据并拉高 `RVALID`。在 PS 拉高
`RREADY` 前，`RDATA`、`RRESP` 和 `RVALID` 保持不变。未定义地址返回 0。

## 寄存器表

| 偏移 | 名称 | 属性 | 复位值/固定值 | 说明 |
| --- | --- | --- | --- | --- |
| `0x00` | `CTRL` | RW | `0` | bit0 使能，bit1 软件复位命令 |
| `0x04` | `STATUS` | RO | `0` | bit0 运行，bit1 错误，bit2 当前背压 |
| `0x08` | `PACKET_LEN` | RW | `16` | 每包包含的 32-bit word 数量 |
| `0x0c` | `RATE_DIV` | RW | `0` | 每次成功传输后的等待周期数 |
| `0x10` | `WORD_COUNT` | RO | `0` | 成功传输的 word 总数 |
| `0x14` | `PACKET_COUNT` | RO | `0` | 成功传输的 packet 总数 |
| `0x18` | `BACKPRESSURE_COUNT` | RO | `0` | 累计背压时钟周期数 |
| `0x1c` | `VERSION` | RO | `32'h0001_0000` | 固定版本号 |

软件只能写 `CTRL`、`PACKET_LEN` 和 `RATE_DIV`。写只读寄存器或未定义地址
不会修改内部状态，但仍返回 OKAY。

## CTRL 行为

| 位 | 名称 | 行为 |
| --- | --- | --- |
| 0 | `ENABLE` | 读写；为 1 时允许产生 AXI-Stream 数据 |
| 1 | `RESET` | 只写命令；写 1 触发一次软件复位，不保存在 `ctrl_reg` 中 |
| 31:2 | 保留 | 写入值被忽略，读取为 0 |

软件复位优先于使能写入。软件复位会：

- 清除 `CTRL`、`STATUS` 和三个运行统计计数器。
- 清除递增计数器、包内下标、节流倒计时和 AXI-Stream 输出寄存器。
- 保留 `PACKET_LEN` 和 `RATE_DIV` 配置。
- 不取消当前 AXI-Lite 写响应，B 通道仍返回 OKAY。

## PACKET_LEN

`PACKET_LEN` 不允许保存为 0。软件写入 0 时，硬件实际保存 1，因此
`PACKET_LEN=1` 表示每个 beat 都是一个完整数据包。

为了避免运行中修改长度改变当前包边界，建议按以下顺序重新配置：停止输出、
写入 `PACKET_LEN`/`RATE_DIV`、执行软件复位、重新使能。

## 当前边界

当前已经实现最小 AXI-Lite 寄存器读写和 AXI-Stream counter 输出，但尚未连接
AXI DMA，也未实现 Linux DMAEngine client、字符设备或 Vivado block design。
