#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/dmaengine.h>
#include "stream_ctrl.h"


static u32 stream_ctrl_read(struct stream_ctrl_dev *sdev, u32 reg)
{
    return readl(sdev->base + reg);
}

static void stream_ctrl_write(struct stream_ctrl_dev *sdev, u32 reg, u32 value)
{
    writel(value, sdev->base + reg);
}

/* 从 MMIO 基地址读取 8 个寄存器并打印 */
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

    /* 创建并保存对象指针 */
    sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev) 
        return -ENOMEM;
    
    sdev->dev = &pdev->dev;
    
    /* 获取 绑定 映射 物理资源 */
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

    /* 绑定pdev和sdev 便于后续访问 */
    platform_set_drvdata(pdev, sdev);

    sdev->rx_chan = dma_request_chan(&pdev->dev, "rx");
    if (IS_ERR(sdev->rx_chan)) {
        int ret = PTR_ERR(sdev->rx_chan);

        if (ret != -EPROBE_DEFER)
            dev_err(&pdev->dev,
                    "stream_ctrl: failed to request RX DMA channel: %d\n",
                    ret);

        return ret;
    }

    dev_info(&pdev->dev,
            "stream_ctrl: DMA RX channel acquired successfully\n");

    stream_ctrl_dump_regs(sdev);

    return 0;
}

static int stream_ctrl_remove(struct platform_device *pdev)
{
    struct stream_ctrl_dev *sdev;
    sdev = platform_get_drvdata(pdev);
    dev_info(&pdev->dev, "stream_ctrl: remove called\n");

    if (sdev && sdev->rx_chan) {
        dma_release_channel(sdev->rx_chan);
        sdev->rx_chan = NULL;
    }

    if (sdev)
        dev_info(&pdev->dev, "stream_ctrl: resources will be released by devm\n");

    return 0;
}

void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write(sdev, STREAM_REG_CTRL, STREAM_CTRL_ENABLE);
}

void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev)
{
    stream_ctrl_write(sdev, STREAM_REG_CTRL, 0);
}

void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev)
{
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

