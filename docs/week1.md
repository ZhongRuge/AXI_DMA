# Week 1 — Platform Driver Bring-up & Register Map v1

## 1. 项目架构

```
PL 持续数据源 → AXI DMA S2MM → DDR → Linux 驱动 → 用户态工具
```

当前阶段：Week 1 — Platform Driver MMIO bring-up（无 DMA、无中断、无字符设备）。

## 2. 当前板卡与环境

| 项目 | 值 |
|---|---|
| 板卡 | Alientek Navigator Zynq Development Board |
| 架构 | Zynq-7000, armv7l |
| 内核 | Linux 5.4.0-xilinx |
| 工具链 | petalinux 2020.2 (cortexa9t2hf-neon-xilinx-linux-gnueabi) |
| 模块名 | `stream_ctrl` |
| 驱动名 | `stream_ctrl` |
| 临时 compatible | `xlnx,PWM-2.0` |
| 未来 compatible | `zrg,zynq-stream-axidma` |
| 当前设备树节点 | `PWM@43c00000` |
| 物理地址范围 | `0x43c00000 – 0x43c0ffff` (64 KB) |

> **注意**：`xlnx,PWM-2.0` 只是第 1 周 bring-up 阶段的临时 compatible。该 PWM IP 核的寄存器布局与正式 `stream_ctrl` IP 不同。当前仅在 probe() 中做只读访问（`readl` / `stream_ctrl_read`），不写入任何寄存器。
>
> 正式 compatible 设计为 `zrg,zynq-stream-axidma`，将在第 2 周 FPGA stream_gen 自定义 IP 部署后替换。

## 3. 正式寄存器表 v1

基地址：`0x43c00000`（临时 PWM 节点，仅用于 bring-up）

| 偏移 | 寄存器名 | 功能 |
|---|---|---|
| 0x00 | CTRL | 控制寄存器（ENABLE = bit0, RESET = bit1） |
| 0x04 | STATUS | 状态寄存器（RUNNING = bit0, ERROR = bit1, BACKPRESSURE = bit2） |
| 0x08 | PACKET_LEN | 包长配置 |
| 0x0c | RATE_DIV | 速率分频 |
| 0x10 | WORD_COUNT | 已传输字数（只读） |
| 0x14 | PACKET_COUNT | 已传输包数（只读） |
| 0x18 | BACKPRESSURE_COUNT | 反压计数（只读） |
| 0x1c | VERSION | IP 版本（只读） |

### CTRL 寄存器位定义

| 位 | 名称 | 描述 |
|---|---|---|
| bit0 | ENABLE | 使能数据流 |
| bit1 | RESET | 复位 IP 核 |
| bit31:2 | — | 保留 |

### STATUS 寄存器位定义

| 位 | 名称 | 描述 |
|---|---|---|
| bit0 | RUNNING | 数据流运行中 |
| bit1 | ERROR | 错误发生 |
| bit2 | BACKPRESSURE | 反压状态 |
| bit31:3 | — | 保留 |

## 4. 代码文件清单

| 文件 | 说明 |
|---|---|
| `stream_ctrl.h` | 设备结构体、寄存器宏、BIT 宏、hw_* 函数声明 |
| `stream_ctrl.c` | platform driver 实现：probe/remove、MMIO 封装、dump_regs、hw_* 骨架 |
| `Makefile` | out-of-tree 模块编译 |

## 5. Bring-up 日志（阶段 0 ✅）

以下功能已在板端验证通过：

- [x] out-of-tree Makefile 可以编译生成 `stream_ctrl.ko`
- [x] `insmod stream_ctrl.ko` 成功匹配 `43c00000.PWM`
- [x] `probe()` 进入，`platform_get_resource()` 正确获取 `[mem 0x43c00000-0x43c0ffff]`
- [x] `devm_ioremap_resource()` 映射成功
- [x] `readl()` 读出 4 个寄存器值并打印到 dmesg
- [x] `rmmod stream_ctrl` 触发 `remove()`

### 阶段 1（当前）

- [x] `stream_ctrl_read()` / `stream_ctrl_write()` MMIO 封装
- [x] `dump_regs()` 改用封装函数，扩展为 8 个寄存器
- [x] `hw_start()` / `hw_stop()` / `hw_reset()` / `hw_get_status()` 骨架
- [x] 寄存器宏更新为正式 v1 表
- [x] BIT 宏更新为 v1 规范（ENABLE/RESET/RUNNING/ERROR/BACKPRESSURE）
- [x] 兼容性注释与未来 compatible 标记
- [x] 板端重新编译验证

## 6. 注意事项

1. `probe()` 中不主动写入任何寄存器——当前复用 PWM 节点，写寄存器可能产生意外行为。
2. `hw_start/stop/reset` 函数已定义但**未被调用**，等待第 2 周 stream_gen IP 部署后启用。
3. 当前寄存器表 v1 是设计阶段预定义，最终以 FPGA stream_gen IP 实际 RTL 实现为准，可能需要在第 2 周调整。
