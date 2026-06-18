#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/err.h>
#include "stream_ctrl.h"

/* 从 MMIO 基地址读取 4 个寄存器并打印 */
static void stream_ctrl_dump_regs(struct stream_ctrl_dev *sdev)
{
    u32 ctrl;
    u32 status;
    u32 frame_len;
    u32 version;

    ctrl = readl(sdev->base + STREAM_REG_CTRL);
    status = readl(sdev->base + STREAM_REG_STATUS);
    frame_len = readl(sdev->base + STREAM_REG_FRAME_LEN);
    version = readl(sdev->base + STREAM_REG_VERSION);

    dev_info(sdev->dev, "stream_ctrl: CTRL      = 0x%08x\n", ctrl);
    dev_info(sdev->dev, "stream_ctrl: STATUS    = 0x%08x\n", status);
    dev_info(sdev->dev, "stream_ctrl: FRAME_LEN = 0x%08x\n", frame_len);
    dev_info(sdev->dev, "stream_ctrl: VERSION   = 0x%08x\n", version);
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
    dev_info(sdev->dev, "stream_ctrl: resource = %pR\n", res);

    sdev->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(sdev->base)) {
        dev_err(&pdev->dev, "ioremap error!\n");
        return PTR_ERR(sdev->base);
    }

    /* 绑定pdev和sdev 便于后续访问 */
    platform_set_drvdata(pdev, sdev);
    dev_info(&pdev->dev, "ioremap success!\n");

    stream_ctrl_dump_regs(sdev);

    return 0;
}