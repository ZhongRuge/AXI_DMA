# Week 4 问题记录

## 1. 单次 S2MM DMA 接收完成后 `WORD_COUNT` 多出 1～3 个 word

### 问题背景

Week 4 实现了最小、可重复的 AXI DMA S2MM 接收闭环：

```
stream_gen
    -> AXI DMA S2MM
    -> coherent DMA buffer
    -> DMA completion callback
    -> 内核数据校验
```

每次 DMA 接收参数：

```
DMA buffer size：64 bytes
word 数量：16
PACKET_LEN：16
数据序列：0, 1, 2, ... , 15
DMA direction：DMA_DEV_TO_MEM
```

驱动每次模块加载时连续执行两次 RX 传输，并对 DMA buffer 中的 16 个 word 进行完整校验。

### 初始现象

DMA buffer 数据校验全部通过，但传输完成后读取 `stream_gen` 计数器时出现：

```
WORD_COUNT = 18 或 19
PACKET_COUNT = 1
```

DMA buffer 中的数据仍然严格为：

```
0, 1, 2, ... , 15
```

没有出现：

- DMA timeout；
- DMA status 异常；
- buffer 数据错误；
- 模块卸载失败；
- callback 残留；
- use-after-free。

### 原始执行顺序

初始实现的顺序为：

```
提交 DMA descriptor
-> dma_async_issue_pending()
-> 启动 stream_gen
-> 等待 DMA completion
-> callback 调用 complete()
-> 等待线程重新获得 CPU
-> stream_ctrl_hw_stop()
```

DMA completion callback 最初只有：

```
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_ctrl_dev *sdev = args;

    complete(&sdev->rx_completion);
}
```

### 根因分析

DMA completion 表示当前 64-byte descriptor 已完成写入 DDR，但它不代表 AXI-Stream 数据生产者已经立即停止。

callback 调用 `complete()` 后，等待线程还需要经过：

```
中断或 tasklet 处理
-> 唤醒等待线程
-> 调度等待线程运行
-> 执行 stream_ctrl_hw_stop()
```

在这个时间窗口内，AXI DMA 的 S2MM 接口可能仍短暂保持 `TREADY`，因此 `stream_gen` 可能继续完成下一包开头的若干次：

```
TVALID && TREADY
```

`WORD_COUNT` 统计的是 `stream_gen` 端全部成功握手的 word 数，而不是当前 DMA descriptor 写入 DDR 的长度。

因此可能出现：

```
DMA descriptor 接收：16 words
stream_gen WORD_COUNT：18 或 19
PACKET_COUNT：1
```

`PACKET_COUNT=1` 说明第一个完整 packet 已经发送完成，额外的 2～3 个 word 只是下一包开头，并没有形成第二个完整 packet。

### 第一次修正

将停止 `stream_gen` 的操作提前到 DMA completion callback 中：

```
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_ctrl_dev *sdev = args;

    stream_ctrl_hw_stop(sdev);
    complete(&sdev->rx_completion);
}
```

`writel()` 和 `complete()` 都不会睡眠，可以在当前 DMA callback 上下文中执行。

等待线程中的重复停止操作暂时保留：

```
stream_ctrl_hw_stop(sdev);
```

重复写入 `CTRL=0` 没有副作用，可以作为防御性停止。

### 修正结果

修改 callback 后，连续测试得到：

```
WORD_COUNT = 16 或 17
PACKET_COUNT = 1
```

原来的 18～19 已经收敛到 16～17，证明将停止操作提前到 callback 显著缩短了额外握手窗口。

部分日志：

```
RX counters after stop: words=17, packets=1
RX data validation passed: words=16
```

也出现过：

```
RX counters after stop: words=16, packets=1
RX data validation passed: words=16
```

### 为什么仍可能出现 17

`stream_gen` RTL 的禁用语义不是立即丢弃当前输出 beat。

当软件清除 `CTRL.ENABLE` 时，如果已经存在一个 pending beat，RTL允许该 beat完成：

```
TVALID && TREADY
```

然后再停止。

因此，即使 callback 已经尽早写入 `CTRL=0`，仍可能有一个已经进入输出状态的 beat完成握手，最终出现：

```
WORD_COUNT = 17
```

这是 RTL 当前停止语义允许的结果，不代表 DMA buffer 越界，也不代表 descriptor 多写入了数据。

### 正确的验收依据

当前 DMA 接收长度应依据：

```
descriptor 长度 = 64 bytes
DMA status = DMA_COMPLETE
completion callback 被调用
buffer 中 16 个 word 全部为 0～15
```

`WORD_COUNT` 只作为数据生产者侧的诊断统计，不能单独作为 DMA descriptor 实际写入长度的硬性判据。

Week 4 当前验收重点：

- coherent DMA buffer 分配成功；
- descriptor 准备成功；
- submit cookie 有效；
- callback 被调用；
- completion 未超时；
- DMA status 为 `DMA_COMPLETE`；
- buffer 数据严格校验为 `0～15`；
- 两次连续传输均成功；
- 连续三轮模块加载、卸载均成功；
- `PACKET_COUNT=1`；
- `WORD_COUNT=16` 或 `17` 均符合当前 RTL 停止语义。

### 不采用的修正方式

不通过单纯增大 `RATE_DIV` 来强求：

```
WORD_COUNT 永远等于 16
```

增大 `RATE_DIV` 只能降低额外 beat 出现的概率，不能从架构上消除竞态。

如果未来要求生产者严格只发送一个 packet，需要在 RTL 中增加类似：

```
one-shot mode
packet budget
发送指定 packet 数后自动停止
```

这属于后续 RTL 功能扩展，不属于当前 Week 4 的最小 DMAEngine 接收目标。

---

## 2. Week 4 最终板端验证结果

模块加载后成功输出：

```
stream_ctrl: DMA RX buffer allocated, size=64
RX data validation passed: words=16
stream_ctrl: starting second rx transfer.
RX data validation passed: words=16
```

模块连续加载、卸载三次均正常：

```
stream_ctrl: remove called
stream_ctrl: resources will be released by devm
```

没有出现：

```
completion timeout
DMA status error
RX data validation failed
invalid module format
kernel crash
use-after-free
```

因此已经证明：

- AXI DMA S2MM 能够将 PL 数据写入 DDR；
- coherent DMA buffer 可以被 CPU 正确读取；
- DMAEngine descriptor、submit、issue pending、callback 和 completion 链路工作正常；
- 接收数据与 `stream_gen` 的 RTL 数据序列一致；
- 同一模块实例中可以连续完成两次传输；
- 模块可以重复加载和卸载；
- DMA channel、callback 和 coherent buffer 的清理路径有效。
