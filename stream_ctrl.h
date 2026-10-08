#ifndef _STREAM_CTRL_H_
#define _STREAM_CTRL_H_

#include <linux/bitops.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "stream_ctrl_uapi.h"

#define STREAM_CTRL_DRV_NAME       "stream_ctrl"
#define STREAM_CTRL_COMPATIBLE     "zrg,zynq-stream-axidma"

/* 发生器寄存器偏移，相对于 sdev->base。 */
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

/* 每包 16 个 32 位数，使用四个缓冲区轮换接收。 */
#define STREAM_RX_WORDS        16U
#define STREAM_RX_BUF_SIZE     (STREAM_RX_WORDS * sizeof(u32))
#define STREAM_RX_BUF_COUNT    4U

#define STREAM_RATE_DIV_DEFAULT 1000U
#define STREAM_RATE_DIV_MAX     1000000U

struct dma_chan;
struct stream_ctrl_dev;

enum stream_rx_state {
    STREAM_RX_IDLE,          /* 暂无接收事务，仍可能有 READY 数据。 */
    STREAM_RX_IN_FLIGHT,     /* 当前有一笔接收任务。 */
    STREAM_RX_CANCELLED,     /* 已取消接收，禁止继续提交。 */
    STREAM_RX_FAULT,         /* 接收发生故障。 */
};

/* 单个缓冲区：FREE -> IN_FLIGHT -> READY -> USER_READING -> FREE。 */
enum stream_buffer_state {
    STREAM_BUF_FREE,         /* 可用于下一包接收。 */
    STREAM_BUF_IN_FLIGHT,    /* 已被本次 DMA 接收占用。 */
    STREAM_BUF_READY,        /* 接收完成，等待读取。 */
    STREAM_BUF_USER_READING, /* 正在交付给用户。 */
};

/* 一块 DMA 缓冲区的地址、状态和所属设备。 */
struct stream_rx_buffer {
    void *cpu_addr;          /* CPU 使用的虚拟地址。 */
    dma_addr_t dma_addr;     /* DMA 硬件使用的地址。 */
    size_t size;

    enum stream_buffer_state state;
    dma_cookie_t cookie;     /* 本次 DMA 事务编号。 */

    struct stream_ctrl_dev *sdev; /* 指向所属设备。 */
};

/* 一台设备的资源，以及各执行路径共用的接收状态。 */
struct stream_ctrl_dev {
    struct device *dev;      /* Linux 设备对象 */
    void __iomem *base;      /* MMIO 虚拟基地址 */
    struct resource *res;    /* 物理地址资源 */

    struct dma_chan *rx_channel;

    struct cdev cdev;        /* 字符设备及其文件操作。 */
    struct device *dev_node; /* device_create() 返回的设备对象。 */

    struct mutex io_lock;   /* 串行化 open、read、release。 */

    wait_queue_head_t rx_wait; /* read 和 poll 共用的通知队列。 */
    enum stream_rx_state rx_state;
    spinlock_t state_lock;   /* 保护接收状态、缓冲区、索引、配置和统计。 */
    struct work_struct rx_work; /* 由工作线程执行的接收任务。 */

    unsigned int open_count; /* 由 io_lock 保护的打开计数。 */

    /* 本次打开周期的统计，由 state_lock 保护。 */
    struct stream_rx_stats stats;
    bool rx_paused_full;     /* 防止同一次满缓冲区暂停被重复计数。 */

    /* 下一次领取的位置，成功领取时前进。 */
    unsigned int rx_write_index;
    unsigned int rx_read_index;

    struct stream_rx_buffer rx_buffers[STREAM_RX_BUF_COUNT];

    u32 rate_div; /* 后续接收使用的发送间隔，由 state_lock 保护。 */
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
