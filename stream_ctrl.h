#ifndef _STREAM_CTRL_H_
#define _STREAM_CTRL_H_

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/mutex.h>

#define STREAM_CTRL_DRV_NAME       "stream_ctrl"

#define STREAM_CTRL_COMPATIBLE     "zrg,zynq-stream-axidma"

#define STREAM_REG_CTRL               0x00  /* 控制寄存器 */
#define STREAM_REG_STATUS             0x04  /* 状态寄存器 */
#define STREAM_REG_PACKET_LEN         0x08  /* 包长度 */
#define STREAM_REG_RATE_DIV           0x0c  /* 速率分频 */
#define STREAM_REG_WORD_COUNT         0x10  /* 已传输数据总数 */
#define STREAM_REG_PACKET_COUNT       0x14  /* 已传输数据包总数 */
#define STREAM_REG_BACKPRESSURE_COUNT 0x18  /* 背压计数 */
#define STREAM_REG_VERSION            0x1c  /* IP 版本 */

#define STREAM_CTRL_ENABLE    BIT(0)  /* 使能数据流 */
#define STREAM_CTRL_RESET     BIT(1)  /* IP 软件复位命令 */

#define STREAM_STATUS_RUNNING        BIT(0)  /* 数据流运行中 */
#define STREAM_STATUS_ERROR          BIT(1)  /* 发生错误 */
#define STREAM_STATUS_BACKPRESSURE   BIT(2)  /* 当前存在背压 */

#define STREAM_RX_WORDS       16U
#define STREAM_RX_BUF_SIZE    (STREAM_RX_WORDS * sizeof(u32))

struct dma_chan;

struct stream_ctrl_dev {
    struct device *dev;      /* Linux 设备对象 */
    void __iomem *base;      /* MMIO 虚拟基地址 */
    struct resource *res;    /* 物理地址资源 */

    struct dma_chan *rx_channel;

    void *rx_buf;            /* CPU 访问 buffer 的虚拟地址 */
    dma_addr_t rx_dma_addr;  /* DMA 访问 buffer 的地址 */
    size_t rx_buf_size;      /* buffer 长度，单位：字节 */
    struct completion rx_completion; /* DMA 完成通知 */

    struct cdev cdev;         /* 字符设备对象（5.1b 由 cdev_init/cdev_add 初始化） */
    struct device *dev_node;  /* device_create() 返回的设备节点指针 */

    struct mutex io_lock;
};

void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev);
void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev);
void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev);
u32 stream_ctrl_hw_get_status(struct stream_ctrl_dev *sdev);

#endif
