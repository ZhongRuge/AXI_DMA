# Week 7：上板问题与构建产物不对应的排查

本文记录本轮实际遇到的问题、定位依据和最终处理。日志来自本轮板上操作；文中的命令是排查记录，不表示已经重新执行。

## 1. 最后解决了什么

最终系统可以启动，`stream_ctrl.ko` 加载成功，阻塞读取、poll 读取和 60 秒连续收发均通过。（置信度：高）

本轮明确发现了三处需要对应的关系：

| 对应关系 | 实际遇到的问题 | 处理 |
| --- | --- | --- |
| Vivado 硬件与 SD 卡 bitstream | 卡上曾加载旧最小工程的 bitstream，却读取新工程地址 | 换成官方工程生成的 `system.bit` |
| bitstream 与设备树 | 新硬件已加载，SD 卡 DTB 仍描述旧地址 | 更新源文件，完整重编独立内核并重新部署 |
| 运行内核与驱动模块 | 模块 Makefile 仍通过 PetaLinux 构建，板上使用独立编译内核 | 改用该独立内核的构建结果编译模块 |

前两项有直接的地址和文件内容证据。第三项是已纠正的构建来源不一致，不能表述为已经观察到模块 ABI 不兼容报错。

## 2. 先认清每个文件的作用

```text
Vivado 官方工程 Navigator_7020
  ├─ system_wrapper.bit → SD 卡 system.bit → FPGA 中实际运行的电路
  └─ 导出包含 bitstream 的 XSA
       └─ PetaLinux 生成设备树源文件
            └─ 复制到独立内核 arch/arm/boot/dts/
                 ├─ system-top.dtb → SD 卡 system.dtb
                 └─ 内核构建 → zImage
                      └─ 同一份内核构建结果 → stream_ctrl.ko
```

设备树相当于 Linux 的设备地址簿。更新 FPGA 电路不会自动修改 Linux 的地址簿；修改 `.dtsi` 也不会自动替换 SD 卡上的 `.dtb`。

`BOOT.BIN` 负责前期启动，具体包含哪些分区由打包命令决定。本轮 `boot.scr` 的 zImage 启动路径还会加载 SD 卡上的 `system.bit`。因此，即使 BOOT.BIN 内含新 bitstream，也可能随后被卡上的旧 `system.bit` 覆盖。

## 3. 起点：短测试通过，长测试出现数据错误

最初阻塞读取 100 包和 poll 读取 100 包均通过，但长时间运行曾出现：

```text
xilinx-vdma 40400000.dma: Channel ... has errors 10, cdr 0 tdr 0
success=315462 data_error=10 read_error=0
first data error: packet=27110 word=0 expected=0 actual=20
```

这里已经有数据校验失败，不能因 `read_error=0` 就判断收发正确。控制器日志中的 `errors 10` 按十六进制打印，对应 `0x10` 的 DMA internal error 位；仅凭这一位不能确定最终根因。

另一次单包测试后，旧硬件的 `WORD_COUNT` 返回 `0x11`，即 17，而接收缓冲区是 16 个 word。这是需要追查硬件行为的线索，不能单凭它认定某一处 RTL 就是全部故障的原因。

之后经历了硬件工程迁移、bitstream、设备树和内核构建更新，因此最终通过不能用于证明最初所有异常都只有同一个原因。

## 4. SD 卡文件损坏是一个独立问题

曾出现：

```text
Invalid FAT entry
** Unable to read file system.bit **
```

虚拟机中启动分区被挂载为只读，读取 `system.bit` 返回输入/输出错误。`fsck.fat -n` 报告该文件簇链异常，并在最后打印：

```text
Leaving filesystem unchanged.
```

这说明 `-n` 那次只检查，没有修复。不能把输出中的“Truncating”“Auto-correcting”当成修复已经写入磁盘。

处理顺序是先保留可读文件，确认当前 SD 分区设备名，卸载后修复文件系统，再重新复制正确文件。设备名可能随重新插卡变化，不能永久照抄当时的 `/dev/sdc1`。复制后执行 `sync` 并安全弹出。

文件系统恢复后，仍需继续处理硬件和设备树不匹配；这两类问题不能混在一起。

## 5. 从旧最小工程迁移到官方板级工程

最终使用：

```text
Navigator_7020/Navigator_7020.xpr
```

官方工程已有显示、音频等外设，原最小工程的地址不能直接照搬。迁移时复用了原有板级连接，并为新增控制接口使用 PS 的 GP1。

曾经给 GP1 分配 `0x40400000`，Vivado 明确报错：

```text
Valid apertures are {<0x8000_0000 [ 1G ]>}
```

这是地址窗口不允许，不是 Tcl 语法错误。最终采用：

| 设备 | 旧最小工程 | 当前官方工程 |
| --- | --- | --- |
| AXI DMA | `0x40400000` | `0x80400000` |
| stream_gen | `0x43C00000` | `0x83CB0000` |
| stream_gen 版本寄存器 | `0x43C0001C` | `0x83CB001C` |

官方工程中 `0x43C00000` 已由 LCD PWM 使用。当前 DMA 设备树中断为 `<0 54 4>`，旧工程是 `<0 29 4>`。所以不能仅手动替换设备树里的两个基地址，应该更新新 XSA 生成的整套描述。

## 6. 第一处不对应：SD 卡加载的 bitstream 仍是旧工程

在 U-Boot 中手动加载：

```text
fatload mmc 0:1 0x00800000 system.bit
fpga loadb 0 ${fileaddr} ${filesize}
```

旧文件的头信息显示：

```text
date = "2026/09/17"
time = "21:27:17"
```

此时读取新工程版本地址：

```text
md.l 0x83cb001c 1
```

停在地址打印处，没有返回数据。这个现场还没有进入 Linux，因此不能直接归因于 Linux 设备树或 `stream_ctrl.ko`。

随后换成官方工程生成的：

```text
Navigator_7020/Navigator_7020.runs/impl_1/system_wrapper.bit
```

复制到 SD 卡并命名为 `system.bit` 后，U-Boot 显示：

```text
date = "2026/09/21"
time = "16:02:04"
Zynq> md.l 0x83cb001c 1
83cb001c: 00010001
```

这证明当前硬件中该地址可以访问，RTL 版本寄存器为 `0x00010001`。（置信度：高）

注意：时间戳是本轮区分旧、新产物的线索，不是通用的完整内容校验。`system_wrapper` 名称和文件大小相同，也不能证明来自同一个工程。

## 7. 第二处不对应：FPGA 已更新，Linux 仍使用旧设备树

### 7.1 不要把最后一条正常日志当成故障源

启动最初看起来停在：

```text
dma-pl330 f8003000.dmac: Loaded driver for PL330 DMAC-241330
```

开启初始化日志后，后面实际进入的是 `xilinx_vdma_driver_init`。进一步的异常栈给出：

```text
Unhandled fault: imprecise external abort (0x406)
PC is at dma_ctrl_read
LR is at xilinx_dma_probe
```

说明故障发生在 Xilinx DMA 驱动初始化时读取寄存器的阶段。PL330 只是前一个打印完成的驱动。此时自写的 `stream_ctrl.ko` 尚未加载。

“imprecise external abort”也不能直接解释成 `0x00000000` 空指针错误；日志不足以直接给出被访问设备的物理地址。

### 7.2 用 U-Boot 单独确认 DMA 寄存器可以访问

加载正确 bitstream 后执行：

```text
md.l 0x80400034 1
```

实际返回：

```text
80400034: 00000001
```

这是 S2MM 状态寄存器，最低位 `Halted=1` 表示通道停止。尚未启动 DMA 时这个结果正常。它只证明 U-Boot 下该寄存器可访问，不等于 DMA 传输已经通过。

此时硬件地址能读通，下一步就应追查 Linux 使用的描述，不能继续盲目修改 FPGA。

### 7.3 反编译 SD 卡上的文件，而不是只看编辑器中的源码

在虚拟机执行：

```bash
dtc -I dtb -O dts /media/zy/boot/system.dtb -o /tmp/sd-board.dts
grep -n -E -B 5 -A 22 'xlnx,axi-dma|xlnx,axi-vdma' /tmp/sd-board.dts
```

实际仍然看到：

```text
dma@40400000
reg = <0x40400000 0x10000>;
stream_gen@43c00000
reg = <0x43c00000 0x10000>;
```

这是直接证据：SD 卡 DTB 描述的仍是旧硬件。`status = "okay"` 只表示启用该节点，不会自动更新地址。

### 7.4 在板上再次确认读到的文件

为排除虚拟机挂载位置或复制目标混淆，在 U-Boot 中直接查看同一 SD 分区的 DTB：

```text
fatload mmc 0:1 0x00100000 system.dtb
fdt addr 0x00100000
fdt print /amba_pl/dma@80400000
fdt print /amba_pl/dma@40400000
fdt list /amba_pl
```

新地址节点返回 `FDT_ERR_NOTFOUND`；旧地址节点存在。`amba_pl` 下只有：

```text
dma@40400000
stream_gen@43c00000
```

因此确认是旧最小工程的设备树，而不是新官方工程的完整设备树。（置信度：高）

`fdt print` 读取的是内存中的设备树数据，不会像 `md.l` 那样访问 DMA 硬件。

### 7.5 沿产物链向前查，找出更新停在哪里

检查两个源文件位置：

```bash
grep -n -E 'dma@|stream_gen@' \
~/petalinux/ALIENTEK-ZYNQ-driver/components/plnx_workspace/device-tree/device-tree/pl.dtsi \
~/workspace/kernel-driver/linux-xlnx-xlnx_rebase_v5.4_2020.2/arch/arm/boot/dts/pl.dtsi
```

两个文件都已经包含新地址：

```text
dma@80400000
stream_gen@83cb0000
```

再检查独立内核的编译产物：

```bash
dtc -I dtb -O dts arch/arm/boot/dts/system-top.dtb -o /tmp/new-board.dts
grep -n -E 'dma@|stream_gen@' /tmp/new-board.dts
```

它却仍然显示 `40400000`、`43c00000`。到这里，定位结果是：

```text
PetaLinux 生成的 pl.dtsi：新
独立内核中的 pl.dtsi：新
独立内核中的 system-top.dtb：旧
SD 卡上的 system.dtb：旧
```

问题已经收敛到编译产物更新环节，而不只是 SD 卡复制错误。

## 8. 按 PDF 完整重编后的解决过程

按《领航者 ZYNQ 之嵌入式 Linux 开发指南 V3.3》第 20.3 节（第 580～584 页），独立内核使用六个设备树源文件：

```text
pcw.dtsi
pl.dtsi
system-top.dts
zynq-7000.dtsi
system-conf.dtsi
system-user.dtsi
```

它们放在 `arch/arm/boot/dts/`，该目录 Makefile 需要包含 `system-top.dtb` 编译项。本项目 `system-user.dtsi` 保留实际使用的 NFS 启动参数和 DMA 驱动关联，不照抄书中的 SD 根文件系统参数。

在已设置教程 SDK 环境的终端中，本轮给出的完整重编流程为：

```bash
cd ~/workspace/kernel-driver/linux-xlnx-xlnx_rebase_v5.4_2020.2
cp .config ../kernel-config-before-rebuild.bak
make clean
make xilinx_zynq_defconfig
make -j8
```

其中 `make clean` 是本轮为排除旧产物额外加入的，不是 PDF 原命令。`defconfig` 会重置内核配置，因此先备份。以后已有自己的配置时，不应每次修改设备树都重复执行 defconfig。

编译成功后部署：

```bash
cp arch/arm/boot/zImage /media/zy/boot/zImage
cp arch/arm/boot/dts/system-top.dtb /media/zy/boot/system.dtb
sync
```

用户随后确认可以进入系统。（置信度：高）

但没有保留足够证据判断旧 DTB 究竟是因为依赖、时间戳、目标配置还是先前编译流程未生效，所以“make dtbs 依赖有问题”仍是推测。PDF 第 584 页明确支持仅修改设备树时使用 `make dtbs`，不能把完整重编当作以后每次修改的必要步骤。

## 9. 第三处对应关系：模块改用实际启动的独立内核构建

原驱动 Makefile 执行的是：

```text
petalinux-build -c stream-ctrl
```

而当前 SD 卡的 `zImage` 来自独立内核。即使版本字符串相同，两套构建的配置、符号版本也可能不同。

因此改为：

```makefile
obj-m := stream_ctrl.o

KDIR := $(HOME)/workspace/kernel-driver/linux-xlnx-xlnx_rebase_v5.4_2020.2

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm clean
```

命令行前为 Tab。在设置好 SDK 交叉编译环境的终端中执行 `make`，使用刚才完整构建的内核目录。用户态 `zstream` 不需要仅因内核重编就重新编译。

新模块加载日志为：

```text
stream_ctrl 83cb0000.stream_gen: stream_ctrl: resource = [mem 0x83cb0000-0x83cbffff]
stream_ctrl 83cb0000.stream_gen: stream_ctrl: DMA RX buffer allocated, size=64, dma=0x3f047000
```

`loading out-of-tree module taints kernel` 是外部模块标记，本身不表示加载失败。

## 10. 最终板上结果与“假卡住”

实际执行结果：

```text
./zstream --count 100 --verify
success=100 data_error=0 read_error=0

./zstream --poll --count 100 --verify
success=100 data_error=0 read_error=0
packets=100 poll_events=100

./zstream --poll --seconds 60 --verify
success=315794 data_error=0 read_error=0
packets=315794 poll_events=315795
```

`--seconds 60` 的含义是持续运行 60 秒，程序结束时才打印统计。因此中间没有输出不代表卡住。前面说明命令时没有提前讲清这一点，造成了误判。

poll 通知数和成功读取包数不是同一个计数；截止时可能发生一次通知但未再读取，因此相差 1 不能直接解释成丢包。具体这一次的分支没有跟踪记录。

以上证明本轮阻塞、poll 和 60 秒数据校验通过，不代表更长时间稳定性或所有异常路径都已验证。

终端长命令仍有覆盖显示的问题，但对应命令正常执行。临时缩短提示符的办法是：

```bash
export PS1='\W# '
```

它显示当前目录名；单独执行 export 只对当前 shell 生效。本轮没有确认终端换行问题的根因，也没有完成新的永久配置修改。

## 11. 这次排查应该记住什么

1. 先确认实际加载的产物，再判断源码逻辑。编辑器里正确，不等于板子运行的就是它。
2. 设备树沿“生成源文件 → 内核源文件 → 内核 DTB → SD DTB → 板上读取”逐段追，不跳步猜测。
3. U-Boot 能访问一个外设，不证明全部外设正常；发生器和 DMA 应分开理解。
4. 关闭 DMA 节点后能启动，只证明故障与该初始化路径相关，不证明设备树语法错误。
5. 日志最后一行可能只是最后完成的步骤，调用栈和初始化入口更有定位价值。
6. BOOT.BIN、独立 system.bit、DTB、zImage 和模块各有来源，不能因为文件名相同就认为对应。
7. 前期排查曾过早推测 FSBL、复位或设备树配置；实际更应先确认卡上的 bitstream 和 DTB。以后优先做能区分原因的最小一步。

当前仓库以 `Navigator_7020/` 为上板工程，`device-tree/` 保存新硬件的设备树源文件，根目录 Makefile 使用独立内核；`zynq_stream_dma/` 保留为早期学习工程。
