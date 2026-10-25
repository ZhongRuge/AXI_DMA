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
#define STREAM_REG_PACKET_LEN         0x08  /* 每包 32 位数的个数 */
#define STREAM_REG_RATE_DIV           0x0c  /* 相邻数据的额外等待周期数 */
#define STREAM_REG_WORD_COUNT         0x10  /* 已握手的 32 位数总数 */
#define STREAM_REG_PACKET_COUNT       0x14  /* 已传输数据包总数 */
#define STREAM_REG_BACKPRESSURE_COUNT 0x18  /* 背压计数 */
#define STREAM_REG_VERSION            0x1c  /* IP 版本 */

#define STREAM_CTRL_VERSION           0x00010001U

#define STREAM_CTRL_ENABLE    BIT(0)  /* 使能数据流 */
#define STREAM_CTRL_RESET     BIT(1)  /* IP 软件复位命令 */

#define STREAM_STATUS_RUNNING        BIT(0)  /* 数据流运行中 */
#define STREAM_STATUS_ERROR          BIT(1)  /* 保留错误位，当前 RTL 固定为 0 */
#define STREAM_STATUS_BACKPRESSURE   BIT(2)  /* 当前存在背压 */

/* 四个缓冲区按最大包长分配，实际传输长度可以更小。 */
#define STREAM_RX_BUF_SIZE     (STREAM_PKT_WORDS_MAX * sizeof(u32))
#define STREAM_RX_BUF_COUNT    4U

#define STREAM_RATE_DIV_DEFAULT  1000U
#define STREAM_RATE_DIV_MAX      1000000U
#define STREAM_READ_TIMEOUT_MS   1000U
#define STREAM_STOP_TIMEOUT_MS   1000U

struct dma_chan;
struct stream_ctrl_dev;

/* 设备是否允许继续接收；与下面每块缓冲区的状态分开看。 */
enum stream_rx_state {
    STREAM_RX_IDLE,          /* 暂无接收事务，仍可能有 READY 数据。 */
    STREAM_RX_IN_FLIGHT,     /* 当前有一包正在接收，可同时预备下一包。 */
    STREAM_RX_CANCELLED,     /* 已取消接收，禁止继续提交。 */
    STREAM_RX_FAULT,         /* 接收发生故障。 */
    STREAM_RX_STOPPING,      /* 禁止新预备，等待已经领取的最多两包结束。 */
};

/* FREE -> PREPARING -> PREPARED -> IN_FLIGHT -> READY -> USER_READING -> FREE。 */
enum stream_buffer_state {
    STREAM_BUF_FREE,         /* 可用于下一包接收。 */
    STREAM_BUF_PREPARING,    /* 已预留，工作线程正在填充、准备和提交。 */
    STREAM_BUF_PREPARED,     /* 已提交到 DMA；数据源尚未为本包启动。 */
    STREAM_BUF_IN_FLIGHT,    /* 数据源已启动，正在接收本包。 */
    STREAM_BUF_READY,        /* 接收完成，等待读取。 */
    STREAM_BUF_USER_READING, /* 正在交付给用户。 */
};

/* 一块 DMA 缓冲区的地址、状态和所属设备。 */
struct stream_rx_buffer {
    void *cpu_addr;          /* CPU 使用的虚拟地址。 */
    dma_addr_t dma_addr;     /* DMA 硬件使用的地址。 */
    size_t size;             /* 分配容量，释放时使用。 */
    size_t data_len;         /* 本包请求的接收字节数，领取 FREE 时确定。 */

    enum stream_buffer_state state;
    dma_cookie_t cookie;     /* 本次 DMA 事务编号。 */
    u64 rx_id;               /* 关联本包诊断事件，不承担回调同步。 */
    u64 dma_start_ns;        /* 本包启动时间，0 表示未开始计时。 */
    u64 ready_ns;            /* 本包发布 READY 的时间。 */

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

    /* 同时需要两把锁时，先 io_lock 后 state_lock；等待时须释放 state_lock。 */
    struct mutex io_lock;    /* 串行化读、启停等用户操作，可跨越睡眠。 */

    wait_queue_head_t rx_wait; /* read、poll 和正常 STOP 共用的通知队列。 */
    enum stream_rx_state rx_state;
    spinlock_t state_lock;   /* 保护与回调共享的状态，持锁期间不能睡眠。 */
    struct work_struct rx_work; /* 准备下一包；可能睡眠的工作只在这里执行。 */
    struct stream_rx_buffer *rx_active; /* 正在接收的一包，由 state_lock 保护。 */
    struct stream_rx_buffer *rx_next;   /* 正在预备或已经提交的下一包。 */

    unsigned int open_count; /* 仅统计收发/控制句柄，由 io_lock 保护。 */

    /* 本次打开周期的统计，由 state_lock 保护。 */
    struct stream_rx_stats stats;
    bool rx_paused_full;     /* 防止同一次满缓冲区暂停被重复计数。 */

    /* 可选的分段计时，与异步起止时间一同由 state_lock 保护。 */
    struct stream_profile_stats profile_stats;
    u64 last_ready_ns;       /* 上包 READY 到下一包启动，不计提前预备的时间。 */
    u64 full_pause_ns;       /* 本次满缓冲区暂停的起点。 */

    /* 下一次领取的位置，成功领取时前进。 */
    unsigned int rx_write_index;
    unsigned int rx_read_index;

    struct stream_rx_buffer rx_buffers[STREAM_RX_BUF_COUNT];

    /* 固定容量的事件历史，由 state_lock 保护，关闭设备不清空。 */
    struct stream_trace_log trace;
    unsigned int trace_head; /* 下一条事件的写入位置。 */
    u64 trace_seq;           /* 事件编号，清空历史后仍递增。 */
    u64 rx_seq;              /* 接收编号，缓冲区轮换后仍递增。 */

    /* 后续接收使用的配置，由 state_lock 保护。 */
    u32 rate_div;            /* 相邻数据之间的额外等待周期数。 */
    u32 packet_words;        /* 每包 32 位数的个数。 */

    /* 已写入 FPGA 的配置，由 state_lock 保护；软件 RESET 保留这些寄存器。 */
    bool hw_config_valid;   /* 初始为 false，确保第一包写入两项配置。 */
    u32 hw_packet_words;
    u32 hw_rate_div;
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
