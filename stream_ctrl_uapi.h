#ifndef STREAM_CTRL_UAPI_H
#define STREAM_CTRL_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* 驱动与用户程序共用的接收统计。 */
struct stream_rx_stats {
    __u64 rx_packets;        /* 发布为 READY 的包数。 */
    __u64 rx_bytes;          /* 发布为 READY 的总字节数。 */
    __u64 read_packets;      /* 完整复制给用户的包数。 */
    __u64 discarded;         /* 已完成但未交付的数据包。 */
    __u64 full_pauses;       /* 缓冲区满导致的暂停次数。 */
    __u64 errors;            /* 提交、状态检查、复制和停止错误。 */
    __u64 timeouts;          /* 阻塞 read 等待超时。 */
};

/* 查询统计，数据从驱动返回用户程序。 */
#define STREAM_IOC_GET_STATS \
    _IOR('Z', 1, struct stream_rx_stats)

#endif
