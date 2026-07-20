#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "stream_ctrl.h"

static u32 stream_ctrl_read(struct stream_ctrl_dev *sdev, u32 reg)
{
    return readl(sdev->base + reg);
}

static void stream_ctrl_write(struct stream_ctrl_dev *sdev, u32 reg, u32 value)
{
    writel(value, sdev->base + reg);
}

/* DMAEngine 完成回调：只通知等待路径，不执行耗时操作 */
static void stream_ctrl_dma_callback(void *args)
{
    struct stream_ctrl_dev *sdev = args;

    complete(&sdev->rx_completion);
}

/* 准备一次 RX 单次传输 descriptor */
static struct dma_async_tx_descriptor *
prepare_descriptor(struct stream_ctrl_dev *sdev)
{
    struct dma_async_tx_descriptor *descriptor;
    unsigned long flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

    descriptor = dmaengine_prep_slave_single(sdev->rx_channel,
                                              sdev->rx_dma_addr,
                                              sdev->rx_buf_size,
                                              DMA_DEV_TO_MEM,
                                              flags);
    if (!descriptor) {
        return NULL;
    }

    /* 绑定 DMA 完成回调及其设备上下文 */
    descriptor->callback = stream_ctrl_dma_callback;
    descriptor->callback_param = sdev;

    return descriptor;
}

/* 提交 RX descriptor，并推送 channel 的 pending 队列 */
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

    /* 创建并保存设备私有对象 */
    sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return -ENOMEM;

    sdev->dev = &pdev->dev;

    init_completion(&sdev->rx_completion);

    /* 获取并映射 MMIO 资源 */
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

    /* 绑定 pdev 和 sdev，便于后续访问 */
    platform_set_drvdata(pdev, sdev);

    sdev->rx_channel = dma_request_chan(&pdev->dev, "rx");
    if (IS_ERR(sdev->rx_channel)) {
        int ret = PTR_ERR(sdev->rx_channel);

        if (ret != -EPROBE_DEFER)
            dev_err(&pdev->dev,
                    "stream_ctrl: failed to request RX DMA channel: %d\n",
                    ret);

        return ret;
    }

    sdev->rx_buf_size = STREAM_RX_BUF_SIZE;
    sdev->rx_buf = dma_alloc_coherent(sdev->dev,
                                       sdev->rx_buf_size,
                                       &sdev->rx_dma_addr,
                                       GFP_KERNEL);
    if (sdev->rx_buf == NULL) {
        dma_release_channel(sdev->rx_channel);
        sdev->rx_channel = NULL;
        return -ENOMEM;
    }

    dev_info(&pdev->dev,
             "stream_ctrl: DMA RX buffer allocated, size=%zu, dma=%pad\n",
             sdev->rx_buf_size, &sdev->rx_dma_addr);

    stream_ctrl_dump_regs(sdev);

    return 0;
}

static int stream_ctrl_remove(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;
    sdev = platform_get_drvdata(pdev);
    dev_info(&pdev->dev, "stream_ctrl: remove called\n");

    /* coherent buffer 必须先于 DMA channel 释放 */
    if (sdev && sdev->rx_buf) {
        dma_free_coherent(sdev->dev,
                          sdev->rx_buf_size,
                          sdev->rx_buf,
                          sdev->rx_dma_addr);
        sdev->rx_buf = NULL;
        sdev->rx_dma_addr = 0;
        sdev->rx_buf_size = 0;
    }

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
    /* 启动 stream_gen 数据输出 */
    stream_ctrl_write(sdev, STREAM_REG_CTRL, STREAM_CTRL_ENABLE);
}

void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev)
{
    /* 停止 stream_gen 数据输出 */
    stream_ctrl_write(sdev, STREAM_REG_CTRL, 0);
}

void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev)
{
    /* 触发 stream_gen 单周期软件复位 */
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

