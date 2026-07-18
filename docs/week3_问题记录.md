# Week 3 问题记录

## 1. stream_gen 寄存器与旧 PWM 占位设备的修正

### 问题背景

Week 1 为了先验证 Linux platform_driver、设备树匹配、MMIO 映射和寄存器读取流程，临时绑定了旧硬件系统中的 PWM 设备：

```dts
PWM@43c00000 {
    compatible = "xlnx,PWM-2.0";
};
```

对应驱动最初使用：

```c
#define STREAM_CTRL_COMPATIBLE "xlnx,PWM-2.0"
```

该 PWM 设备只是 Week 1 的 MMIO 占位靶子，并不是真实的 stream_gen。因此，旧系统中对 0x43c00000 的读取只能证明：

- 设备树能够匹配驱动；
- platform_get_resource() 能获取资源；
- devm_ioremap_resource() 能完成映射；
- Linux 能读取该地址范围。

它不能证明正式 stream_gen RTL 已存在，也不能证明 AXI DMA 数据通路正确。

### 正式寄存器表

Week 2 完成 stream_gen.v 后，正式寄存器定义为：

| 偏移 | 名称 | 说明 |
| --- | --- | --- |
| 0x00 | CTRL | bit0：ENABLE，bit1：RESET |
| 0x04 | STATUS | bit0：RUNNING，bit1：ERROR，bit2：BACKPRESSURE |
| 0x08 | PACKET_LEN | 每个 AXI-Stream packet 的 word 数 |
| 0x0c | RATE_DIV | 成功传输后的等待周期数 |
| 0x10 | WORD_COUNT | 已成功传输的 word 总数 |
| 0x14 | PACKET_COUNT | 已完成 packet 数量 |
| 0x18 | BACKPRESSURE_COUNT | 发生 backpressure 的周期计数 |
| 0x1c | VERSION | RTL 版本号 |

正式 RTL 默认值为：

```text
CTRL               = 0x00000000
STATUS             = 0x00000000
PACKET_LEN         = 0x00000010
RATE_DIV           = 0x00000000
WORD_COUNT         = 0x00000000
PACKET_COUNT       = 0x00000000
BACKPRESSURE_COUNT = 0x00000000
VERSION            = 0x00010000
```

### 修正内容

驱动设备树匹配字符串最终改为：

```c
#define STREAM_CTRL_COMPATIBLE "zrg,zynq-stream-axidma"
```

设备树使用：

```dts
&stream_gen_0 {
    compatible = "zrg,zynq-stream-axidma";
    dmas = <&axi_dma_0 1>;
    dma-names = "rx";
    status = "okay";
};
```

驱动寄存器偏移保持与 RTL 完全一致：

```c
#define STREAM_REG_CTRL               0x00
#define STREAM_REG_STATUS             0x04
#define STREAM_REG_PACKET_LEN         0x08
#define STREAM_REG_RATE_DIV           0x0c
#define STREAM_REG_WORD_COUNT         0x10
#define STREAM_REG_PACKET_COUNT       0x14
#define STREAM_REG_BACKPRESSURE_COUNT 0x18
#define STREAM_REG_VERSION            0x1c
```

### 验证结果

U-Boot 中读取：

```console
md.l 0x43c00000 8
```

返回：

```text
43c00000: 00000000 00000000 00000010 00000000
43c00010: 00000000 00000000 00000000 00010000
```

Linux 驱动加载后也读取到：

```text
stream_ctrl: CTRL               = 0x00000000
stream_ctrl: STATUS             = 0x00000000
stream_ctrl: PACKET_LEN         = 0x00000010
stream_ctrl: RATE_DIV           = 0x00000000
stream_ctrl: WORD_COUNT         = 0x00000000
stream_ctrl: PACKET_COUNT       = 0x00000000
stream_ctrl: BACKPRESSURE_COUNT = 0x00000000
stream_ctrl: VERSION            = 0x00010000
```

因此可以确认：

- 0x43c00000 已经对应正式 stream_gen；
- Linux 驱动中的寄存器定义与 RTL 一致；
- VERSION=0x00010000 可以作为识别正确 RTL 的关键证据；
- 后续不能再把旧 PWM 的寄存器行为当作正式硬件依据。

## 2. Linux 内核版本与外部驱动模块版本不匹配

### 问题现象

更新 Vivado XSA 并重新构建 PetaLinux 系统后，加载旧的 stream_ctrl.ko 出现：

```text
stream_ctrl: disagrees about version of symbol module_layout
insmod: can't insert 'stream_ctrl.ko': invalid module format
```

该错误发生在驱动 probe() 执行之前，因此与以下内容无关：

- 设备树 compatible；
- MMIO 地址；
- AXI DMA controller；
- dma_request_chan()；
- 驱动业务逻辑。

这是 Linux 内核拒绝加载 ABI 不匹配的外部模块。

### 版本对比

板卡正在运行的内核为：

```console
uname -r
5.4.0-xilinx-v2020.2
```

板卡内核完整信息为：

```text
Linux version 5.4.0-xilinx-v2020.2
```

旧模块的 vermagic 为：

```text
vermagic=5.4.0-xilinx SMP preempt mod_unload modversions ARMv7 p2v8
```

两者不一致：

```text
运行内核：5.4.0-xilinx-v2020.2
旧模块：  5.4.0-xilinx
```

旧 Makefile 使用的是独立 Linux 源码目录，而不是当前 PetaLinux 工程实际构建内核时使用的源码、配置和 Module.symvers。即使基础内核版本同为 Linux 5.4，只要以下任一项不同，也可能导致符号 CRC 不一致：

- CONFIG_LOCALVERSION；
- 内核 .config；
- CONFIG_MODVERSIONS；
- Module.symvers；
- 编译器或构建参数；
- 内核源码补丁状态。

### 根文件系统中的残留问题

NFS rootfs 中同时存在：

```text
/lib/modules/5.4.0-xilinx
/lib/modules/5.4.0-xilinx-v2020.2
```

旧的 stream_ctrl.ko 被放在：

```text
/lib/modules/5.4.0-xilinx/
```

但板卡当前运行内核只应使用：

```text
/lib/modules/5.4.0-xilinx-v2020.2/
```

这说明旧模块目录是历史构建残留，不能继续用于当前系统。

### 修正方法

在当前 PetaLinux 工程中创建模块组件：

```console
petalinux-create -t modules \
    --name stream-ctrl \
    --enable
```

将正式源码放入：

```text
project-spec/meta-user/recipes-modules/stream-ctrl/files/
```

包括：

```text
stream_ctrl.c
stream_ctrl.h
Makefile
COPYING
```

模块 Makefile 使用 PetaLinux/Yocto 传入的 KERNEL_SRC：

```makefile
obj-m := stream_ctrl.o

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) clean
```

构建命令：

```console
petalinux-build -c stream-ctrl
```

生成的正确模块位于：

```text
build/tmp/sysroots-components/zynq_generic/stream-ctrl/lib/modules/5.4.0-xilinx-v2020.2/extra/stream_ctrl.ko
```

将其复制到 NFS rootfs：

```text
/lib/modules/5.4.0-xilinx-v2020.2/extra/stream_ctrl.ko
```

然后在板卡上执行：

```console
depmod -a
modprobe stream_ctrl
```

### 最终验证结果

模块成功加载，并输出：

```text
stream_ctrl 43c00000.stream_gen: stream_ctrl: resource = [mem 0x43c00000-0x43c0ffff]
stream_ctrl 43c00000.stream_gen: stream_ctrl: DMA RX channel acquired successfully
stream_ctrl 43c00000.stream_gen: stream_ctrl: PACKET_LEN         = 0x00000010
stream_ctrl 43c00000.stream_gen: stream_ctrl: VERSION            = 0x00010000
```

模块也可以正常卸载：

```text
stream_ctrl 43c00000.stream_gen: stream_ctrl: remove called
stream_ctrl 43c00000.stream_gen: stream_ctrl: resources will be released by devm
```

因此可以确认：

- 新模块与 5.4.0-xilinx-v2020.2 内核 ABI 匹配；
- 驱动成功绑定正式 stream_gen 节点；
- AXI DMA provider 已成功注册；
- dma_request_chan(dev, "rx") 成功获得 S2MM channel；
- Week 3 的 Linux DMAEngine channel 获取目标完成。

### 后续规则

每次重新构建或更换以下任一内容后，都必须重新编译外部 .ko：

- Linux 内核；
- PetaLinux 工程；
- 内核配置；
- Module.symvers；
- Xilinx Linux 版本；
- CONFIG_LOCALVERSION；
- 工具链或内核源码树。

禁止继续使用硬编码到其他内核源码目录的旧 Makefile 构建正式模块，也不要通过 insmod -f 或关闭模块版本检查来掩盖 ABI 不匹配问题。
