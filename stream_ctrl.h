#ifndef _STREAM_CTRL_H_
#define _STREAM_CTRL_H_

#include <linux/types.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>

#define STREAM_CTRL_DRV_NAME       "stream_ctrl"

#define STREAM_CTRL_COMPATIBLE     "zrg,zynq-stream-axidma"

#define STREAM_REG_CTRL               0x00  /* Control register */
#define STREAM_REG_STATUS             0x04  /* Status register */
#define STREAM_REG_PACKET_LEN         0x08  /* Packet length */
#define STREAM_REG_RATE_DIV           0x0c  /* Rate divider */
#define STREAM_REG_WORD_COUNT         0x10  /* Total words transferred */
#define STREAM_REG_PACKET_COUNT       0x14  /* Total packets transferred */
#define STREAM_REG_BACKPRESSURE_COUNT 0x18  /* Backpressure count */
#define STREAM_REG_VERSION            0x1c  /* IP version */

#define STREAM_CTRL_ENABLE    BIT(0)  /* Enable data stream */
#define STREAM_CTRL_RESET     BIT(1)  /* Reset IP core */

#define STREAM_STATUS_RUNNING        BIT(0)  /* Stream is running */
#define STREAM_STATUS_ERROR          BIT(1)  /* Error occurred */
#define STREAM_STATUS_BACKPRESSURE   BIT(2)  /* Backpressure asserted */

#define STREAM_RX_WORDS       16U
#define STREAM_RX_BUF_SIZE    (STREAM_RX_WORDS * sizeof(u32))

struct dma_chan;

struct stream_ctrl_dev {
    struct device *dev;      /* Linux 设备对象 */
    void __iomem *base;      /* MMIO 虚拟基地址 */
    struct resource *res;    /* 物理地址资源 */

    struct dma_chan *rx_channel;

    void *rx_buf;            /* CPU 访问 coherent buffer 使用的虚拟地址 */
    dma_addr_t rx_dma_addr;  /* AXI DMA 访问该 buffer 使用的 DMA 地址 */
    size_t rx_buf_size;      /* buffer 长度，单位：字节 */
    struct completion rx_completion;
};

void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev);
void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev);
void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev);
u32 stream_ctrl_hw_get_status(struct stream_ctrl_dev *sdev);

#endif