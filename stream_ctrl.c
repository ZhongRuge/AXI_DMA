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
#include <linux/poll.h>

#include "stream_ctrl.h"

static dev_t stream_dev_num;
static struct class *stream_class;


/* 读取 stream_gen 的 32 位寄存器。 */
static u32 stream_ctrl_read(struct stream_ctrl_dev *sdev, u32 reg)
{
    return readl(sdev->base + reg);
}

/* 写入 stream_gen 的 32 位寄存器。 */
static void stream_ctrl_write(struct stream_ctrl_dev *sdev, u32 reg, u32 value)
{
    writel(value, sdev->base + reg);
}

/* 复位并配置发生器，填充接收缓冲区。 */
static void stream_ctrl_prepare_rx(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;

    stream_ctrl_hw_stop(sdev);

    stream_ctrl_hw_reset(sdev);

    stream_ctrl_write(sdev, STREAM_REG_PACKET_LEN, STREAM_RX_WORDS);
    stream_ctrl_write(sdev, STREAM_REG_RATE_DIV, 1000);

    /* 成功传输后，这个标记应被 0、1、...、15 完整覆盖。 */
    memset(buf->cpu_addr, 0xA5, buf->size);
}

/* 读取设备的接收状态。 */
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

/* 判断下一包数据是否已经就绪。 */
static bool stream_ctrl_has_ready_buffer(struct stream_ctrl_dev *sdev)
{
    unsigned long flags;
    bool ready;
    ready = 0;

    spin_lock_irqsave(&sdev->state_lock, flags);
    if (sdev->rx_buffers[sdev->rx_read_index].state == STREAM_BUF_READY)
        ready = 1;
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    return ready;
}

/* 更新设备的接收状态。 */
static void stream_ctrl_set_rx_state(struct stream_ctrl_dev *sdev,
                                    enum stream_rx_state new_state)
{
    unsigned long flags;
    spin_lock_irqsave(&sdev->state_lock, flags);
    sdev->rx_state = new_state;
    spin_unlock_irqrestore(&sdev->state_lock, flags);
}

/* 按写索引领取空闲缓冲区。 */
static struct stream_rx_buffer *
stream_ctrl_take_free_buffer(struct stream_ctrl_dev *sdev)
{
    struct stream_rx_buffer *buf = &sdev->rx_buffers[sdev->rx_write_index];

    if (buf->state != STREAM_BUF_FREE)
        return NULL;

    buf->state = STREAM_BUF_IN_FLIGHT;
    sdev->rx_write_index = (sdev->rx_write_index + 1) % STREAM_RX_BUF_COUNT;
    return buf;
}

/* 按读索引领取已完成的缓冲区。 */
static struct stream_rx_buffer *
stream_ctrl_take_ready_buffer(struct stream_ctrl_dev *sdev)
{
    struct stream_rx_buffer *buf = &sdev->rx_buffers[sdev->rx_read_index];

    if (buf->state != STREAM_BUF_READY)
        return NULL;

    buf->state = STREAM_BUF_USER_READING;
    sdev->rx_read_index = (sdev->rx_read_index + 1) % STREAM_RX_BUF_COUNT;
    return buf;
}

/* DMA 完成后公布数据，安排下一包并唤醒等待者。 */
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_rx_buffer *buf = args;
    struct stream_ctrl_dev *sdev = buf->sdev;
    unsigned long flags;
    bool notify = false;

    stream_ctrl_hw_stop(sdev);

    spin_lock_irqsave(&sdev->state_lock, flags);
    if (sdev->rx_state == STREAM_RX_IN_FLIGHT &&
        buf->state == STREAM_BUF_IN_FLIGHT) {
        buf->state = STREAM_BUF_READY;
        sdev->stats.rx_packets++;
        sdev->stats.rx_bytes += buf->size;
        sdev->rx_state = STREAM_RX_IDLE;
        /* 在状态锁内排入工作，与取消路径互斥。 */
        schedule_work(&sdev->rx_work);
        notify = true;
    }
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    if (notify)
        wake_up_interruptible(&sdev->rx_wait);
}

/* 为指定缓冲区准备 DMA 描述符和回调。 */
static struct dma_async_tx_descriptor *
prepare_descriptor(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    struct dma_async_tx_descriptor *descriptor;

    /* 请求完成中断回调，并确认这个单次使用的 descriptor。 */
    unsigned long flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

    /* DMA 设备写入 DMA 地址，不能把 CPU 虚拟地址传给硬件。 */
    descriptor = dmaengine_prep_slave_single(sdev->rx_channel,
                                           buf->dma_addr,
                                           buf->size,
                                           DMA_DEV_TO_MEM,
                                           flags);
    if (!descriptor) {
        return NULL;
    }

    /* 保存回调函数，并把本次缓冲区指针传回回调。 */
    descriptor->callback = stream_ctrl_dma_callback;
    descriptor->callback_param = buf;

    return descriptor;
}

/* 提交 DMA 事务，保存 cookie 并启动接收。 */
static int submit_assist(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    struct dma_async_tx_descriptor *descriptor;
    dma_cookie_t temp_cookie;

    descriptor = prepare_descriptor(buf);
    if (!descriptor) {
        return -ENOMEM;
    }

    temp_cookie = dmaengine_submit(descriptor);
    if (dma_submit_error(temp_cookie)) {
        return dma_submit_error(temp_cookie);
    }

    buf->cookie = temp_cookie;

    dma_async_issue_pending(sdev->rx_channel);

    return 0;
}

/* 在内核工作线程中启动下一包接收。 */
static void stream_ctrl_rx_work(struct work_struct *work)
{
    struct stream_ctrl_dev *sdev =
        container_of(work, struct stream_ctrl_dev, rx_work);
    struct stream_rx_buffer *buf = NULL;
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&sdev->state_lock, flags);
    if (sdev->rx_state == STREAM_RX_IDLE) {
        buf = stream_ctrl_take_free_buffer(sdev);
        if (buf) {
            sdev->rx_paused_full = false;
            sdev->rx_state = STREAM_RX_IN_FLIGHT;
        } else if (!sdev->rx_paused_full) {
            sdev->rx_paused_full = true;
            sdev->stats.full_pauses++;
        }
    }
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    if (!buf)
        return;

    stream_ctrl_prepare_rx(buf);
    ret = submit_assist(buf);
    if (ret) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.errors++;
        buf->state = STREAM_BUF_FREE;
        if (sdev->rx_state == STREAM_RX_IN_FLIGHT)
            sdev->rx_state = STREAM_RX_FAULT;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        wake_up_interruptible(&sdev->rx_wait);
        return;
    }

    stream_ctrl_hw_start(sdev);
}

/* 禁止续接，等待工作和 DMA 停止，再回收未交付的数据。 */
static int stream_ctrl_abort_rx(struct stream_ctrl_dev *sdev)
{
    int ret;
    unsigned long flags;
    unsigned int i;

    spin_lock_irqsave(&sdev->state_lock, flags);
    sdev->rx_state = STREAM_RX_CANCELLED;
    sdev->rx_paused_full = false;
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    cancel_work_sync(&sdev->rx_work);
    stream_ctrl_hw_stop(sdev);
    ret = dmaengine_terminate_sync(sdev->rx_channel);
    if (ret) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.errors++;
        sdev->rx_state = STREAM_RX_FAULT;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        wake_up_interruptible(&sdev->rx_wait);
        return ret;
    }
    /* DMA 已停止，回收未交付的数据；正在复制的缓冲区由 read() 归还。 */
    spin_lock_irqsave(&sdev->state_lock, flags);
    for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
        if (sdev->rx_buffers[i].state == STREAM_BUF_READY)
            sdev->stats.discarded++;
        if (sdev->rx_buffers[i].state == STREAM_BUF_IN_FLIGHT ||
            sdev->rx_buffers[i].state == STREAM_BUF_READY) {
            sdev->rx_buffers[i].state = STREAM_BUF_FREE;
        }
    }
    sdev->rx_write_index = 0;
    sdev->rx_read_index = 0;
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    wake_up_interruptible(&sdev->rx_wait);
    return 0;
}

/* 等待数据就绪、故障或取消，超时和信号到达时终止接收。 */
static int stream_ctrl_wait_rx(struct stream_ctrl_dev *sdev)
{
    int ret;
    long wait_ret;
    unsigned long flags;
    enum stream_rx_state rx_state;

    wait_ret = wait_event_interruptible_timeout(
        sdev->rx_wait,
        stream_ctrl_has_ready_buffer(sdev) ||
        stream_ctrl_get_rx_state(sdev) == STREAM_RX_FAULT ||
        stream_ctrl_get_rx_state(sdev) == STREAM_RX_CANCELLED,
        msecs_to_jiffies(1000));
    if (wait_ret == 0) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.timeouts++;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
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

    rx_state = stream_ctrl_get_rx_state(sdev);

    if (rx_state == STREAM_RX_FAULT)
        return -EIO;

    if (rx_state == STREAM_RX_CANCELLED)
        return -ECANCELED;

    return 0;
}

/* 检查指定 DMA 事务是否正常完成。 */
static int stream_ctrl_finish_rx(struct stream_rx_buffer *buf)
{
    int ret;
    unsigned long flags;
    enum dma_status dma_status;
    struct dma_tx_state state = {};
    struct stream_ctrl_dev *sdev = buf->sdev;

    /* 回调到达并不等于 DMA 状态一定正常，还要查询 DMAEngine 状态。 */
    dma_status = dmaengine_tx_status(sdev->rx_channel, buf->cookie, &state);
    if (dma_status != DMA_COMPLETE || state.residue) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.errors++;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        dev_err(sdev->dev,
                "DMA completed with dma_status=%d residue=%u\n",
                dma_status, state.residue);

        /* DMA 状态异常时，不能让 channel 保持在未知状态。 */
        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            return ret;
        return -EIO;
    }

    return 0;
}

/* 打印本次打开周期的接收统计。 */
static void stream_ctrl_log_stats(struct stream_ctrl_dev *sdev)
{
    struct stream_rx_stats stats;
    unsigned long flags;

    spin_lock_irqsave(&sdev->state_lock, flags);
    stats = sdev->stats;
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    dev_info(sdev->dev,
             "rx_packets=%llu rx_bytes=%llu read_packets=%llu "
             "discarded=%llu full_pauses=%llu errors=%llu timeouts=%llu\n",
             (unsigned long long)stats.rx_packets,
             (unsigned long long)stats.rx_bytes,
             (unsigned long long)stats.read_packets,
             (unsigned long long)stats.discarded,
             (unsigned long long)stats.full_pauses,
             (unsigned long long)stats.errors,
             (unsigned long long)stats.timeouts);
}

/* 关联设备对象并增加打开计数。 */
static int stream_ctrl_open(struct inode *inode, struct file* filp)
{
    int ret;
    unsigned long flags;

    struct stream_ctrl_dev *sdev;
    sdev = container_of(inode->i_cdev, struct stream_ctrl_dev, cdev);
    filp->private_data = sdev;

    ret = nonseekable_open(inode, filp);
    if (ret) return ret;

    ret = mutex_lock_interruptible(&sdev->io_lock);
    if (ret) return ret;

    if (sdev->open_count == 0) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        memset(&sdev->stats, 0, sizeof(sdev->stats));
        sdev->rx_paused_full = false;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
    }
    sdev->open_count += 1;

    mutex_unlock(&sdev->io_lock);
    return 0;
}

/* 减少打开计数，最后关闭时清理接收。 */
static int stream_ctrl_release(struct inode *inode, struct file *filp)
{
    int ret;
    struct stream_ctrl_dev *sdev;
    sdev = filp->private_data;

    mutex_lock(&sdev->io_lock);
    sdev->open_count -= 1;

    if (sdev->open_count == 0) {
        /* IDLE 时也可能有排队的工作或尚未读取的数据。 */
        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            dev_err(sdev->dev, "RX cleanup on last close failed: %d\n", ret);
        else
            stream_ctrl_set_rx_state(sdev, STREAM_RX_IDLE);

        stream_ctrl_log_stats(sdev);
    }
    mutex_unlock(&sdev->io_lock);

    return 0;
}

/* 按顺序领取已完成的数据包，复制给用户后归还缓冲区。 */
static ssize_t stream_ctrl_file_read(struct file *filp, char __user *buf,
                                     size_t count, loff_t *ppos)
{
    int ret;
    struct stream_ctrl_dev *sdev;
    enum stream_rx_state rx_state;
    unsigned long flags;
    struct stream_rx_buffer *rx_buffer;

    sdev = filp->private_data;

    if (count == 0) return 0;
    if (count < STREAM_RX_BUF_SIZE) return -EMSGSIZE;

    if (filp->f_flags & O_NONBLOCK) {
        if (!mutex_trylock(&sdev->io_lock)) return -EAGAIN;
    } else {
        ret = mutex_lock_interruptible(&sdev->io_lock);
        if (ret) return ret;
    }

    for (;;) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        rx_state = sdev->rx_state;
        if (rx_state == STREAM_RX_FAULT || rx_state == STREAM_RX_CANCELLED) {
            ret = (rx_state == STREAM_RX_FAULT) ? -EIO : -ECANCELED;
            spin_unlock_irqrestore(&sdev->state_lock, flags);
            goto out_unlock;
        }

        rx_buffer = stream_ctrl_take_ready_buffer(sdev);
        if (rx_state == STREAM_RX_IDLE)
            schedule_work(&sdev->rx_work);
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        if (rx_buffer)
            break;

        if (filp->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            goto out_unlock;
        }

        ret = stream_ctrl_wait_rx(sdev);
        if (ret)
            goto out_unlock;
    }

    ret = stream_ctrl_finish_rx(rx_buffer);
    if (ret)
        goto out_free_buffer;

    if (copy_to_user(buf, rx_buffer->cpu_addr, STREAM_RX_BUF_SIZE))
        ret = -EFAULT;
    else
        ret = STREAM_RX_BUF_SIZE;

out_free_buffer:
    /* 归还缓冲区后，尝试恢复因缓冲区满而暂停的接收。 */
    spin_lock_irqsave(&sdev->state_lock, flags);
    if (ret == STREAM_RX_BUF_SIZE) {
        sdev->stats.read_packets++;
    } else {
        sdev->stats.discarded++;
        if (ret == -EFAULT)
            sdev->stats.errors++;
    }
    rx_buffer->state = STREAM_BUF_FREE;
    if (sdev->rx_state == STREAM_RX_IDLE)
        schedule_work(&sdev->rx_work);
    spin_unlock_irqrestore(&sdev->state_lock, flags);

out_unlock:
    mutex_unlock(&sdev->io_lock);
    return ret;
}

/* 注册等待队列，报告数据可读或错误事件。 */
static __poll_t stream_ctrl_poll(struct file *filp, poll_table *wait)
{
    struct stream_ctrl_dev *sdev;
    enum stream_rx_state rx_state;

    sdev = filp->private_data;
    poll_wait(filp, &sdev->rx_wait, wait);

    rx_state = stream_ctrl_get_rx_state(sdev);

    if (rx_state == STREAM_RX_FAULT || rx_state == STREAM_RX_CANCELLED)
        return POLLERR;

    if (stream_ctrl_has_ready_buffer(sdev))
        return POLLIN | POLLRDNORM;

    return 0;
}

static struct file_operations stream_ctrl_fops = {
    .owner = THIS_MODULE,
    .open = stream_ctrl_open,
    .release = stream_ctrl_release,
    .read = stream_ctrl_file_read,
    .llseek = no_llseek,
    .poll = stream_ctrl_poll
};

/* 释放已分配的 DMA 缓冲区内存。 */
static void stream_ctrl_free_buffers(struct stream_ctrl_dev *sdev)
{
    unsigned int i;
    struct device *dma_chan_dev;

    dma_chan_dev = sdev->rx_channel->device->dev;

    for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
        if (sdev->rx_buffers[i].cpu_addr != NULL) {
            dma_free_coherent(dma_chan_dev,
                              sdev->rx_buffers[i].size,
                              sdev->rx_buffers[i].cpu_addr,
                              sdev->rx_buffers[i].dma_addr);
            sdev->rx_buffers[i].cpu_addr = NULL;
        }
    }
}

/* 分配并初始化四块 DMA 缓冲区，失败时回滚。 */
static int stream_ctrl_alloc_buffers(struct stream_ctrl_dev *sdev)
{
    struct device *dma_chan_dev;
    struct stream_rx_buffer *buf;
    unsigned int i;

    dma_chan_dev = sdev->rx_channel->device->dev;

    for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
        buf = &sdev->rx_buffers[i];

        buf->sdev = sdev;
        buf->size = STREAM_RX_BUF_SIZE;
        buf->state = STREAM_BUF_FREE;
        buf->cookie = 0;

        buf->cpu_addr = dma_alloc_coherent(dma_chan_dev,
                                           STREAM_RX_BUF_SIZE,
                                           &buf->dma_addr,
                                           GFP_KERNEL);
        if (buf->cpu_addr == NULL) {
            stream_ctrl_free_buffers(sdev);
            return -ENOMEM;
        }
    }
    return 0;
}

/* 初始化设备、寄存器映射和 DMA，创建字符设备。 */
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

    /* 初始化读写路径使用的互斥锁。 */
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

    ret = stream_ctrl_alloc_buffers(sdev);
    if (ret)
        goto err_release_channel;

    init_waitqueue_head(&sdev->rx_wait);
    spin_lock_init(&sdev->state_lock);
    INIT_WORK(&sdev->rx_work, stream_ctrl_rx_work);
    sdev->rx_state = STREAM_RX_IDLE;
    sdev->rx_write_index = 0;
    sdev->rx_read_index = 0;

    dev_info(&pdev->dev,
             "stream_ctrl: allocated %u RX buffers, %zu bytes each\n",
             STREAM_RX_BUF_COUNT, sdev->rx_buffers[0].size);

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
    stream_ctrl_free_buffers(sdev);
err_release_channel:
    dma_release_channel(sdev->rx_channel);
    sdev->rx_channel = NULL;

    return ret;
}

/* 撤销字符设备，停止接收并释放 DMA 资源。 */
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

    if (sdev && sdev->rx_channel) {
        stream_ctrl_abort_rx(sdev);
    }

    if (sdev) {
        stream_ctrl_hw_reset(sdev);
    }

    /* 只有 DMA 不再访问 buffer 后，才能释放 coherent 内存。 */
    if (sdev && sdev->rx_channel) {
        stream_ctrl_free_buffers(sdev);
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

/* 启动数据发生器，发送一个数据包。 */
void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write(sdev, STREAM_REG_CTRL, STREAM_CTRL_ENABLE);
}

/* 请求数据发生器停止输出。 */
void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write(sdev, STREAM_REG_CTRL, 0);
}

/* 软件复位数据发生器。 */
void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write(sdev, STREAM_REG_CTRL, STREAM_CTRL_RESET);
}

/* 读取数据发生器的硬件状态。 */
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

/* 申请设备号、创建设备类并注册驱动。 */
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

/* 注销驱动，释放设备类和设备号。 */
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
