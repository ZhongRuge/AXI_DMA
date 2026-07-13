# Week 2 AXI-Stream 数据输出说明

## Enable 行为

`CTRL[0]=1` 时，`stream_gen` 可以提出 AXI-Stream 数据；`CTRL[0]=0` 时不再
提出新数据。如果清除使能时已经存在一个尚未握手的有效 beat，该 beat 会保持
`TDATA`、`TVALID` 和 `TLAST`，完成握手后再停止。

## 软件复位

向 `CTRL[1]` 写 1 会触发一次软件复位，并自动清除使能。软件复位会清除：

- 递增数据计数器和包内下标。
- `WORD_COUNT`、`PACKET_COUNT` 和 `BACKPRESSURE_COUNT`。
- AXI-Stream 的 `TDATA`、`TVALID`、`TLAST` 和节流倒计时。

软件复位保留 `PACKET_LEN` 和 `RATE_DIV`。`TKEEP` 始终固定为
`4'b1111`。

## Counter 数据

第一次有效传输的数据为 0，之后每成功握手一次，下一份数据增加 1。只有
`TVALID && TREADY` 时计数器和 `WORD_COUNT` 才会增加；未握手时不得跳数。
32-bit counter 和统计计数器溢出后自然回绕。

## Packet 和 TLAST

`PACKET_LEN` 表示每个包包含的 beat 数量。包内下标从 0 开始，因此当当前下标
等于 `PACKET_LEN-1` 时，当前 beat 的 `TLAST=1`。

- `PACKET_LEN=1` 时，每个 beat 的 `TLAST` 都为 1。
- 只有带 `TLAST` 的 beat 成功握手时，`PACKET_COUNT` 才增加。
- 包尾握手后，包内下标清零，下一份数据属于新包。
- 软件写入 `PACKET_LEN=0` 时，硬件强制保存为 1。

## RATE_DIV

`RATE_DIV` 定义每个成功传输之后主动等待的时钟周期数：

- `RATE_DIV=0`：不主动节流，下游持续 ready 时可每拍传输一个 beat。
- `RATE_DIV=N`：每次成功握手后将 `TVALID` 拉低 N 个时钟周期，再提出下一份数据。

节流等待由发送方主动产生，此时 `TVALID=0`，不属于背压，也不会增加
`BACKPRESSURE_COUNT`。

## 背压和状态

`TVALID=1 && TREADY=0` 表示下游暂时不能接收，即发生背压。背压期间输出数据和
包尾标志保持不变，每持续一个时钟周期，`BACKPRESSURE_COUNT` 增加 1。

| STATUS 位 | 含义 |
| --- | --- |
| bit0 `RUNNING` | 等于 `CTRL[0]` |
| bit1 `ERROR` | 当前固定为 0 |
| bit2 `BACKPRESSURE` | 当前 `TVALID && !TREADY` |

`STATUS.BACKPRESSURE` 是当前状态，`BACKPRESSURE_COUNT` 是累计周期数，二者
不是同一概念。

## 当前边界

当前仅完成 `stream_gen` RTL。尚未连接 AXI DMA，也尚未编写 Linux DMAEngine
client、字符设备、ring buffer、`poll`、`mmap`、中断或 Vivado block design。
