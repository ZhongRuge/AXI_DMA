/*
 * 预备：rx_work 领取下一块 FREE，填充标记并准备、提交 DMA。
 * 接收：当前包完成后，由回调或 rx_work 接手 PREPARED，启动下一包。
 * 读取：read 领取 READY -> 检查事务 -> copy_to_user -> 归还 FREE。
 * 停止：STOP/最后一次 close 等已领取的最多两包结束；异常和 RESET 强制终止。
 * io_lock 串行化用户操作；state_lock 保护与工作线程、回调共享的状态。
 */
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "stream_ctrl.h"

static dev_t stream_dev_num;
static struct class *stream_class;

/* 加载时选择，运行期间只读，避免同一轮测量中途改变口径。 */
static bool profile;
module_param(profile, bool, 0444);
MODULE_PARM_DESC(profile, "Collect stage timings (default: off)");

/* 前置声明：便于按阅读顺序排列函数，不执行操作。 */
static void stream_ctrl_free_buffers(struct stream_ctrl_dev *sdev);
static void stream_ctrl_dma_callback(void *args);
static int stream_ctrl_abort_rx(struct stream_ctrl_dev *sdev);
static u64 stream_ctrl_profile_now(void);
static void stream_ctrl_profile_add_locked(struct stream_ctrl_dev *sdev,
                                          enum stream_profile_stage stage,
                                          u64 begin_ns, u64 end_ns);
static void stream_ctrl_profile_finish(struct stream_ctrl_dev *sdev,
                                      enum stream_profile_stage stage,
                                      u64 begin_ns);
static void stream_ctrl_trace_locked(struct stream_ctrl_dev *sdev,
                                     struct stream_rx_buffer *buf,
                                     u32 event, int result, u32 detail);
static void stream_ctrl_trace(struct stream_ctrl_dev *sdev,
                              struct stream_rx_buffer *buf,
                              u32 event, int result, u32 detail);
static int stream_ctrl_get_trace(struct stream_ctrl_dev *sdev,
                                 void __user *user_trace);
static int stream_ctrl_clear_trace(struct stream_ctrl_dev *sdev,
                                   bool explicit_clear);
static void stream_ctrl_log_stats(struct stream_ctrl_dev *sdev);

/* 1. 缓冲区管理：分配、领取、状态查询与释放。 */

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
        buf->data_len = 0;
        buf->state = STREAM_BUF_FREE;
        buf->cookie = 0;

        buf->cpu_addr = dma_alloc_coherent(dma_chan_dev, STREAM_RX_BUF_SIZE,
                                           &buf->dma_addr, GFP_KERNEL);
        if (!buf->cpu_addr) {
            stream_ctrl_free_buffers(sdev);
            return -ENOMEM;
        }
    }

    return 0;
}

/* 释放已分配的 DMA 缓冲区内存。 */
static void stream_ctrl_free_buffers(struct stream_ctrl_dev *sdev)
{
    struct device *dma_chan_dev;
    struct stream_rx_buffer *buf;
    unsigned int i;

    dma_chan_dev = sdev->rx_channel->device->dev;

    for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
        buf = &sdev->rx_buffers[i];
        if (buf->cpu_addr) {
            dma_free_coherent(dma_chan_dev, buf->size,
                              buf->cpu_addr, buf->dma_addr);
            buf->cpu_addr = NULL;
        }
    }
}

/* 按写索引领取 FREE 缓冲区，调用者须持有 state_lock。 */
static struct stream_rx_buffer *
stream_ctrl_take_free_buffer_locked(struct stream_ctrl_dev *sdev)
{
    struct stream_rx_buffer *buf = &sdev->rx_buffers[sdev->rx_write_index];

    if (buf->state != STREAM_BUF_FREE)
        return NULL;

    buf->state = STREAM_BUF_PREPARING;
    buf->data_len = sdev->packet_words * sizeof(u32);
    buf->cookie = 0;
    buf->rx_id = ++sdev->rx_seq;
    buf->dma_start_ns = 0;
    buf->ready_ns = 0;
    stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_RX_BEGIN, 0, 0);
    sdev->rx_write_index = (sdev->rx_write_index + 1) % STREAM_RX_BUF_COUNT;

    return buf;
}

/* 按读索引领取 READY 缓冲区，调用者须持有 state_lock。 */
static struct stream_rx_buffer *
stream_ctrl_take_ready_buffer_locked(struct stream_ctrl_dev *sdev)
{
    struct stream_rx_buffer *buf = &sdev->rx_buffers[sdev->rx_read_index];

    if (buf->state != STREAM_BUF_READY)
        return NULL;

    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_READY_TO_READ,
                                   buf->ready_ns, stream_ctrl_profile_now());
    buf->ready_ns = 0;
    buf->state = STREAM_BUF_USER_READING;
    stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_READ_TAKE, 0, 0);
    sdev->rx_read_index = (sdev->rx_read_index + 1) % STREAM_RX_BUF_COUNT;

    return buf;
}

/* 判断下一包数据是否已经就绪。 */
static bool stream_ctrl_has_ready_buffer(struct stream_ctrl_dev *sdev)
{
    unsigned long flags;
    bool ready;

    spin_lock_irqsave(&sdev->state_lock, flags);
    ready = (sdev->rx_buffers[sdev->rx_read_index].state == STREAM_BUF_READY);
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    return ready;
}

/* 正常 STOP 等待已领取的包，包含正在预备和已经提交的下一包。 */
static bool stream_ctrl_has_unfinished_buffer(struct stream_ctrl_dev *sdev)
{
    unsigned long flags;
    unsigned int i;
    bool unfinished = false;

    spin_lock_irqsave(&sdev->state_lock, flags);
    for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
        if (sdev->rx_buffers[i].state == STREAM_BUF_PREPARING ||
            sdev->rx_buffers[i].state == STREAM_BUF_PREPARED ||
            sdev->rx_buffers[i].state == STREAM_BUF_IN_FLIGHT) {
            unfinished = true;
            break;
        }
    }
    spin_unlock_irqrestore(&sdev->state_lock, flags);
    return unfinished;
}

/* 2. FPGA 寄存器、包长配置与接收准备。 */

/* 读取 stream_gen 的 32 位寄存器。 */
static u32 stream_ctrl_read_reg(struct stream_ctrl_dev *sdev, u32 reg)
{
    return readl(sdev->base + reg);
}

/* 写入 stream_gen 的 32 位寄存器。 */
static void stream_ctrl_write_reg(struct stream_ctrl_dev *sdev, u32 reg,
                                  u32 value)
{
    writel(value, sdev->base + reg);
}

/* 启动数据发生器，发送一个数据包。 */
void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write_reg(sdev, STREAM_REG_CTRL, STREAM_CTRL_ENABLE);
}

/* 请求数据发生器停止输出。 */
void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write_reg(sdev, STREAM_REG_CTRL, 0);
}

/* 软件复位数据发生器。 */
void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write_reg(sdev, STREAM_REG_CTRL, STREAM_CTRL_RESET);
}

/* 读取数据发生器的硬件状态。 */
u32 stream_ctrl_hw_get_status(struct stream_ctrl_dev *sdev)
{
    return stream_ctrl_read_reg(sdev, STREAM_REG_STATUS);
}

/* 仅在旧包全部释放后修改包长，避免新旧包使用不同长度。 */
static int stream_ctrl_set_packet_len(struct stream_ctrl_dev *sdev,
                                      u32 packet_words)
{
    unsigned long flags;
    unsigned int i;
    int ret;

    if (packet_words == 0 || packet_words > STREAM_PKT_WORDS_MAX)
        return -EINVAL;

    ret = mutex_lock_interruptible(&sdev->io_lock);
    if (ret)
        return ret;

    ret = 0;
    spin_lock_irqsave(&sdev->state_lock, flags);
    if (sdev->rx_state == STREAM_RX_FAULT) {
        ret = -EIO;
    } else if (sdev->rx_state == STREAM_RX_IN_FLIGHT) {
        ret = -EBUSY;
    } else {
        /* 旧包全部释放后才能改长度，忙时由调用者先 STOP。 */
        for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
            if (sdev->rx_buffers[i].state != STREAM_BUF_FREE) {
                ret = -EBUSY;
                break;
            }
        }
        if (!ret)
            sdev->packet_words = packet_words;
    }
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    mutex_unlock(&sdev->io_lock);
    return ret;
}

/* 包间复位并按需更新 FPGA 配置；调用者持有 state_lock，当前包须已结束。 */
static void stream_ctrl_configure_hw_locked(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    u32 packet_words = buf->data_len / sizeof(u32);
    u32 rate_div = sdev->rate_div;
    u64 begin_ns = stream_ctrl_profile_now();

    /* RESET 同时停止发生器，但不会清除包长和速率配置。 */
    stream_ctrl_hw_reset(sdev);

    if (!sdev->hw_config_valid || sdev->hw_packet_words != packet_words) {
        stream_ctrl_write_reg(sdev, STREAM_REG_PACKET_LEN, packet_words);
        sdev->hw_packet_words = packet_words;
    }
    if (!sdev->hw_config_valid || sdev->hw_rate_div != rate_div) {
        stream_ctrl_write_reg(sdev, STREAM_REG_RATE_DIV, rate_div);
        sdev->hw_rate_div = rate_div;
    }
    sdev->hw_config_valid = true;
    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_HW_CONFIG,
                                   begin_ns, stream_ctrl_profile_now());
    stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_CONFIG, 0, rate_div);
}

/* 只处理下一包的内存，不复位或改动正在发送当前包的 FPGA。 */
static void stream_ctrl_prepare_buffer(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    u64 begin_ns = stream_ctrl_profile_now();

    /* 正常接收后，0xA5 应被整包数据覆盖。 */
    memset(buf->cpu_addr, 0xA5, buf->data_len);
    stream_ctrl_profile_finish(sdev, STREAM_PROFILE_BUFFER_FILL, begin_ns);
}

/* 3. 接收启动、DMA 提交与完成回调。 */

/* 读取设备的接收状态。 */
static enum stream_rx_state
stream_ctrl_get_rx_state(struct stream_ctrl_dev *sdev)
{
    enum stream_rx_state state;
    unsigned long flags;

    spin_lock_irqsave(&sdev->state_lock, flags);
    state = sdev->rx_state;
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    return state;
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

/* 启动或恢复接收；已有事务时保持原接收，故障时返回错误。 */
static int stream_ctrl_start_rx(struct stream_ctrl_dev *sdev)
{
    unsigned long flags;
    int ret;

    ret = mutex_lock_interruptible(&sdev->io_lock);
    if (ret)
        return ret;

    ret = 0;
    spin_lock_irqsave(&sdev->state_lock, flags);
    stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_START,
                             sdev->rx_state == STREAM_RX_FAULT ? -EIO : 0, 0);
    if (sdev->rx_state == STREAM_RX_CANCELLED ||
        sdev->rx_state == STREAM_RX_IDLE) {
        sdev->rx_state = STREAM_RX_IDLE;
        schedule_work(&sdev->rx_work);
    } else if (sdev->rx_state == STREAM_RX_FAULT) {
        ret = -EIO;
    }
    /* IN_FLIGHT 时保持当前接收，重复 START 仍返回成功。 */
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    mutex_unlock(&sdev->io_lock);
    return ret;
}

/* 为指定缓冲区准备 DMA 描述符和回调。 */
static struct dma_async_tx_descriptor *
stream_ctrl_prepare_descriptor(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    struct dma_async_tx_descriptor *descriptor;
    unsigned long dma_flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

    /* 硬件使用 dma_addr；CPU 访问缓冲区时使用 cpu_addr。 */
    descriptor = dmaengine_prep_slave_single(sdev->rx_channel,
                                             buf->dma_addr,
                                             buf->data_len,
                                             DMA_DEV_TO_MEM,
                                             dma_flags);
    if (!descriptor)
        return NULL;

    /* 回调通过这个指针找到本次完成的缓冲区。 */
    descriptor->callback = stream_ctrl_dma_callback;
    descriptor->callback_param = buf;

    return descriptor;
}

/* 在线程中准备并提交；不启动 FPGA，描述符提交后交由 DMA 驱动管理。 */
static int stream_ctrl_submit_rx(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    struct dma_async_tx_descriptor *descriptor;
    dma_cookie_t cookie;
    unsigned long flags;
    u64 begin_ns;

    begin_ns = stream_ctrl_profile_now();
    descriptor = stream_ctrl_prepare_descriptor(buf);
    stream_ctrl_profile_finish(sdev, STREAM_PROFILE_DMA_PREP, begin_ns);
    if (!descriptor) {
        stream_ctrl_trace(sdev, buf, STREAM_TRACE_PREP_FAILED, -ENOMEM, 0);
        return -ENOMEM;
    }

    begin_ns = stream_ctrl_profile_now();
    cookie = dmaengine_submit(descriptor);
    stream_ctrl_profile_finish(sdev, STREAM_PROFILE_DMA_SUBMIT, begin_ns);
    if (dma_submit_error(cookie)) {
        stream_ctrl_trace(sdev, buf, STREAM_TRACE_SUBMIT,
                          dma_submit_error(cookie), 0);
        return dma_submit_error(cookie);
    }

    spin_lock_irqsave(&sdev->state_lock, flags);
    buf->cookie = cookie;
    stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_SUBMIT, 0, 0);
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    return 0;
}

/* 接手已经提交的下一包；调用者持有 state_lock，不在这里分配内存。 */
static void stream_ctrl_start_prepared_locked(struct stream_ctrl_dev *sdev)
{
    struct stream_rx_buffer *buf = sdev->rx_next;
    u64 begin_ns;

    if (sdev->rx_active || !buf || buf->state != STREAM_BUF_PREPARED)
        return;
    if (sdev->rx_state != STREAM_RX_IDLE &&
        sdev->rx_state != STREAM_RX_IN_FLIGHT &&
        sdev->rx_state != STREAM_RX_STOPPING)
        return;

    sdev->rx_next = NULL;
    sdev->rx_active = buf;
    buf->state = STREAM_BUF_IN_FLIGHT;
    if (sdev->rx_state != STREAM_RX_STOPPING)
        sdev->rx_state = STREAM_RX_IN_FLIGHT;

    stream_ctrl_configure_hw_locked(buf);
    stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_ISSUE_REQUEST, 0, 0);
    begin_ns = stream_ctrl_profile_now();
    /* 底层可能已在上包完成时接手此描述符；重复 issue 不会重复启动它。 */
    dma_async_issue_pending(sdev->rx_channel);
    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_DMA_ISSUE,
                                   begin_ns, stream_ctrl_profile_now());

    buf->dma_start_ns = stream_ctrl_profile_now();
    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_READY_TO_NEXT_RX,
                                   sdev->last_ready_ns, buf->dma_start_ns);
    sdev->last_ready_ns = 0;
    stream_ctrl_hw_start(sdev);
    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_START_WRITE,
                                   buf->dma_start_ns, stream_ctrl_profile_now());
    stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_START_CMD, 0, 0);
}

/* 当前包传输时提前预备一包；第一包或预备较慢时也在这里完成启动。 */
static void stream_ctrl_rx_work(struct work_struct *work)
{
    struct stream_ctrl_dev *sdev =
        container_of(work, struct stream_ctrl_dev, rx_work);
    struct stream_rx_buffer *buf;
    unsigned long flags;
    u64 prepare_begin_ns;
    int ret;

    for (;;) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        if ((sdev->rx_state != STREAM_RX_IDLE &&
             sdev->rx_state != STREAM_RX_IN_FLIGHT) || sdev->rx_next) {
            spin_unlock_irqrestore(&sdev->state_lock, flags);
            return;
        }

        buf = stream_ctrl_take_free_buffer_locked(sdev);
        if (buf) {
            sdev->rx_next = buf;
            prepare_begin_ns = stream_ctrl_profile_now();
            stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_FULL_PAUSE,
                                           sdev->full_pause_ns, prepare_begin_ns);
            sdev->full_pause_ns = 0;
            if (sdev->rx_paused_full)
                stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_FULL_RESUME,
                                         0, 0);
            sdev->rx_paused_full = false;
        } else if (!sdev->rx_active && !sdev->rx_paused_full) {
            sdev->rx_paused_full = true;
            sdev->full_pause_ns = stream_ctrl_profile_now();
            sdev->stats.full_pauses++;
            stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_FULL_PAUSE, 0, 0);
        }
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        if (!buf)
            return;

        stream_ctrl_prepare_buffer(buf);
        /* 已领取的包完成 prep/submit 配对，避免留下无法通用释放的描述符。 */
        ret = stream_ctrl_submit_rx(buf);
        if (ret) {
            spin_lock_irqsave(&sdev->state_lock, flags);
            sdev->rx_next = NULL;
            buf->state = STREAM_BUF_FREE;
            if (sdev->rx_state != STREAM_RX_CANCELLED) {
                sdev->stats.errors++;
                sdev->rx_state = STREAM_RX_FAULT;
            }
            spin_unlock_irqrestore(&sdev->state_lock, flags);
            wake_up_all(&sdev->rx_wait);
            return;
        }

        spin_lock_irqsave(&sdev->state_lock, flags);
        buf->state = STREAM_BUF_PREPARED;
        /* CANCELLED/FAULT 不启动；abort 等本工作退出后统一终止和回收。 */
        stream_ctrl_start_prepared_locked(sdev);
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        wake_up_all(&sdev->rx_wait);
        /* 第一包已启动时可继续准备下一包；rx_next 非空时循环立即退出。 */
    }
}

/* 回调公布 READY 并立即接手已预备的包；STOPPING 只排空已经领取的包。 */
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_rx_buffer *buf = args;
    struct stream_ctrl_dev *sdev = buf->sdev;
    unsigned long flags;
    bool notify = false;
    u64 callback_begin_ns = stream_ctrl_profile_now();

    spin_lock_irqsave(&sdev->state_lock, flags);
    if ((sdev->rx_state == STREAM_RX_IN_FLIGHT ||
         sdev->rx_state == STREAM_RX_STOPPING ||
         sdev->rx_state == STREAM_RX_FAULT) &&
        sdev->rx_active == buf &&
        buf->state == STREAM_BUF_IN_FLIGHT) {
        stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_START_TO_CALLBACK,
                                       buf->dma_start_ns, callback_begin_ns);
        buf->dma_start_ns = 0;
        stream_ctrl_hw_stop(sdev);
        buf->ready_ns = stream_ctrl_profile_now();
        sdev->last_ready_ns = buf->ready_ns;
        buf->state = STREAM_BUF_READY;
        sdev->rx_active = NULL;
        sdev->stats.rx_packets++;
        sdev->stats.rx_bytes += buf->data_len;
        stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_CALLBACK_READY, 0, 0);

        /* 已提交的下一包可能已被底层接手，正常 STOP 也让它完整结束。 */
        if (sdev->rx_state == STREAM_RX_IN_FLIGHT) {
            sdev->rx_state = STREAM_RX_IDLE;
        }
        stream_ctrl_start_prepared_locked(sdev);
        if (sdev->rx_state == STREAM_RX_IDLE ||
            sdev->rx_state == STREAM_RX_IN_FLIGHT) {
            schedule_work(&sdev->rx_work);
        }
        notify = true;
    } else {
        stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_CALLBACK_IGNORED,
                                 0, sdev->rx_state);
    }
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    /* 同时通知可中断的 read/poll 等待和正常 STOP 的限时等待。 */
    if (notify)
        wake_up_all(&sdev->rx_wait);
    stream_ctrl_profile_finish(sdev, STREAM_PROFILE_CALLBACK_BODY,
                               callback_begin_ns);
}

/* 4. 等待数据与检查 DMA 事务。 */

/* read 持有 io_lock 等数据；超时或信号到达时强制终止接收。 */
static int stream_ctrl_wait_rx(struct stream_ctrl_dev *sdev)
{
    enum stream_rx_state rx_state;
    long wait_ret;
    unsigned long flags;
    int ret;

    /* 等待数据，也等待故障或取消通知。 */
    wait_ret = wait_event_interruptible_timeout(
        sdev->rx_wait,
        stream_ctrl_has_ready_buffer(sdev) ||
        stream_ctrl_get_rx_state(sdev) == STREAM_RX_FAULT ||
        stream_ctrl_get_rx_state(sdev) == STREAM_RX_CANCELLED,
        msecs_to_jiffies(STREAM_READ_TIMEOUT_MS));

    if (wait_ret == 0) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.timeouts++;
        stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_WAIT_TIMEOUT,
                                 -ETIMEDOUT, STREAM_READ_TIMEOUT_MS);
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            return ret;
        return -ETIMEDOUT;
    }

    if (wait_ret < 0) {
        stream_ctrl_trace(sdev, NULL, STREAM_TRACE_WAIT_INTERRUPTED,
                          (int)wait_ret, 0);
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

/* read 交付前检查本包 DMA 结果，异常时强制停止接收。 */
static int stream_ctrl_check_rx_result(struct stream_rx_buffer *buf)
{
    struct stream_ctrl_dev *sdev = buf->sdev;
    struct dma_tx_state state = {};
    enum dma_status dma_status;
    unsigned long flags;
    u64 begin_ns;
    int ret;

    /* 此接口只提供 DMA 状态；完整数据内容仍由用户态校验。 */
    begin_ns = stream_ctrl_profile_now();
    dma_status = dmaengine_tx_status(sdev->rx_channel, buf->cookie, &state);
    stream_ctrl_profile_finish(sdev, STREAM_PROFILE_DMA_STATUS, begin_ns);
    if (dma_status != DMA_COMPLETE || state.residue) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.errors++;
        stream_ctrl_trace_locked(sdev, buf, STREAM_TRACE_DMA_CHECK_FAILED,
                                 -EIO, dma_status);
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        dev_err(sdev->dev,
                "DMA completed with dma_status=%d residue=%u\n",
                dma_status, state.residue);

        /* 状态异常时停止本轮接收。 */
        ret = stream_ctrl_abort_rx(sdev);
        if (ret)
            return ret;
        return -EIO;
    }

    return 0;
}

/* 5. 正常停止与强制清理。 */

/* 正常停止：禁止新预备，限时排空已领取的包，再清理；调用者持有 io_lock。 */
static int stream_ctrl_stop_rx(struct stream_ctrl_dev *sdev)
{
    unsigned long flags;
    long wait_ret;
    int ret = 0;
    int cleanup_ret;

    spin_lock_irqsave(&sdev->state_lock, flags);
    /* 已完成清理的 STOP 不重复终止 DMA，便于随后 close。 */
    if (sdev->rx_state == STREAM_RX_CANCELLED) {
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        return 0;
    }
    if (sdev->rx_state != STREAM_RX_FAULT)
        sdev->rx_state = STREAM_RX_STOPPING;
    stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_STOP_WAIT,
                             0, STREAM_STOP_TIMEOUT_MS);
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    /* 不再领取新包；正在准备的包完成提交，由工作线程或回调启动并排空。 */
    cancel_work_sync(&sdev->rx_work);

    if (stream_ctrl_get_rx_state(sdev) == STREAM_RX_FAULT) {
        ret = -EIO;
    } else {
        /* 不持有自旋锁，也不提前关闭发生器；回调负责唤醒。 */
        wait_ret = wait_event_timeout(sdev->rx_wait,
                    !stream_ctrl_has_unfinished_buffer(sdev) ||
                    stream_ctrl_get_rx_state(sdev) == STREAM_RX_FAULT,
                    msecs_to_jiffies(STREAM_STOP_TIMEOUT_MS));
        if (stream_ctrl_get_rx_state(sdev) == STREAM_RX_FAULT) {
            ret = -EIO;
        } else if (wait_ret == 0) {
            spin_lock_irqsave(&sdev->state_lock, flags);
            sdev->stats.timeouts++;
            sdev->stats.errors++;
            stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_STOP_TIMEOUT,
                                     -ETIMEDOUT, STREAM_STOP_TIMEOUT_MS);
            spin_unlock_irqrestore(&sdev->state_lock, flags);
            ret = -ETIMEDOUT;
        }
    }

    /* 无论正常完成还是等待失败，都执行终止与回调同步后再回收。 */
    cleanup_ret = stream_ctrl_abort_rx(sdev);
    if (cleanup_ret)
        return cleanup_ret;
    return ret;
}

/* 强制终止并同步旧回调，再回收未交付数据；正常调用者持有 io_lock。 */
static int stream_ctrl_abort_rx(struct stream_ctrl_dev *sdev)
{
    int ret;
    unsigned long flags;
    unsigned int i;

    spin_lock_irqsave(&sdev->state_lock, flags);
    stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_STOP_BEGIN, 0, 0);
    sdev->rx_state = STREAM_RX_CANCELLED;
    /* 没有恢复领取便停止时，也结束这次满缓冲区等待。 */
    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_FULL_PAUSE,
                                   sdev->full_pause_ns, stream_ctrl_profile_now());
    sdev->full_pause_ns = 0;
    sdev->last_ready_ns = 0;
    sdev->rx_paused_full = false;
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    cancel_work_sync(&sdev->rx_work);
    stream_ctrl_hw_stop(sdev);

    /* 底层仍须负责停止硬件；其未上报的停止失败不由 RCU 修复。 */
    ret = dmaengine_terminate_sync(sdev->rx_channel);
    if (ret) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->stats.errors++;
        sdev->rx_state = STREAM_RX_FAULT;
        stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_STOP_END, ret, 0);
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        wake_up_interruptible(&sdev->rx_wait);
        return ret;
    }

    /*
     * Xilinx 2020.2 的终止路径清空描述符队列，但不等待 tasklet。
     * 在 5.4 非 RT 内核中，RCU 等待已运行的旧软中断处理退出。
     * 此处不持有 state_lock；等待结束前禁止新提交和缓冲区复用。
     */
    synchronize_rcu();

    /* 保留 USER_READING 缓冲区，由持有它的 read() 归还。 */
    spin_lock_irqsave(&sdev->state_lock, flags);
    for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
        if (sdev->rx_buffers[i].state == STREAM_BUF_READY)
            sdev->stats.discarded++;
        if (sdev->rx_buffers[i].state == STREAM_BUF_PREPARING ||
            sdev->rx_buffers[i].state == STREAM_BUF_PREPARED ||
            sdev->rx_buffers[i].state == STREAM_BUF_IN_FLIGHT ||
            sdev->rx_buffers[i].state == STREAM_BUF_READY) {
            sdev->rx_buffers[i].state = STREAM_BUF_FREE;
            sdev->rx_buffers[i].dma_start_ns = 0;
            sdev->rx_buffers[i].ready_ns = 0;
        }
    }
    sdev->rx_active = NULL;
    sdev->rx_next = NULL;
    sdev->rx_write_index = 0;
    sdev->rx_read_index = 0;
    stream_ctrl_trace_locked(sdev, NULL, STREAM_TRACE_STOP_END, 0, 0);
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    wake_up_interruptible(&sdev->rx_wait);
    return 0;
}

/* 6. 字符设备入口：打开、读取、通知、控制与关闭。 */

/* 关联设备对象；只读查询不加入收发周期，也不清零统计。 */
static int stream_ctrl_open(struct inode *inode, struct file *filp)
{
    struct stream_ctrl_dev *sdev;
    unsigned long flags;
    int ret;

    sdev = container_of(inode->i_cdev, struct stream_ctrl_dev, cdev);
    filp->private_data = sdev;

    ret = nonseekable_open(inode, filp);
    if (ret)
        return ret;

    if (!(filp->f_mode & FMODE_WRITE))
        return 0;

    ret = mutex_lock_interruptible(&sdev->io_lock);
    if (ret)
        return ret;

    if (sdev->open_count == 0) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        memset(&sdev->stats, 0, sizeof(sdev->stats));
        memset(&sdev->profile_stats, 0, sizeof(sdev->profile_stats));
        sdev->profile_stats.enabled = profile;
        sdev->last_ready_ns = 0;
        sdev->full_pause_ns = 0;
        sdev->rx_paused_full = false;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
    }
    sdev->open_count += 1;

    mutex_unlock(&sdev->io_lock);
    return 0;
}

/* 按顺序领取已完成的数据包，复制给用户后归还缓冲区。 */
static ssize_t stream_ctrl_file_read(struct file *filp, char __user *user_buf,
                                     size_t count, loff_t *ppos)
{
    struct stream_ctrl_dev *sdev;
    struct stream_rx_buffer *rx_buffer;
    enum stream_rx_state rx_state;
    unsigned long flags;
    u64 copy_begin_ns;
    int ret;

    sdev = filp->private_data;

    /* 只读句柄只查询诊断信息，不消费数据或隐式启动 DMA。 */
    if (!(filp->f_mode & FMODE_WRITE))
        return -EPERM;

    if (count == 0)
        return 0;

    /* io_lock 串行化读取；非阻塞模式拿不到锁就立即返回。 */
    if (filp->f_flags & O_NONBLOCK) {
        if (!mutex_trylock(&sdev->io_lock))
            return -EAGAIN;
    } else {
        ret = mutex_lock_interruptible(&sdev->io_lock);
        if (ret)
            return ret;
    }

    for (;;) {
        spin_lock_irqsave(&sdev->state_lock, flags);
        rx_state = sdev->rx_state;
        if (rx_state == STREAM_RX_FAULT || rx_state == STREAM_RX_CANCELLED) {
            if (rx_state == STREAM_RX_FAULT)
                ret = -EIO;
            else
                ret = -ECANCELED;

            spin_unlock_irqrestore(&sdev->state_lock, flags);
            goto out_unlock;
        }

        /* 包长修改也持有 io_lock；空间不足时不领取、不丢弃数据包。 */
        if (count < sdev->packet_words * sizeof(u32)) {
            spin_unlock_irqrestore(&sdev->state_lock, flags);
            ret = -EMSGSIZE;
            goto out_unlock;
        }

        rx_buffer = stream_ctrl_take_ready_buffer_locked(sdev);
        if (rx_state == STREAM_RX_IDLE)
            schedule_work(&sdev->rx_work);
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        if (rx_buffer)
            break;

        if (filp->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            goto out_unlock;
        }

        /* 等数据前已释放 state_lock，回调才能更新状态。 */
        ret = stream_ctrl_wait_rx(sdev);
        if (ret)
            goto out_unlock;
    }

    ret = stream_ctrl_check_rx_result(rx_buffer);
    if (ret)
        goto out_free_buffer;

    /* 用户内存访问可能睡眠，不能放在 state_lock 内。 */
    copy_begin_ns = stream_ctrl_profile_now();
    if (copy_to_user(user_buf, rx_buffer->cpu_addr, rx_buffer->data_len))
        ret = -EFAULT;
    else
        ret = rx_buffer->data_len;
    stream_ctrl_profile_finish(sdev, STREAM_PROFILE_COPY_TO_USER, copy_begin_ns);

out_free_buffer:
    /* 归还缓冲区后，尝试恢复因缓冲区满而暂停的接收。 */
    spin_lock_irqsave(&sdev->state_lock, flags);
    if (ret > 0) {
        sdev->stats.read_packets++;
    } else {
        sdev->stats.discarded++;
        if (ret == -EFAULT)
            sdev->stats.errors++;
    }
    rx_buffer->state = STREAM_BUF_FREE;
    stream_ctrl_trace_locked(sdev, rx_buffer, STREAM_TRACE_READ_DONE, ret, 0);
    /* 即使当前包仍在接收，归还的缓冲区也可以立即用于预备下一包。 */
    if (sdev->rx_state == STREAM_RX_IDLE ||
        sdev->rx_state == STREAM_RX_IN_FLIGHT)
        schedule_work(&sdev->rx_work);
    spin_unlock_irqrestore(&sdev->state_lock, flags);

out_unlock:
    mutex_unlock(&sdev->io_lock);
    return ret;
}

/* 登记等待关系，只报告可读或错误状态。 */
static __poll_t stream_ctrl_poll(struct file *filp, poll_table *wait)
{
    struct stream_ctrl_dev *sdev;
    enum stream_rx_state rx_state;

    sdev = filp->private_data;

    if (!(filp->f_mode & FMODE_WRITE))
        return POLLERR;

    /* 这里只登记通知，睡眠由内核 poll 框架处理。 */
    poll_wait(filp, &sdev->rx_wait, wait);

    rx_state = stream_ctrl_get_rx_state(sdev);

    if (rx_state == STREAM_RX_FAULT || rx_state == STREAM_RX_CANCELLED)
        return POLLERR;

    if (stream_ctrl_has_ready_buffer(sdev))
        return POLLIN | POLLRDNORM;

    return 0;
}

/* ioctl 负责取参数和分发；启停、包长等具体动作由对应函数处理。 */
static long stream_ctrl_ioctl(struct file *filp,
                              unsigned int cmd,
                              unsigned long arg)
{
    struct stream_ctrl_dev *sdev;
    struct stream_rx_stats stats;
    struct stream_profile_stats profile_stats;
    struct stream_trace_error trace_error;
    u32 rate_div;
    u32 packet_words;
    unsigned long flags;
    int ret;

    sdev = filp->private_data;

    /* 查询句柄仅实现 GET 接口，禁止通过 ioctl 修改设备或日志。 */
    if (!(filp->f_mode & FMODE_WRITE) &&
        cmd != STREAM_IOC_GET_TRACE && cmd != STREAM_IOC_GET_STATS &&
        cmd != STREAM_IOC_GET_PROFILE)
        return -ENOTTY;

    switch (cmd) {
    case STREAM_IOC_GET_PROFILE:
        spin_lock_irqsave(&sdev->state_lock, flags);
        profile_stats = sdev->profile_stats;
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        if (copy_to_user((void __user *)arg, &profile_stats, sizeof(profile_stats)))
            return -EFAULT;
        return 0;

    case STREAM_IOC_GET_TRACE:
        return stream_ctrl_get_trace(sdev, (void __user *)arg);

    case STREAM_IOC_CLR_TRACE:
        return stream_ctrl_clear_trace(sdev, true);

    case STREAM_IOC_PREP_TRACE:
        return stream_ctrl_clear_trace(sdev, false);

    case STREAM_IOC_FREEZE_TRACE:
        if (copy_from_user(&trace_error, (void __user *)arg,
                           sizeof(trace_error)))
            return -EFAULT;
        if (trace_error.error >= 0 || trace_error.error < -MAX_ERRNO)
            return -EINVAL;
        if (trace_error.event != STREAM_TRACE_USER_POLL_TIMEOUT &&
            trace_error.event != STREAM_TRACE_USER_IO_ERROR &&
            trace_error.event != STREAM_TRACE_USER_DATA_ERROR)
            return -EINVAL;

        /* 用户态异常不强行关联当前 DMA，它可能发生在上一包的校验中。 */
        stream_ctrl_trace(sdev, NULL, trace_error.event, trace_error.error, 0);
        return 0;

    case STREAM_IOC_START:
        return stream_ctrl_start_rx(sdev);

    case STREAM_IOC_STOP:
        ret = mutex_lock_interruptible(&sdev->io_lock);
        if (ret)
            return ret;
        ret = stream_ctrl_stop_rx(sdev);
        mutex_unlock(&sdev->io_lock);
        return ret;

    case STREAM_IOC_RESET:
        ret = mutex_lock_interruptible(&sdev->io_lock);
        if (ret)
            return ret;

        ret = stream_ctrl_abort_rx(sdev);
        if (!ret) {
            stream_ctrl_hw_reset(sdev);
            stream_ctrl_trace(sdev, NULL, STREAM_TRACE_RESET_CMD, 0, 0);
        }
        /* 清理成功后保持 CANCELLED，由后续 START 恢复接收。 */
        mutex_unlock(&sdev->io_lock);
        return ret;

    case STREAM_IOC_SET_PKT_LEN:
        if (copy_from_user(&packet_words, (void __user *)arg,
                           sizeof(packet_words)))
            return -EFAULT;
        return stream_ctrl_set_packet_len(sdev, packet_words);

    case STREAM_IOC_CLR_STATS:
        spin_lock_irqsave(&sdev->state_lock, flags);
        memset(&sdev->stats, 0, sizeof(sdev->stats));
        spin_unlock_irqrestore(&sdev->state_lock, flags);
        return 0;

    case STREAM_IOC_GET_STATS:
        spin_lock_irqsave(&sdev->state_lock, flags);
        stats = sdev->stats;
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        ret = copy_to_user((void __user *)arg, &stats, sizeof(stats));
        if (ret != 0)
            return -EFAULT;
        return 0;

    case STREAM_IOC_SET_RATE:
        ret = copy_from_user(&rate_div, (void __user *)arg, sizeof(rate_div));
        if (ret)
            return -EFAULT;
        if (rate_div > STREAM_RATE_DIV_MAX)
            return -EINVAL;

        spin_lock_irqsave(&sdev->state_lock, flags);
        sdev->rate_div = rate_div;
        spin_unlock_irqrestore(&sdev->state_lock, flags);

        return 0;

    default:
        return -ENOTTY;
    }
}

/* 查询句柄直接关闭，最后一个收发/控制句柄关闭时清理接收。 */
static int stream_ctrl_release(struct inode *inode, struct file *filp)
{
    struct stream_ctrl_dev *sdev;
    int ret;

    if (!(filp->f_mode & FMODE_WRITE))
        return 0;

    sdev = filp->private_data;

    mutex_lock(&sdev->io_lock);
    sdev->open_count -= 1;

    if (sdev->open_count == 0) {
        /* IDLE 时也可能有排队的工作或尚未读取的数据。 */
        ret = stream_ctrl_stop_rx(sdev);
        if (ret)
            dev_err(sdev->dev, "RX cleanup on last close failed: %d\n", ret);
        else
            stream_ctrl_set_rx_state(sdev, STREAM_RX_IDLE);

        stream_ctrl_log_stats(sdev);
    }

    mutex_unlock(&sdev->io_lock);

    return 0;
}

static struct file_operations stream_ctrl_fops = {
    .owner          = THIS_MODULE,
    .open           = stream_ctrl_open,
    .release        = stream_ctrl_release,
    .read           = stream_ctrl_file_read,
    .llseek         = no_llseek,
    .poll           = stream_ctrl_poll,
    .unlocked_ioctl = stream_ctrl_ioctl,
};

/* 7. 辅助计时、事件日志与统计。 */

/* 未开启分段计时时返回 0，调用者据此跳过统计。 */
static u64 stream_ctrl_profile_now(void)
{
    return profile ? ktime_get_ns() : 0;
}

/* 累加一个完整区间；调用者须持有 state_lock。 */
static void stream_ctrl_profile_add_locked(struct stream_ctrl_dev *sdev,
                                          enum stream_profile_stage stage,
                                          u64 begin_ns, u64 end_ns)
{
    struct stream_timing_sample *sample;
    u64 elapsed_ns;

    if (!begin_ns || end_ns < begin_ns)
        return;

    elapsed_ns = end_ns - begin_ns;
    sample = &sdev->profile_stats.stages[stage];
    sample->count++;
    sample->total_ns += elapsed_ns;
    if (elapsed_ns > sample->max_ns)
        sample->max_ns = elapsed_ns;
}

/* 操作结束立即取时间，再持锁累加；统计更新不算进该操作耗时。 */
static void stream_ctrl_profile_finish(struct stream_ctrl_dev *sdev,
                                      enum stream_profile_stage stage,
                                      u64 begin_ns)
{
    unsigned long flags;
    u64 end_ns;

    if (!begin_ns)
        return;

    end_ns = ktime_get_ns();
    spin_lock_irqsave(&sdev->state_lock, flags);
    stream_ctrl_profile_add_locked(sdev, stage, begin_ns, end_ns);
    spin_unlock_irqrestore(&sdev->state_lock, flags);
}

/* 追加一条事件，首个错误冻结历史；调用者须持有 state_lock。 */
static void stream_ctrl_trace_locked(struct stream_ctrl_dev *sdev,
                                     struct stream_rx_buffer *buf,
                                     u32 event, int result, u32 detail)
{
    struct stream_trace_entry *entry;
    u64 begin_ns;

    if (sdev->trace.frozen)
        return;

    begin_ns = stream_ctrl_profile_now();
    entry = &sdev->trace.entries[sdev->trace_head];
    entry->seq = ++sdev->trace_seq;
    entry->time_ns = ktime_get_ns();
    entry->rx_id = buf ? buf->rx_id : 0;
    entry->cookie = buf ? buf->cookie : 0;
    entry->buffer_index = buf ? (int)(buf - sdev->rx_buffers) : -1;
    entry->event = event;
    entry->result = result;
    entry->data_len = buf ? buf->data_len : 0;
    entry->detail = detail;

    sdev->trace_head = (sdev->trace_head + 1) % STREAM_TRACE_CAPACITY;
    if (sdev->trace.count < STREAM_TRACE_CAPACITY)
        sdev->trace.count++;
    else
        sdev->trace.overwritten++;

    if (result < 0)
        sdev->trace.frozen = 1;

    stream_ctrl_profile_add_locked(sdev, STREAM_PROFILE_TRACE_WRITE,
                                   begin_ns, stream_ctrl_profile_now());
}

/* 为未持有 state_lock 的调用者记录事件。 */
static void stream_ctrl_trace(struct stream_ctrl_dev *sdev,
                              struct stream_rx_buffer *buf,
                              u32 event, int result, u32 detail)
{
    unsigned long flags;

    spin_lock_irqsave(&sdev->state_lock, flags);
    stream_ctrl_trace_locked(sdev, buf, event, result, detail);
    spin_unlock_irqrestore(&sdev->state_lock, flags);
}

/* 取得一致的历史副本，释放自旋锁后再复制给用户。 */
static int stream_ctrl_get_trace(struct stream_ctrl_dev *sdev,
                                 void __user *user_trace)
{
    struct stream_trace_log *snapshot;
    unsigned long flags;
    unsigned int first;
    unsigned int i;
    int ret;

    /* 历史较大，不放在内核栈上；只在用户查询时分配。 */
    snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
    if (!snapshot)
        return -ENOMEM;

    spin_lock_irqsave(&sdev->state_lock, flags);
    snapshot->count = sdev->trace.count;
    snapshot->frozen = sdev->trace.frozen;
    snapshot->overwritten = sdev->trace.overwritten;
    /* 从最旧事件开始复制，把环形存储整理成顺序数组。 */
    first = (sdev->trace_head + STREAM_TRACE_CAPACITY - snapshot->count) %
            STREAM_TRACE_CAPACITY;
    for (i = 0; i < snapshot->count; i++)
        snapshot->entries[i] =
            sdev->trace.entries[(first + i) % STREAM_TRACE_CAPACITY];
    spin_unlock_irqrestore(&sdev->state_lock, flags);

    ret = copy_to_user(user_trace, snapshot, sizeof(*snapshot)) ? -EFAULT : 0;
    kfree(snapshot);
    return ret;
}

/* 空闲时清理历史，只有显式清空才允许解除故障冻结。 */
static int stream_ctrl_clear_trace(struct stream_ctrl_dev *sdev,
                                   bool explicit_clear)
{
    unsigned long flags;
    unsigned int i;
    int ret;

    ret = mutex_lock_interruptible(&sdev->io_lock);
    if (ret)
        return ret;

    ret = 0;
    spin_lock_irqsave(&sdev->state_lock, flags);
    /* 检查和清理共用状态锁，避免中途冻结的新故障被自动清掉。 */
    if (!explicit_clear && sdev->trace.frozen) {
        ret = -EALREADY;
    } else {
        for (i = 0; i < STREAM_RX_BUF_COUNT; i++) {
            if (sdev->rx_buffers[i].state != STREAM_BUF_FREE) {
                ret = -EBUSY;
                break;
            }
        }
    }
    if (!ret) {
        sdev->trace_head = 0;
        sdev->trace.count = 0;
        sdev->trace.overwritten = 0;
        sdev->trace.frozen = 0;
    }
    spin_unlock_irqrestore(&sdev->state_lock, flags);
    mutex_unlock(&sdev->io_lock);
    return ret;
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

/* 8. 设备初始化、资源释放与驱动注册。 */

/* 初始化设备、寄存器映射和 DMA，创建字符设备。 */
static int stream_ctrl_probe(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;
    struct resource *res;
    u32 version;
    int ret;

    /* 回调同步补充依赖普通 tasklet 的执行环境。 */
    if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
        dev_err(&pdev->dev,
                "stream_ctrl: callback synchronization requires a non-RT kernel\n");
        return -EOPNOTSUPP;
    }

    /* 设备私有数据由 devm 管理。 */
    sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return -ENOMEM;

    sdev->rate_div = STREAM_RATE_DIV_DEFAULT;
    sdev->packet_words = STREAM_PKT_WORDS_DEFAULT;
    sdev->profile_stats.enabled = profile;

    sdev->dev = &pdev->dev;
    mutex_init(&sdev->io_lock);

    /* 映射发生器寄存器，并检查硬件版本。 */
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

    version = stream_ctrl_read_reg(sdev, STREAM_REG_VERSION);
    if (version != STREAM_CTRL_VERSION) {
        dev_err(&pdev->dev,
                "stream_ctrl: incompatible stream_gen version 0x%08x, "
                "expected 0x%08x\n",
                version, STREAM_CTRL_VERSION);
        return -ENODEV;
    }

    /* 保存设备指针，供 remove() 使用。 */
    platform_set_drvdata(pdev, sdev);

    /* 按设备树中的 rx 名称申请 DMA 通道，再分配缓冲区。 */
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

    /* 发布字符设备入口。 */
    cdev_init(&sdev->cdev, &stream_ctrl_fops);
    sdev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&sdev->cdev, stream_dev_num, 1);
    if (ret) {
        dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);
        goto err_free_dma;
    }

    sdev->dev_node = device_create(stream_class, &pdev->dev,
                                   stream_dev_num, sdev, "zynq_stream0");
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

    /* 释放内存的前提是 DMA 和回调已不再访问它。 */
    if (sdev && sdev->rx_channel) {
        stream_ctrl_free_buffers(sdev);
    }

    /* 缓冲区释放后，再归还 DMA 通道。 */
    if (sdev && sdev->rx_channel) {
        dma_release_channel(sdev->rx_channel);
        sdev->rx_channel = NULL;
    }

    if (sdev)
        dev_info(&pdev->dev, "stream_ctrl: resources will be released by devm\n");

    return 0;
}

static const struct of_device_id stream_ctrl_of_match[] = {
    { .compatible = STREAM_CTRL_COMPATIBLE },
    { }
};

MODULE_DEVICE_TABLE(of, stream_ctrl_of_match);

static struct platform_driver stream_ctrl_driver = {
    .probe  = stream_ctrl_probe,
    .remove = stream_ctrl_remove,
    .driver = {
        .name           = STREAM_CTRL_DRV_NAME,
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
