#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "stream_ctrl.h"

static dev_t stream_dev_num;
static struct class *stream_class;


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
 *   5. 为本次接收准备 DMA buffer。
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

}

static enum stream_rx_state
stream_ctrl_get_rx_state(struct stream_ctrl_dev *sdev)
{
    unsigned long flags;
    enum stream_rx_state new_state;
    spin_lock_irqsave(&sdev->state_lock, flags);
    new_state = sdev->rx_state;
    spin_unlock_irqrestore(&sdev->state_lock, flags);
    return new_state;
}

static void stream_ctrl_set_rx_state(struct stream_ctrl_dev *sdev,
                                    enum stream_rx_state new_state)
{
    unsigned long flags;
    spin_lock_irqsave(&sdev->state_lock, flags);
    sdev->rx_state = new_state;
    spin_unlock_irqrestore(&sdev->state_lock, flags);
}

/*
 * DMAEngine 完成回调。
 *
 * 这个回调可能运行在 DMAEngine 的中断或 tasklet 相关上下文中，因此不能
 * 睡眠、遍历 buffer 或输出大量日志。回调只更新接收状态并唤醒
 * 处于进程上下文中的等待路径。
 */
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_ctrl_dev *sdev = args;

    stream_ctrl_hw_stop(sdev);
    stream_ctrl_set_rx_state(sdev, STREAM_RX_DONE);
    wake_up_interruptible(&sdev->rx_wait);
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

static int stream_ctrl_abort_rx(struct stream_ctrl_dev *sdev)
{
    int ret;
    stream_ctrl_hw_stop(sdev);
    ret = dmaengine_terminate_sync(sdev->rx_channel);
    if (ret) {
        stream_ctrl_set_rx_state(sdev, STREAM_RX_FAULT);
        return ret;
    }
    stream_ctrl_set_rx_state(sdev, STREAM_RX_IDLE);
    return 0;
}

/*
 * 执行一次完整的 RX 传输，但暂不校验 buffer 数据。
 *
 * 这个函数必须运行在允许睡眠的进程上下文中，因为等待数据时可能睡眠。
 * DMA 完成回调不会调用这个函数，只负责更新接收状态并唤醒等待路径。
 */
static int stream_ctrl_receive_once(struct stream_ctrl_dev *sdev)
{
    dma_cookie_t cookie;
    struct dma_tx_state state = {};
    enum dma_status dma_status;
    enum stream_rx_state stream_rx_state;
    long wait_ret;
    int ret;

    stream_rx_state = stream_ctrl_get_rx_state(sdev);
    if (stream_rx_state == STREAM_RX_FAULT) {
        return -EIO;
    }

    /* 把 RTL、buffer 和接收状态准备到确定的初始状态。 */
    stream_ctrl_prepare_rx(sdev);

    stream_ctrl_set_rx_state(sdev, STREAM_RX_IN_FLIGHT);

    /* 先让 DMA descriptor 就绪，再允许 stream_gen 产生数据。 */
    ret = submit_assist(sdev, &cookie);
    if (ret) {
        stream_ctrl_set_rx_state(sdev, STREAM_RX_IDLE);
        dev_err(sdev->dev, "submit failed: %d\n", ret);
        return ret;
    }

    /* 只有 S2MM channel 准备完成后，才启动数据生产者。 */
    stream_ctrl_hw_start(sdev);

    /* 等待 DMAEngine 回调，但不能无限等待。 */
    wait_ret = wait_event_interruptible_timeout(
        sdev->rx_wait,
        stream_ctrl_get_rx_state(sdev) == STREAM_RX_DONE,
        msecs_to_jiffies(1000));
    if (wait_ret == 0) {
        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            return ret;
        return -ETIMEDOUT;
    }

    if (wait_ret < 0) {
        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            return ret;
        return wait_ret;
    }
    /* buffer 已完成本次请求，停止 generator，避免它继续产生数据。 */
    stream_ctrl_hw_stop(sdev);

    /* 回调到达并不等于 DMA 状态一定正常，还要查询 DMAEngine 状态。 */
    dma_status = dmaengine_tx_status(sdev->rx_channel, cookie, &state);
    if (dma_status != DMA_COMPLETE || state.residue) {
        dev_err(sdev->dev,
                "DMA completed with dma_status=%d residue=%u\n",
                dma_status, state.residue);

        /* DMA 状态异常时，不能让 channel 保持在未知状态。 */
        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            return ret;
        return -EIO;
    }

    /* 正常完成后终止 DMA，但保留 DONE，直到 read() 完成数据拷贝。 */
    ret = dmaengine_terminate_sync(sdev->rx_channel);
    if (ret) {
        dev_err(sdev->dev,
                "failed to reset RX DMA channel: %d\n",
                ret);
        stream_ctrl_set_rx_state(sdev, STREAM_RX_FAULT);
        return ret;
    }

    dev_dbg(sdev->dev,
        "RX counters after stop: words=%u, packets=%u\n",
        stream_ctrl_read(sdev, STREAM_REG_WORD_COUNT),
        stream_ctrl_read(sdev, STREAM_REG_PACKET_COUNT));

    return 0;
}

/* 读取并打印 stream_gen 的寄存器 */
static int stream_ctrl_open(struct inode *inode, struct file* filp)
{
    struct stream_ctrl_dev *sdev;
    sdev = container_of(inode->i_cdev, struct stream_ctrl_dev, cdev);
    filp->private_data = sdev;
    return nonseekable_open(inode, filp);
}

static int stream_ctrl_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t stream_ctrl_file_read(struct file *filp, char __user *buf,
                                     size_t count, loff_t *ppos)
{
    int ret;
    struct stream_ctrl_dev *sdev;
    sdev = filp->private_data;

    if (count == 0) return 0;
    if (count < STREAM_RX_BUF_SIZE) return -EMSGSIZE;

    ret = mutex_lock_interruptible(&sdev->io_lock);
    if (ret) {
        dev_err(sdev->dev, "mutex lock acquire failed!\n");
        return ret;
    }

    ret = stream_ctrl_receive_once(sdev);
    if (ret)
        goto out_unlock;

    if (copy_to_user(buf, sdev->rx_buf, STREAM_RX_BUF_SIZE))
        ret = -EFAULT;
    else
        ret = STREAM_RX_BUF_SIZE;
    
    stream_ctrl_set_rx_state(sdev, STREAM_RX_IDLE);

out_unlock:
    mutex_unlock(&sdev->io_lock);
    return ret;
}

static struct file_operations stream_ctrl_fops = {
    .owner = THIS_MODULE,
    .open = stream_ctrl_open,
    .release = stream_ctrl_release,
    .read = stream_ctrl_file_read,
    .llseek = no_llseek
};

static int stream_ctrl_probe(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;
    struct resource *res;
    u32 version;
    int ret;

    /* devm_kzalloc() 会把 sdev 的生命周期绑定到 platform device。 */
    sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return -ENOMEM;

    sdev->dev = &pdev->dev;

    /* probe 时初始化一次接收等待队列、状态锁和初始状态。 */

    
    mutex_init(&sdev->io_lock);

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

    version = stream_ctrl_read(sdev, STREAM_REG_VERSION);
    if (version != STREAM_CTRL_VERSION) {
        dev_err(&pdev->dev,
                "stream_ctrl: incompatible stream_gen version 0x%08x, "
                "expected 0x%08x\n",
                version, STREAM_CTRL_VERSION);
        return -ENODEV;
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

    init_waitqueue_head(&sdev->rx_wait);
    spin_lock_init(&sdev->state_lock);
    sdev->rx_state = STREAM_RX_IDLE;

    dev_info(&pdev->dev,
             "stream_ctrl: DMA RX buffer allocated, size=%zu, dma=%pad\n",
             sdev->rx_buf_size, &sdev->rx_dma_addr);

    cdev_init(&sdev->cdev, &stream_ctrl_fops);
    sdev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&sdev->cdev, stream_dev_num, 1);
    if (ret) {
        dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);
        goto err_free_dma;
    }

    sdev->dev_node = device_create(stream_class, &pdev->dev, stream_dev_num, sdev, "zynq_stream0");
    if (IS_ERR(sdev->dev_node)) {
        ret = PTR_ERR(sdev->dev_node);
        dev_err(&pdev->dev, "device_create failed: %d\n", ret);
        goto err_cdev_del;
    }

    return 0;

err_cdev_del:
    cdev_del(&sdev->cdev);
err_free_dma:
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

static int stream_ctrl_remove(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;

    sdev = platform_get_drvdata(pdev);
    dev_info(&pdev->dev, "stream_ctrl: remove called\n");

    if (sdev) {
        cdev_del(&sdev->cdev);
        stream_ctrl_hw_stop(sdev);
    }

    device_destroy(stream_class, stream_dev_num);

    /*
     * 终止 pending 或 active 状态的传输，并等待 callback 结束，然后才能
     * 释放 callback 或 DMAEngine 可能仍然访问的内存。
     */
    if (sdev && sdev->rx_channel) {
        stream_ctrl_abort_rx(sdev);
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

static int __init stream_ctrl_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&stream_dev_num, 0, 1, "zynq_stream");
    if (ret) {
        pr_err("alloc_chrdev_region failed: ret=%d\n", ret);
        return ret;
    }

    pr_info("major:%u, minor:%u\n", MAJOR(stream_dev_num), MINOR(stream_dev_num));

    stream_class = class_create(THIS_MODULE, "zynq_stream");
    if (IS_ERR(stream_class)) {
        ret = PTR_ERR(stream_class);
        pr_err("class_create failed: ret=%d\n", ret);
        unregister_chrdev_region(stream_dev_num, 1);
        return ret;
    }

    ret = platform_driver_register(&stream_ctrl_driver);
    if (ret) {
        pr_err("platform_driver_register failed: ret=%d\n", ret);
        class_destroy(stream_class);
        unregister_chrdev_region(stream_dev_num, 1);
        return ret;
    }
    return 0;
}

static void __exit stream_ctrl_exit(void)
{
    platform_driver_unregister(&stream_ctrl_driver);
    class_destroy(stream_class);
    unregister_chrdev_region(stream_dev_num, 1);
}

module_init(stream_ctrl_init);
module_exit(stream_ctrl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZRG");
MODULE_DESCRIPTION("Zynq stream control platform driver");
