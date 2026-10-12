#ifndef STREAM_CTRL_UAPI_H
#define STREAM_CTRL_UAPI_H

/* 驱动和 zstream 共用：包长单位、统计结构、日志格式、ioctl 命令。 */
#include <linux/ioctl.h>
#include <linux/types.h>

/* 包长单位为 32 位数，范围由当前接收缓冲区容量限定。 */
#define STREAM_PKT_WORDS_DEFAULT 16U
#define STREAM_PKT_WORDS_MAX     16U

/* 驱动与用户程序共用的接收统计。 */
struct stream_rx_stats {
    __u64 rx_packets;        /* 发布为 READY 的包数。 */
    __u64 rx_bytes;          /* READY 包按请求长度累计的字节数，非 DMA 实测长度。 */
    __u64 read_packets;      /* 完整复制给用户的包数。 */
    __u64 discarded;         /* 已完成但未交付的数据包。 */
    __u64 full_pauses;       /* 缓冲区满导致的暂停次数。 */
    __u64 errors;            /* 提交、状态检查、复制和停止错误。 */
    __u64 timeouts;          /* 阻塞 read 或正常 STOP 等待超时。 */
};

/* 诊断记录按事件计数；一包数据会产生多条事件。 */
#define STREAM_TRACE_CAPACITY 128U

/* 事件编号供用户程序解码，新增类型追加到末尾。 */
enum stream_trace_event {
    STREAM_TRACE_START = 1,
    STREAM_TRACE_RX_BEGIN,
    STREAM_TRACE_CONFIG,
    STREAM_TRACE_PREP_FAILED,
    STREAM_TRACE_SUBMIT,
    STREAM_TRACE_ISSUE_REQUEST,
    STREAM_TRACE_START_CMD,
    STREAM_TRACE_CALLBACK_READY,
    STREAM_TRACE_CALLBACK_IGNORED,
    STREAM_TRACE_READ_TAKE,
    STREAM_TRACE_READ_DONE,
    STREAM_TRACE_DMA_CHECK_FAILED,
    STREAM_TRACE_FULL_PAUSE,
    STREAM_TRACE_FULL_RESUME,
    STREAM_TRACE_WAIT_TIMEOUT,
    STREAM_TRACE_WAIT_INTERRUPTED,
    STREAM_TRACE_STOP_BEGIN,
    STREAM_TRACE_STOP_END,
    STREAM_TRACE_RESET_CMD,
    STREAM_TRACE_USER_POLL_TIMEOUT,
    STREAM_TRACE_USER_IO_ERROR,
    STREAM_TRACE_USER_DATA_ERROR,
    STREAM_TRACE_STOP_WAIT,
    STREAM_TRACE_STOP_TIMEOUT,
};

/* 只保存数值副本，不保存指针；rx_id=0、buffer_index=-1 表示全局事件。 */
struct stream_trace_entry {
    __u64 seq;
    __u64 time_ns;          /* 单调时间，纳秒。 */
    __u64 rx_id;            /* 软件接收编号，提交前也有效。 */
    __s32 cookie;           /* 0 表示尚未成功提交或不对应单笔事务。 */
    __s32 buffer_index;
    __u32 event;
    __s32 result;           /* 负数为异常，记录后冻结历史。 */
    __u32 data_len;         /* 本包请求的字节数；是否成功由 event/result 判断。 */
    /* 间隔、DMA 状态、等待毫秒数或忽略回调时的设备状态，按 event 解释。 */
    __u32 detail;
};

/* GET_TRACE 按从旧到新的顺序返回，读取不会清空或解冻。 */
struct stream_trace_log {
    __u64 overwritten;
    __u32 count;
    __u32 frozen;
    struct stream_trace_entry entries[STREAM_TRACE_CAPACITY];
};

/* 用户态只报告自身观察到的异常，不冒充驱动或硬件结论。 */
struct stream_trace_error {
    __u32 event;
    __s32 error;            /* 负 errno。 */
};

/* O_RDONLY 仅支持 GET_STATS/GET_TRACE；接收及修改操作须用 O_RDWR 打开。 */
#define STREAM_IOC_GET_STATS \
    _IOR('Z', 1, struct stream_rx_stats)
#define STREAM_IOC_CLR_STATS _IO('Z', 2)
#define STREAM_IOC_SET_RATE _IOW('Z', 3, __u32)
#define STREAM_IOC_START _IO('Z', 4)
#define STREAM_IOC_STOP  _IO('Z', 5)

/* 清理接收并复位发生器，保持停止，保留配置和软件统计。 */
#define STREAM_IOC_RESET _IO('Z', 6)

/* 参数类型为 __u32，表示每包 32 位数的个数；忙时返回 EBUSY。 */
#define STREAM_IOC_SET_PKT_LEN _IOW('Z', 7, __u32)

#define STREAM_IOC_GET_TRACE _IOR('Z', 8, struct stream_trace_log)
/* 仅在没有占用中的缓冲区时清空并重新开启记录。 */
#define STREAM_IOC_CLR_TRACE _IO('Z', 9)
/* 写入用户态异常并冻结记录；停止接收仍由 STOP 完成。 */
#define STREAM_IOC_FREEZE_TRACE _IOW('Z', 10, struct stream_trace_error)
/* 原子检查并清理普通历史；已冻结时返回 EALREADY，不改动记录。 */
#define STREAM_IOC_PREP_TRACE _IO('Z', 11)

#endif
