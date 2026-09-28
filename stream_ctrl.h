#ifndef _STREAM_CTRL_H_
#define _STREAM_CTRL_H_

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/dmaengine.h>
#include <linux/workqueue.h>

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

#define STREAM_CTRL_VERSION           0x00010001U

#define STREAM_CTRL_ENABLE    BIT(0)  /* 使能数据流 */
#define STREAM_CTRL_RESET     BIT(1)  /* IP 软件复位命令 */

#define STREAM_STATUS_RUNNING        BIT(0)  /* 数据流运行中 */
#define STREAM_STATUS_ERROR          BIT(1)  /* 发生错误 */
#define STREAM_STATUS_BACKPRESSURE   BIT(2)  /* 当前存在背压 */

#define STREAM_RX_WORDS       16U
#define STREAM_RX_BUF_SIZE    (STREAM_RX_WORDS * sizeof(u32))
#define STREAM_RX_BUF_COUNT 4U

struct dma_chan;
struct stream_ctrl_dev;

enum stream_rx_state {
    STREAM_RX_IDLE,          /* 暂无接收事务，仍可能有 READY 数据。 */
    STREAM_RX_IN_FLIGHT,
    STREAM_RX_CANCELLED,     /* 禁止继续提交，取消通知保留到最后关闭。 */
    STREAM_RX_FAULT,
};

enum stream_buffer_state {
    STREAM_BUF_FREE,
    STREAM_BUF_IN_FLIGHT,
    STREAM_BUF_READY,
    STREAM_BUF_USER_READING,
};

struct stream_rx_buffer {
    void *cpu_addr;
    dma_addr_t dma_addr;
    size_t size;

    enum stream_buffer_state state;
    dma_cookie_t cookie;

    struct stream_ctrl_dev *sdev;
};

struct stream_rx_stats {
    u64 rx_packets;          /* 发布为 READY 的包数。 */
    u64 rx_bytes;
    u64 read_packets;        /* 完整复制给用户的包数。 */
    u64 discarded;           /* 已完成但未交付的数据包。 */
    u64 full_pauses;         /* 缓冲区满导致的暂停次数。 */
    u64 errors;              /* 提交、状态检查、复制和停止错误。 */
    u64 timeouts;            /* 阻塞 read 等待超时。 */
};

struct stream_ctrl_dev {
    struct device *dev;      /* Linux 设备对象 */
    void __iomem *base;      /* MMIO 虚拟基地址 */
    struct resource *res;    /* 物理地址资源 */

    struct dma_chan *rx_channel;

    struct cdev cdev;         /* 字符设备对象（5.1b 由 cdev_init/cdev_add 初始化） */
    struct device *dev_node;  /* device_create() 返回的设备节点指针 */

    struct mutex io_lock;

    wait_queue_head_t rx_wait;
    enum stream_rx_state rx_state;
    spinlock_t state_lock;
    struct work_struct rx_work;

    unsigned int open_count;

    /* 本次打开周期的统计，由 state_lock 保护。 */
    struct stream_rx_stats stats;
    bool rx_paused_full;

    /* 环形缓冲区的下次提交、读取位置，由 state_lock 保护。 */
    unsigned int rx_write_index;
    unsigned int rx_read_index;

    struct stream_rx_buffer rx_buffers[STREAM_RX_BUF_COUNT];
};

/* 启动发生器发送一包数据。 */
void stream_ctrl_hw_start(struct stream_ctrl_dev *sdev);

/* 请求发生器停止输出。 */
void stream_ctrl_hw_stop(struct stream_ctrl_dev *sdev);

/* 软件复位数据发生器。 */
void stream_ctrl_hw_reset(struct stream_ctrl_dev *sdev);

/* 读取发生器的硬件状态。 */
u32 stream_ctrl_hw_get_status(struct stream_ctrl_dev *sdev);

#endif
