#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "stream_ctrl.h"

static u32 stream_ctrl_read(struct stream_ctrl_dev *sdev, u32 reg)
{
    return readl(sdev->base + reg);
}

static void stream_ctrl_write(struct stream_ctrl_dev *sdev, u32 reg, u32 value)
{
    writel(value, sdev->base + reg);
}

/*
 * 为一次 DMA 接收准备 stream_gen 和 RX buffer。
 *
 * 这个函数只负责准备硬件和内存，不负责准备或提交 descriptor，也不会
 * 启动 stream_gen。调用顺序很重要：
 *
 *   1. 停止数据生产者，避免继续产生 AXI-Stream 数据；
 *   2. 复位 RTL 中的数据序列、计数器和状态；
 *   3. 配置数据包长度和输出速率；
 *   4. 用标记值填充 buffer，便于发现 DMA 未完整写入；
 *   5. 在提交 descriptor 前重新准备 completion。
 *
 * RTL 中的软件复位是一个单周期命令，会清除序列计数器、传输计数器，
 * 并自动清除 CTRL。软件不需要再单独清除 reset 位。PACKET_LEN 和
 * RATE_DIV 属于配置寄存器，不会被该软件复位清除，因此复位后要重新写入。
 *
 * 调用者必须保证之前没有 DMA 传输仍在使用这个 buffer。这里的内存来自
 * dma_alloc_coherent()，CPU 和 DMA 对它具有一致性，因此 memset 后不需要
 * 额外调用 dma_sync_*()。
 */
static void stream_ctrl_prepare_rx(struct stream_ctrl_dev *sdev)
{
    /* 修改硬件状态或 buffer 内容前，先停止数据生产者。 */
    stream_ctrl_hw_stop(sdev);

    /* 复位数据序列、计数器、状态以及尚未完成的 AXI-Stream 输出。 */
    stream_ctrl_hw_reset(sdev);

    /* 使用一个包含 16 个 word 的数据包，不设置额外的数据间隔。 */
    stream_ctrl_write(sdev, STREAM_REG_PACKET_LEN, STREAM_RX_WORDS);
    stream_ctrl_write(sdev, STREAM_REG_RATE_DIV, 1000);

    /* 成功传输后，这个标记应被 0、1、...、15 完整覆盖。 */
    memset(sdev->rx_buf, 0xA5, sdev->rx_buf_size);

    /* 提交新的 DMA 工作前，丢弃上一次的 completion 状态。 */
    reinit_completion(&sdev->rx_completion);
}

/*
 * DMAEngine 完成回调。
 *
 * 这个回调可能运行在 DMAEngine 的中断或 tasklet 相关上下文中，因此不能
 * 睡眠、遍历 buffer 或输出大量日志。complete() 只记录完成事件，并唤醒
 * 处于进程上下文中的等待路径。
 */
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_ctrl_dev *sdev = args;

    stream_ctrl_hw_stop(sdev);
    complete(&sdev->rx_completion);
}

/*
 * 准备一次单 buffer S2MM descriptor。
 *
 * dmaengine_prep_slave_single() 只创建并准备 descriptor，不会把它提交到
 * channel，也不会启动硬件传输。
 */
static struct dma_async_tx_descriptor *
prepare_descriptor(struct stream_ctrl_dev *sdev)
{
    struct dma_async_tx_descriptor *descriptor;

    /* 请求完成中断回调，并确认这个单次使用的 descriptor。 */
    unsigned long flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

    /* DMA 设备写入 DMA 地址，不能把 CPU 虚拟地址传给硬件。 */
    descriptor = dmaengine_prep_slave_single(sdev->rx_channel,
                                              sdev->rx_dma_addr,
                                              sdev->rx_buf_size,
                                              DMA_DEV_TO_MEM,
                                              flags);
    if (!descriptor) {
        return NULL;
    }

    /* 保存回调函数，并把 sdev 作为不透明上下文传回回调。 */
    descriptor->callback = stream_ctrl_dma_callback;
    descriptor->callback_param = sdev;

    return descriptor;
}

/*
 * 提交一个已经准备好的 descriptor，并把它推入 RX channel 的 pending 队列。
 *
 * DMAEngine 特意把传输分成三个阶段：
 *
 *   prep          -> 创建并准备 descriptor；
 *   submit        -> 获得 cookie，并把 descriptor 放入 channel 队列；
 *   issue_pending -> 通知 channel 开始处理队列中的传输。
 *
 * 只有 dmaengine_submit() 成功后，cookie_out 才会被写入有效 cookie。调用者
 * 可以用这个 cookie 查询本次特定传输的 DMA 状态。
 */
static int submit_assist(struct stream_ctrl_dev *sdev,
                         dma_cookie_t *cookie_out)
{
    struct dma_async_tx_descriptor *descriptor;
    dma_cookie_t temp_cookie;

    descriptor = prepare_descriptor(sdev);
    if (!descriptor) {
        return -ENOMEM;
    }

    temp_cookie = dmaengine_submit(descriptor);
    if (dma_submit_error(temp_cookie)) {
        return dma_submit_error(temp_cookie);
    }

    *cookie_out = temp_cookie;

    dma_async_issue_pending(sdev->rx_channel);

    return 0;
}

/*
 * 执行一次完整的 RX 传输，但暂不校验 buffer 数据。
 *
 * 这个函数必须运行在允许睡眠的进程上下文中，因为
 * wait_for_completion_timeout() 可能睡眠。DMA 完成回调不会调用这个函数，
 * 回调只负责 complete(&sdev->rx_completion)。
 */
static int stream_ctrl_receive_once(struct stream_ctrl_dev *sdev)
{
    dma_cookie_t cookie;
    struct dma_tx_state state = {};
    enum dma_status status;
    unsigned long wait_ret;
    int ret;

    /* 把 RTL、buffer 和 completion 都准备到确定的初始状态。 */
    stream_ctrl_prepare_rx(sdev);

    /* 先让 DMA descriptor 就绪，再允许 stream_gen 产生数据。 */
    ret = submit_assist(sdev, &cookie);
    if (ret) {
        dev_err(sdev->dev, "submit failed: %d\n", ret);
        return ret;
    }

    /* 只有 S2MM channel 准备完成后，才启动数据生产者。 */
    stream_ctrl_hw_start(sdev);

    /* 等待 DMAEngine 回调，但不能无限等待。 */
    wait_ret = wait_for_completion_timeout(
        &sdev->rx_completion,
        msecs_to_jiffies(1000));
    if (!wait_ret) {
        /* 在终止 channel 前保存 DMA 状态，便于分析超时原因。 */
        status = dmaengine_tx_status(sdev->rx_channel, cookie, &state);

        /* 先停止数据生产者，再终止 DMA 并等待回调结束。 */
        stream_ctrl_hw_stop(sdev);
        dmaengine_terminate_sync(sdev->rx_channel);

        dev_err(sdev->dev, "completion timeout, dma status=%d\n", status);
        return -ETIMEDOUT;
    }

    /* buffer 已完成本次请求，停止 generator，避免它继续产生数据。 */
    stream_ctrl_hw_stop(sdev);

    /* completion 到达并不等于 DMA 状态一定正常，还要查询 DMAEngine 状态。 */
    status = dmaengine_tx_status(sdev->rx_channel, cookie, &state);
    if (status != DMA_COMPLETE) {
        dev_err(sdev->dev, "DMA completed with unexpected status=%d\n", status);

        /* DMA 状态异常时，不能让 channel 保持在未知状态。 */
        dmaengine_terminate_sync(sdev->rx_channel);
        return -EIO;
    }

    /*
     * 当前应用是一次性单包接收。复位 S2MM 通道，
     * 清除 DMA 内部可能缓存的下一包部分数据。
     */
    ret = dmaengine_terminate_sync(sdev->rx_channel);
    if (ret) {
        dev_err(sdev->dev,
                "failed to reset RX DMA channel: %d\n",
                ret);
        return ret;
    }
    
    dev_info(sdev->dev,
         "RX counters after stop: words=%u, packets=%u\n",
         stream_ctrl_read(sdev, STREAM_REG_WORD_COUNT),
         stream_ctrl_read(sdev, STREAM_REG_PACKET_COUNT));

    return 0;
}

/*
 * 在进程上下文中校验一次 RX buffer 的内容。
 *
 * stream_gen.v 已经确认：复位后第一个数据为 0，之后每个数据递增 1。
 * 当前 PACKET_LEN 为 16，因此这里期望 buffer 中依次出现 0 到 15。
 *
 * 这个函数故意不逐个打印所有 word，只记录第一个错误的位置、期望值、
 * 实际值和总错误数。这样既能定位问题，也不会让内核日志被大量输出占满。
 * DMA buffer 来自 dma_alloc_coherent()，DMA 完成后 CPU 可以直接通过这个
 * 虚拟地址读取内容。
 */
static int stream_ctrl_validate_rx(struct stream_ctrl_dev *sdev)
{
    u32 *data = sdev->rx_buf;
    u32 first_expected = 0;
    u32 first_actual = 0;
    unsigned int error_count = 0;
    unsigned int i;
    int first_error = -1;

    for (i = 0; i < STREAM_RX_WORDS; i++) {
        u32 expected = i;
        u32 actual = data[i];

        if (actual == expected)
            continue;

        if (first_error < 0) {
            first_error = i;
            first_expected = expected;
            first_actual = actual;
        }

        error_count++;
    }

    if (error_count) {
        dev_err(sdev->dev,
                "RX data validation failed: first index=%d, "
                "expected=0x%08x, actual=0x%08x, errors=%u\n",
                first_error,
                first_expected,
                first_actual,
                error_count);
        return -EIO;
    }

    dev_info(sdev->dev,
             "RX data validation passed: words=%u\n",
             STREAM_RX_WORDS);

    return 0;
}

/* 读取并打印 stream_gen 的寄存器 */
static void stream_ctrl_dump_regs(struct stream_ctrl_dev *sdev)
{
    u32 ctrl;
    u32 status;
    u32 packet_len;
    u32 rate_div;
    u32 word_count;
    u32 packet_count;
    u32 backpressure_count;
    u32 version;

    ctrl               = stream_ctrl_read(sdev, STREAM_REG_CTRL);
    status             = stream_ctrl_read(sdev, STREAM_REG_STATUS);
    packet_len         = stream_ctrl_read(sdev, STREAM_REG_PACKET_LEN);
    rate_div           = stream_ctrl_read(sdev, STREAM_REG_RATE_DIV);
    word_count         = stream_ctrl_read(sdev, STREAM_REG_WORD_COUNT);
    packet_count       = stream_ctrl_read(sdev, STREAM_REG_PACKET_COUNT);
    backpressure_count = stream_ctrl_read(sdev, STREAM_REG_BACKPRESSURE_COUNT);
    version            = stream_ctrl_read(sdev, STREAM_REG_VERSION);

    dev_info(sdev->dev, "stream_ctrl: CTRL               = 0x%08x\n", ctrl);
    dev_info(sdev->dev, "stream_ctrl: STATUS             = 0x%08x\n", status);
    dev_info(sdev->dev, "stream_ctrl: PACKET_LEN         = 0x%08x\n", packet_len);
    dev_info(sdev->dev, "stream_ctrl: RATE_DIV           = 0x%08x\n", rate_div);
    dev_info(sdev->dev, "stream_ctrl: WORD_COUNT         = 0x%08x\n", word_count);
    dev_info(sdev->dev, "stream_ctrl: PACKET_COUNT       = 0x%08x\n", packet_count);
    dev_info(sdev->dev, "stream_ctrl: BACKPRESSURE_COUNT = 0x%08x\n", backpressure_count);
    dev_info(sdev->dev, "stream_ctrl: VERSION            = 0x%08x\n", version);
}

static int stream_ctrl_probe(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;
    struct resource *res;
    int ret;

    /* devm_kzalloc() 会把 sdev 的生命周期绑定到 platform device。 */
    sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return -ENOMEM;

    sdev->dev = &pdev->dev;

    /* probe 时初始化一次；之后每次传输使用 reinit_completion()。 */
    init_completion(&sdev->rx_completion);

    /* 获取并映射 stream_gen 的 MMIO 资源。 */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(&pdev->dev, "stream_ctrl: failed to get MEM resource\n");
        return -ENODEV;
    }
    sdev->res = res;
    dev_info(&pdev->dev, "stream_ctrl: resource = %pR\n", res);

    sdev->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(sdev->base)) {
        dev_err(&pdev->dev, "stream_ctrl: ioremap failed\n");
        return PTR_ERR(sdev->base);
    }

    /* 保存 sdev，使 remove() 和后续驱动操作可以取得设备私有数据。 */
    platform_set_drvdata(pdev, sdev);

    /* 根据设备树中的 "rx" 名称申请 DMAEngine 接收 channel。 */
    sdev->rx_channel = dma_request_chan(&pdev->dev, "rx");
    if (IS_ERR(sdev->rx_channel)) {
        ret = PTR_ERR(sdev->rx_channel);

        if (ret != -EPROBE_DEFER)
            dev_err(&pdev->dev,
                    "stream_ctrl: failed to request RX DMA channel: %d\n",
                    ret);

        return ret;
    }

    /* 分配一个由 CPU 和 AXI DMA 共同访问的 coherent buffer。 */
    sdev->rx_buf_size = STREAM_RX_BUF_SIZE;
    sdev->rx_buf = dma_alloc_coherent(sdev->dev,
                                       sdev->rx_buf_size,
                                       &sdev->rx_dma_addr,
                                       GFP_KERNEL);
    if (sdev->rx_buf == NULL) {
        /* buffer 分配失败时，必须回滚前面已经申请的 channel。 */
        dma_release_channel(sdev->rx_channel);
        sdev->rx_channel = NULL;
        return -ENOMEM;
    }

    dev_info(&pdev->dev,
             "stream_ctrl: DMA RX buffer allocated, size=%zu, dma=%pad\n",
             sdev->rx_buf_size, &sdev->rx_dma_addr);

    ret = stream_ctrl_receive_once(sdev);
    if (!ret)
        ret = stream_ctrl_validate_rx(sdev);

    if (!ret) {
        dev_info(sdev->dev, "stream_ctrl: starting second rx transfer.\n");
        ret = stream_ctrl_receive_once(sdev);
        if (!ret)
            ret = stream_ctrl_validate_rx(sdev);
    }

    if (ret) {
        dev_err(sdev->dev,
                "stream_ctrl: RX transfer failed: %d\n",
                ret);
        stream_ctrl_dump_regs(sdev);

        dma_free_coherent(sdev->dev,
                          sdev->rx_buf_size,
                          sdev->rx_buf,
                          sdev->rx_dma_addr);
        sdev->rx_buf = NULL;
        sdev->rx_dma_addr = 0;
        sdev->rx_buf_size = 0;

        dma_release_channel(sdev->rx_channel);
        sdev->rx_channel = NULL;

        return ret;
    }

    /* 一次性 RX 传输成功后，读取并打印 stream_gen 寄存器。 */
    stream_ctrl_dump_regs(sdev);

    return 0;
}

static int stream_ctrl_remove(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;

    sdev = platform_get_drvdata(pdev);
    dev_info(&pdev->dev, "stream_ctrl: remove called\n");

    if (sdev) {
        stream_ctrl_hw_stop(sdev);
    }

    /*
     * 终止 pending 或 active 状态的传输，并等待 callback 结束，然后才能
     * 释放 callback 或 DMAEngine 可能仍然访问的内存。
     */
    if (sdev && sdev->rx_channel) {
        dmaengine_terminate_sync(sdev->rx_channel);
    }

    if (sdev) {
        stream_ctrl_hw_reset(sdev);
    }

    /* 只有 DMA 不再访问 buffer 后，才能释放 coherent 内存。 */
    if (sdev && sdev->rx_buf) {
        dma_free_coherent(sdev->dev,
                          sdev->rx_buf_size,
                          sdev->rx_buf,
                          sdev->rx_dma_addr);
        sdev->rx_buf = NULL;
        sdev->rx_dma_addr = 0;
        sdev->rx_buf_size = 0;
    }

    /* 释放 DMA 可见的 buffer 后，再释放 DMA channel。 */
    if (sdev && sdev->rx_channel) {
        dma_release_channel(sdev->rx_channel);
        sdev->rx_channel = NULL;
    }

    if (sdev)
        dev_info(&pdev->dev, "stream_ctrl: resources will be released by devm\n");

    return 0;
}

void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev)
{
    /* 设置 CTRL.ENABLE，使 stream_gen 开始产生 AXI-Stream 数据。 */
    stream_ctrl_write(sdev, STREAM_REG_CTRL, STREAM_CTRL_ENABLE);
}

void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev)
{
    /* 清除 CTRL.ENABLE，停止继续产生新的数据。 */
    stream_ctrl_write(sdev, STREAM_REG_CTRL, 0);
}

void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev)
{
    /*
     * 写入单周期软件复位命令。RTL 会自动清除 CTRL 和复位状态，软件不需要
     * 再单独写一次 0 来清除 reset 位。
     */
    stream_ctrl_write(sdev, STREAM_REG_CTRL, STREAM_CTRL_RESET);
}

u32 stream_ctrl_hw_get_status(struct stream_ctrl_dev *sdev)
{
    return stream_ctrl_read(sdev, STREAM_REG_STATUS);
}

static const struct of_device_id stream_ctrl_of_match[] = {
    { .compatible = STREAM_CTRL_COMPATIBLE },
    { }
};

MODULE_DEVICE_TABLE(of, stream_ctrl_of_match);

static struct platform_driver stream_ctrl_driver = {
    .probe = stream_ctrl_probe,
    .remove = stream_ctrl_remove,
    .driver = {
        .name = STREAM_CTRL_DRV_NAME,
        .of_match_table = stream_ctrl_of_match,
    }
};

module_platform_driver(stream_ctrl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZRG");
MODULE_DESCRIPTION("Zynq stream control platform driver");
