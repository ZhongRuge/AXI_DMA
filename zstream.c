/* 用户态主线：解析参数 -> open -> 配置/START -> 接收校验 -> STOP -> close。 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "stream_ctrl_uapi.h"

#define DEV_PATH     "/dev/zynq_stream0"
#define PKT_BUF_SIZE (STREAM_PKT_WORDS_MAX * sizeof(__u32))
#define DEFAULT_COUNT   100
#define POLL_TIMEOUT_MS 1000

enum program_command {
    COMMAND_RECEIVE,
    COMMAND_TRACE,
    COMMAND_RESET,
};

/* 命令行只决定这几个参数，接收过程中不再修改。 */
struct program_options {
    enum program_command command;
    int packet_count;
    int packet_words;
    int rate_div;            /* -1 表示沿用驱动当前配置。 */
    int blocking;
};

/* 把事件编号转换成便于阅读的名称。 */
static const char *trace_event_name(unsigned int event)
{
    static const char *const names[] = {
        [STREAM_TRACE_START] = "START",
        [STREAM_TRACE_RX_BEGIN] = "RX_BEGIN",
        [STREAM_TRACE_CONFIG] = "CONFIG",
        [STREAM_TRACE_PREP_FAILED] = "PREP_FAILED",
        [STREAM_TRACE_SUBMIT] = "SUBMIT",
        [STREAM_TRACE_ISSUE_REQUEST] = "ISSUE_REQUEST",
        [STREAM_TRACE_START_CMD] = "START_CMD",
        [STREAM_TRACE_CALLBACK_READY] = "CALLBACK_READY",
        [STREAM_TRACE_CALLBACK_IGNORED] = "CALLBACK_IGNORED",
        [STREAM_TRACE_READ_TAKE] = "READ_TAKE",
        [STREAM_TRACE_READ_DONE] = "READ_DONE",
        [STREAM_TRACE_DMA_CHECK_FAILED] = "DMA_CHECK_FAILED",
        [STREAM_TRACE_FULL_PAUSE] = "FULL_PAUSE",
        [STREAM_TRACE_FULL_RESUME] = "FULL_RESUME",
        [STREAM_TRACE_WAIT_TIMEOUT] = "WAIT_TIMEOUT",
        [STREAM_TRACE_WAIT_INTERRUPTED] = "WAIT_INTERRUPTED",
        [STREAM_TRACE_STOP_BEGIN] = "STOP_BEGIN",
        [STREAM_TRACE_STOP_END] = "STOP_END",
        [STREAM_TRACE_RESET_CMD] = "RESET_CMD",
        [STREAM_TRACE_USER_POLL_TIMEOUT] = "USER_POLL_TIMEOUT",
        [STREAM_TRACE_USER_IO_ERROR] = "USER_IO_ERROR",
        [STREAM_TRACE_USER_DATA_ERROR] = "USER_DATA_ERROR",
        [STREAM_TRACE_STOP_WAIT] = "STOP_WAIT",
        [STREAM_TRACE_STOP_TIMEOUT] = "STOP_TIMEOUT",
    };

    if (event < sizeof(names) / sizeof(names[0]) && names[event])
        return names[event];
    return "UNKNOWN";
}

/* 读取并打印历史副本，不清空、不解冻驱动记录。 */
static int print_trace(int fd)
{
    struct stream_trace_log *trace;
    unsigned int i;

    trace = malloc(sizeof(*trace));
    if (!trace) {
        perror("malloc trace");
        return -1;
    }
    if (ioctl(fd, STREAM_IOC_GET_TRACE, trace) < 0) {
        perror("ioctl GET_TRACE");
        free(trace);
        return -1;
    }
    if (trace->count > STREAM_TRACE_CAPACITY) {
        fprintf(stderr, "invalid trace count\n");
        free(trace);
        return -1;
    }

    printf("trace: count=%u frozen=%u overwritten=%llu\n",
           trace->count, trace->frozen,
           (unsigned long long)trace->overwritten);
    printf("seq time_us rx_id buf cookie event result bytes detail\n");
    for (i = 0; i < trace->count; i++) {
        const struct stream_trace_entry *entry = &trace->entries[i];

        printf("%llu %llu %llu %d %d %s %d %u %u\n",
               (unsigned long long)entry->seq,
               (unsigned long long)(entry->time_ns / 1000),
               (unsigned long long)entry->rx_id,
               entry->buffer_index, entry->cookie,
               trace_event_name(entry->event), entry->result,
               entry->data_len, entry->detail);
    }
    free(trace);
    return 0;
}

/* 一次 ioctl 检查并清理普通历史，冻结记录必须显式 reset 才能清除。 */
static int prepare_trace(int fd)
{
    if (ioctl(fd, STREAM_IOC_PREP_TRACE, 0) < 0) {
        if (errno == EALREADY) {
            fprintf(stderr,
                    "Saved error history is frozen. View it with './zstream trace';\n"
                    "use './zstream reset' to clear it before a new run.\n");
        } else {
            perror("ioctl PREP_TRACE");
        }
        return -1;
    }
    return 0;
}

/* 把用户态发现的异常写入历史并冻结，已有故障记录保持不变。 */
static void freeze_trace(int fd, unsigned int event, int error)
{
    struct stream_trace_error report = { event, error };

    if (ioctl(fd, STREAM_IOC_FREEZE_TRACE, &report) < 0)
        perror("ioctl FREEZE_TRACE");
}

/* 解析非负整数参数，格式或范围错误时返回 -1。 */
static int parse_nonnegative(const char *text)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > INT_MAX)
        return -1;

    return (int)value;
}

/* 校验一包小端数据是否从 0 逐个递增，发现第一处错误就报告。 */
static int verify_packet(const unsigned char *buf, int packet_words,
                         unsigned int packet)
{
    int word_index;

    for (word_index = 0; word_index < packet_words; word_index++) {
        int offset = word_index * 4;
        unsigned int expected = word_index;
        unsigned int actual;

        /* 按小端顺序把四个字节拼成一个 32 位数。 */
        actual = (unsigned int)buf[offset] |
                 ((unsigned int)buf[offset + 1] << 8) |
                 ((unsigned int)buf[offset + 2] << 16) |
                 ((unsigned int)buf[offset + 3] << 24);

        if (actual != expected) {
            fprintf(stderr,
                    "data error: packet=%u word=%d expected=%u actual=%u\n",
                    packet, word_index, expected, actual);
            return -1;
        }
    }

    return 0;
}

/* 解析接收参数或 trace/reset 命令；不访问设备。 */
static int parse_options(int argc, char *argv[], struct program_options *options)
{
    int count_set = 0;
    int i;

    /* trace/reset 单独使用；普通接收只需写包数，其余参数按需指定。 */
    if (argc > 1 && (strcmp(argv[1], "trace") == 0 ||
                     strcmp(argv[1], "reset") == 0)) {
        if (argc != 2)
            return -1;
        if (strcmp(argv[1], "trace") == 0)
            options->command = COMMAND_TRACE;
        else
            options->command = COMMAND_RESET;
    } else {
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--pkt-len") == 0 && i + 1 < argc) {
                options->packet_words = parse_nonnegative(argv[++i]);
            } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
                options->rate_div = parse_nonnegative(argv[++i]);
                if (options->rate_div < 0)
                    return -1;
            } else if (strcmp(argv[i], "--blocking") == 0) {
                options->blocking = 1;
            } else if (argv[i][0] != '-' && !count_set) {
                options->packet_count = parse_nonnegative(argv[i]);
                count_set = 1;
            } else {
                return -1;
            }
        }
    }

    if (options->packet_count <= 0 || options->packet_words <= 0 ||
        options->packet_words > (int)STREAM_PKT_WORDS_MAX)
        return -1;
    return 0;
}

/* 打印保留的命令行用法。 */
static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [count] [--pkt-len N] [--rate N] [--blocking]\n"
            "       %s trace\n"
            "       %s reset\n"
            "  Default: receive %d packets with poll and verify every packet.\n"
            "  --pkt-len N: 32-bit words per packet (1..%u, default %u).\n"
            "  --rate N: FPGA wait cycles (0 means no extra delay).\n"
            "  --blocking: use blocking read instead of poll.\n"
            "  trace: view saved history without starting reception.\n"
            "  reset: reset reception and discard saved history; do not start.\n",
            program, program, program, DEFAULT_COUNT,
            STREAM_PKT_WORDS_MAX, STREAM_PKT_WORDS_DEFAULT);
}

/* 读出一整包；EAGAIN 时用 poll 等通知，再重新 read。 */
static int receive_packet(int fd, unsigned char *buf, size_t size, int blocking)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    ssize_t n;

    for (;;) {
        n = read(fd, buf, size);
        if (n < 0 && errno == EAGAIN && !blocking) {
            int poll_ret;

            pfd.revents = 0;
            poll_ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
            if (poll_ret < 0) {
                int error = errno;

                perror("poll");
                freeze_trace(fd, STREAM_TRACE_USER_IO_ERROR, -error);
                return -1;
            }
            if (poll_ret == 0) {
                freeze_trace(fd, STREAM_TRACE_USER_POLL_TIMEOUT, -ETIMEDOUT);
                fprintf(stderr, "poll timeout: no data for 1 second\n");
                return -1;
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                freeze_trace(fd, STREAM_TRACE_USER_IO_ERROR, -EIO);
                fprintf(stderr, "poll error: revents=0x%x\n",
                        (unsigned int)(unsigned short)pfd.revents);
                return -1;
            }

            /* poll 只报告事件，回到循环顶部由 read 真正取数据。 */
            continue;
        }

        if (n != (ssize_t)size) {
            int error = n < 0 ? errno : EIO;

            if (n < 0)
                perror("read");
            else
                fprintf(stderr, "unexpected read size: %zd bytes (expected %zu)\n",
                        n, size);

            freeze_trace(fd, STREAM_TRACE_USER_IO_ERROR, -error);
            return -1;
        }

        return 0;
    }
}

/* 默认 poll 接收并校验，trace 查看历史，reset 复位并清空历史。 */
int main(int argc, char *argv[])
{
    unsigned char buf[PKT_BUF_SIZE];
    size_t packet_size;
    struct stream_rx_stats stats;
    struct program_options options = {
        .command = COMMAND_RECEIVE,
        .packet_count = DEFAULT_COUNT,
        .packet_words = STREAM_PKT_WORDS_DEFAULT,
        .rate_div = -1,
        .blocking = 0,
    };
    int stats_ret = 0;
    int stop_ret = 0;
    int trace_ret = 0;
    int open_flags;
    int fd;

    unsigned int packets = 0;
    unsigned int data_err_cnt = 0;
    unsigned int read_err_cnt = 0;

    if (parse_options(argc, argv, &options) < 0) {
        print_usage(argv[0]);
        return 1;
    }
    packet_size = (size_t)options.packet_words * sizeof(__u32);

    /* trace 只读打开；收发和 reset 用读写句柄参与设备控制。 */
    open_flags = options.command == COMMAND_TRACE ? O_RDONLY : O_RDWR;
    if (!options.blocking)
        open_flags |= O_NONBLOCK;

    fd = open(DEV_PATH, open_flags);
    if (fd < 0) {
        perror("open " DEV_PATH);
        return 1;
    }

    /* 管理命令只查看或复位，不启动接收。 */
    if (options.command != COMMAND_RECEIVE) {
        int command_ret = 0;

        if (options.command == COMMAND_TRACE) {
            command_ret = print_trace(fd);
        } else if (ioctl(fd, STREAM_IOC_RESET, 0) < 0) {
            perror("ioctl RESET");
            command_ret = -1;
        } else if (ioctl(fd, STREAM_IOC_CLR_TRACE, 0) < 0) {
            perror("ioctl CLR_TRACE");
            command_ret = -1;
        } else {
            printf("reset done; trace cleared and unfrozen\n");
        }
        if (close(fd) < 0) {
            perror("close");
            return 1;
        }
        return command_ret < 0 ? 1 : 0;
    }

    if (prepare_trace(fd) < 0)
        goto startup_error;

    /* 每次明确设置包长，未指定时使用默认值，保证收发双方长度一致。 */
    {
        __u32 words = (__u32)options.packet_words;

        if (ioctl(fd, STREAM_IOC_SET_PKT_LEN, &words) < 0) {
            perror("ioctl SET_PKT_LEN");
            goto startup_error;
        }
    }

    /* 启动接收前设置间隔，允许 rate 为 0。 */
    if (options.rate_div >= 0) {
        __u32 rate_div = (__u32)options.rate_div;

        if (ioctl(fd, STREAM_IOC_SET_RATE, &rate_div) < 0) {
            perror("ioctl SET_RATE");
            goto startup_error;
        }
    }

    /* START 请求驱动安排接收任务，数据是否就绪仍由 read/poll 判断。 */
    if (ioctl(fd, STREAM_IOC_START, 0) < 0) {
        perror("ioctl START");
        goto startup_error;
    }

    /* 这里直接表达接收目标：取一包、计数、校验，重复到指定包数。 */
    while (packets < (unsigned int)options.packet_count) {
        if (receive_packet(fd, buf, packet_size, options.blocking) < 0) {
            read_err_cnt++;
            break;
        }

        /* 收到整包才计数；校验失败时立即结束本轮接收。 */
        packets++;
        if (verify_packet(buf, options.packet_words, packets) < 0) {
            freeze_trace(fd, STREAM_TRACE_USER_DATA_ERROR, -EBADMSG);
            data_err_cnt++;
            break;
        }
    }

    /* 收够包或出错退出循环后，都先请求停止接收。 */
    stop_ret = ioctl(fd, STREAM_IOC_STOP, 0);
    if (stop_ret < 0)
        perror("ioctl STOP");

    /* 停止后仍保留 fd，用它查询包含清理结果的统计。 */
    stats_ret = ioctl(fd, STREAM_IOC_GET_STATS, &stats);
    if (stats_ret < 0) {
        perror("ioctl GET_STATS");
    } else {
        printf("driver: rx_packets=%llu rx_bytes=%llu read_packets=%llu\n",
               (unsigned long long)stats.rx_packets,
               (unsigned long long)stats.rx_bytes,
               (unsigned long long)stats.read_packets);
    }

    /* 出错自动打印历史，正常运行只显示统计。 */
    if (data_err_cnt || read_err_cnt || stop_ret < 0)
        trace_ret = print_trace(fd);

    /* 关闭 fd；若 STOP 已完成清理，驱动不重复终止 DMA。 */
    if (close(fd) < 0) {
        perror("close");
        read_err_cnt++;
    }

    printf("success=%u data_error=%u read_error=%u\n",
           packets - data_err_cnt, data_err_cnt, read_err_cnt);

    if (data_err_cnt || read_err_cnt || stop_ret < 0 ||
        stats_ret < 0 || trace_ret < 0)
        return 1;
    return 0;

startup_error:
    /* 启动前失败也要关闭 fd，最后关闭时由驱动清理资源。 */
    if (close(fd) < 0)
        perror("close");
    return 1;
}
