#ifndef _STREAM_CTRL_H_
#define _STREAM_CTRL_H_

#include <linux/types.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/bitops.h>

#define STREAM_CTRL_DRV_NAME       "stream_ctrl"
#define STREAM_CTRL_COMPATIBLE     "xlnx,PWM-2.0"

#define STREAM_REG_CTRL        0x00
#define STREAM_REG_STATUS      0x04
#define STREAM_REG_FRAME_LEN   0x08
#define STREAM_REG_VERSION     0x0c

#define STREAM_CTRL_START      BIT(0)
#define STREAM_CTRL_STOP       BIT(1)
#define STREAM_CTRL_RESET      BIT(2)

#define STREAM_STATUS_BUSY     BIT(0)
#define STREAM_STATUS_ERROR    BIT(1)

struct stream_ctrl_dev {
    struct device *dev; // Linux设备对象的指针 资源管理
    void __iomem *base; // 映射后的虚拟基地址
    struct resource *res; // 保存物理地址资源
};

void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev);
void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev);
void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev);
u32 stream_ctrl_hw_get_status(struct stream_ctrl_dev *sdev);

#endif