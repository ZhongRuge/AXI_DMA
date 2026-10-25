/* 用户态主线：解析参数 -> open -> 配置/START -> 接收校验 -> STOP -> close。 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
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
    int verify;              /* 默认校验，可关闭以测量校验开销。 */
};

/* 用户侧的调用耗时，含返回错误/EAGAIN 的调用；profile 开启时采集。 */
struct user_profile {
    int enabled;
    int failed;
    struct stream_timing_sample read_calls;
    struct stream_timing_sample poll_calls;
    struct stream_timing_sample verify_calls;
};

/* 两个单调时间点相减，纳秒部分换算成秒。 */
static double elapsed_seconds(const struct timespec *begin,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - begin->tv_sec) +
           (double)(end->tv_nsec - begin->tv_nsec) / 1000000000.0;
}

/* 取得纳秒时间戳；计时失败只停止采集，不打断接收和清理。 */
static __u64 user_profile_now(struct user_profile *timings)
{
    struct timespec now;

    if (!timings->enabled || timings->failed)
        return 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("clock_gettime PROFILE");
        timings->failed = 1;
        return 0;
    }
    return (__u64)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

/* 累加调用耗时，保留刚刚 read/poll 返回的 errno。 */
static void user_profile_finish(struct user_profile *timings,
                                struct stream_timing_sample *sample,
                                __u64 begin_ns)
{
    int saved_errno = errno;
    __u64 end_ns;
    __u64 elapsed_ns;

    if (!begin_ns)
        return;
    end_ns = user_profile_now(timings);
    if (!timings->failed && end_ns >= begin_ns) {
        elapsed_ns = end_ns - begin_ns;
        sample->count++;
        sample->total_ns += elapsed_ns;
        if (elapsed_ns > sample->max_ns)
            sample->max_ns = elapsed_ns;
    }
    errno = saved_errno;
}

/* 将纳秒汇总换算成总毫秒、平均微秒及最大微秒。 */
static void print_timing_row(const char *name,
                             const struct stream_timing_sample *sample)
{
    double average_us = 0.0;

    if (sample->count)
        average_us = (double)sample->total_ns / sample->count / 1000.0;
    printf("%-22s %8llu %10.3f %10.3f %10.3f\n",
           name, (unsigned long long)sample->count,
           sample->total_ns / 1000000.0, average_us, sample->max_ns / 1000.0);
}

/* STOP 后打印两侧的汇总；各行会重叠，不能相加成接收总时间。 */
static int print_profile(int fd, const struct user_profile *timings)
{
    static const char *const names[STREAM_PROFILE_STAGE_COUNT] = {
        [STREAM_PROFILE_HW_CONFIG] = "drv.hw_config",
        [STREAM_PROFILE_BUFFER_FILL] = "drv.buffer_fill",
        [STREAM_PROFILE_DMA_PREP] = "drv.dma_prep",
        [STREAM_PROFILE_DMA_SUBMIT] = "drv.dma_submit",
        [STREAM_PROFILE_DMA_ISSUE] = "drv.dma_issue",
        [STREAM_PROFILE_START_WRITE] = "drv.start_write",
        [STREAM_PROFILE_START_TO_CALLBACK] = "drv.start_to_callback",
        [STREAM_PROFILE_CALLBACK_BODY] = "drv.callback_body",
        [STREAM_PROFILE_READY_TO_NEXT_RX] = "drv.ready_to_next_start",
        [STREAM_PROFILE_READY_TO_READ] = "drv.ready_to_read",
        [STREAM_PROFILE_DMA_STATUS] = "drv.dma_status",
        [STREAM_PROFILE_COPY_TO_USER] = "drv.copy_to_user",
        [STREAM_PROFILE_TRACE_WRITE] = "drv.trace_write",
        [STREAM_PROFILE_FULL_PAUSE] = "drv.full_pause",
    };
    struct stream_profile_stats stats;
    unsigned int i;

    if (ioctl(fd, STREAM_IOC_GET_PROFILE, &stats) < 0) {
        perror("ioctl GET_PROFILE");
        return -1;
    }

    printf("profile: elapsed time, includes waits/preemption; not CPU time\n");
    printf("Rows overlap; driver samples include packets discarded at STOP.\n");
    printf("Only finished intervals are counted; calls may include failures.\n");
    printf("%-22s %8s %10s %10s %10s\n",
           "stage", "count", "total_ms", "avg_us", "max_us");
    for (i = 0; i < STREAM_PROFILE_STAGE_COUNT; i++)
        print_timing_row(names[i], &stats.stages[i]);

    if (timings->failed) {
        fprintf(stderr, "userspace profile unavailable: clock read failed\n");
        return -1;
    }
    print_timing_row("user.read", &timings->read_calls);
    print_timing_row("user.poll", &timings->poll_calls);
    print_timing_row("user.verify", &timings->verify_calls);
    return 0;
}

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
        if (errno == EOPNOTSUPP) {
            printf("trace: disabled; load stream_ctrl.ko with trace=1 to record events\n");
            free(trace);
            return 0;
        }
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
            } else if (strcmp(argv[i], "--no-verify") == 0) {
                options->verify = 0;
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
            "Usage: %s [count] [--pkt-len N] [--rate N] [--blocking] [--no-verify]\n"
            "       %s trace\n"
            "       %s reset\n"
            "  Default: receive %d packets with poll and verify every packet.\n"
            "  --pkt-len N: 32-bit words per packet (1..%u, default %u).\n"
            "  --rate N: FPGA wait cycles (0 means no extra delay).\n"
            "  --blocking: use blocking read instead of poll.\n"
            "  --no-verify: skip content checks for a throughput comparison.\n"
            "  Stage timings: load stream_ctrl.ko with profile=1.\n"
            "  Event logging: load stream_ctrl.ko with trace=1 (default off).\n"
            "  trace: view saved history without starting reception.\n"
            "  reset: reset reception and discard saved history; do not start.\n",
            program, program, program, DEFAULT_COUNT,
            STREAM_PKT_WORDS_MAX, STREAM_PKT_WORDS_DEFAULT);
}

/* 读出一整包；EAGAIN 时用 poll 等通知，再重新 read。 */
static int receive_packet(int fd, unsigned char *buf, size_t size, int blocking,
                           struct user_profile *timings)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    ssize_t n;
    __u64 begin_ns;

    for (;;) {
        begin_ns = user_profile_now(timings);
        n = read(fd, buf, size);
        user_profile_finish(timings, &timings->read_calls, begin_ns);
        if (n < 0 && errno == EAGAIN && !blocking) {
            int poll_ret;

            pfd.revents = 0;
            begin_ns = user_profile_now(timings);
            poll_ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
            user_profile_finish(timings, &timings->poll_calls, begin_ns);
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
    struct timespec start_time;
    struct timespec receive_end_time;
    struct timespec stop_end_time;
    struct stream_rx_stats stats;
    struct stream_profile_stats driver_profile;
    struct user_profile timings = {0};
    struct program_options options = {
        .command = COMMAND_RECEIVE,
        .packet_count = DEFAULT_COUNT,
        .packet_words = STREAM_PKT_WORDS_DEFAULT,
        .rate_div = -1,
        .blocking = 0,
        .verify = 1,
    };
    int stats_ret = 0;
    int stop_ret = 0;
    int stop_errno;
    int trace_ret = 0;
    int profile_ret = 0;
    int timing_error = 0;
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

    /* 开关由模块决定，用户侧与驱动侧使用同一轮计时口径。 */
    if (ioctl(fd, STREAM_IOC_GET_PROFILE, &driver_profile) < 0) {
        perror("ioctl GET_PROFILE");
        goto startup_error;
    }
    timings.enabled = driver_profile.enabled;

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

    /* T0：计时从 START 前开始，包含启动、接收和校验。 */
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) < 0) {
        perror("clock_gettime START");
        goto startup_error;
    }

    /* START 请求驱动安排接收任务，数据是否就绪仍由 read/poll 判断。 */
    if (ioctl(fd, STREAM_IOC_START, 0) < 0) {
        perror("ioctl START");
        goto startup_error;
    }

    /* 这里直接表达接收目标：取一包、计数、校验，重复到指定包数。 */
    while (packets < (unsigned int)options.packet_count) {
        if (receive_packet(fd, buf, packet_size, options.blocking, &timings) < 0) {
            read_err_cnt++;
            break;
        }

        /* 收到整包才计数；启用校验时，数据错误立即结束本轮接收。 */
        packets++;
        if (options.verify) {
            __u64 begin_ns = user_profile_now(&timings);
            int verify_ret = verify_packet(buf, options.packet_words, packets);

            user_profile_finish(&timings, &timings.verify_calls, begin_ns);
            if (verify_ret < 0) {
                freeze_trace(fd, STREAM_TRACE_USER_DATA_ERROR, -EBADMSG);
                data_err_cnt++;
                break;
            }
        }
    }

    /* T1：接收循环结束；STOP 的收尾时间单独计算。 */
    if (clock_gettime(CLOCK_MONOTONIC, &receive_end_time) < 0) {
        perror("clock_gettime RECEIVE_END");
        timing_error = 1;
    }

    /* 收够包或出错退出循环后，都先请求停止接收。 */
    stop_ret = ioctl(fd, STREAM_IOC_STOP, 0);
    stop_errno = errno;

    /* T2：STOP 返回后立即计时，避免把日志打印算入停止耗时。 */
    if (clock_gettime(CLOCK_MONOTONIC, &stop_end_time) < 0) {
        perror("clock_gettime STOP_END");
        timing_error = 1;
    }
    if (stop_ret < 0) {
        errno = stop_errno;
        perror("ioctl STOP");
    }

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

    if (timings.enabled)
        profile_ret = print_profile(fd, &timings);

    /* 关闭 fd；若 STOP 已完成清理，驱动不重复终止 DMA。 */
    if (close(fd) < 0) {
        perror("close");
        read_err_cnt++;
    }

    /* 关闭校验时，success 只表示成功读取，不能证明数据内容正确。 */
    printf("success=%u data_error=%u read_error=%u verify=%s\n",
           packets - data_err_cnt, data_err_cnt, read_err_cnt,
           options.verify ? "on" : "off");

    if (!timing_error) {
        double receive_seconds = elapsed_seconds(&start_time, &receive_end_time);
        double stop_seconds = elapsed_seconds(&receive_end_time, &stop_end_time);
        unsigned long long payload_bytes =
            (unsigned long long)(packets - data_err_cnt) * packet_size;

        /* 校验关闭时只报告读取量；MB 使用十进制的 1000000 字节。 */
        printf("perf: %s=%llu rx_ms=%.3f stop_ms=%.3f ",
               options.verify ? "valid_bytes" : "read_bytes", payload_bytes,
               receive_seconds * 1000.0, stop_seconds * 1000.0);
        if (receive_seconds > 0.0)
            printf("throughput_MBps=%.3f\n",
                   payload_bytes / receive_seconds / 1000000.0);
        else
            printf("throughput_MBps=unavailable\n");
    }

    if (data_err_cnt || read_err_cnt || stop_ret < 0 ||
        stats_ret < 0 || trace_ret < 0 || timing_error || profile_ret < 0)
        return 1;
    return 0;

startup_error:
    /* 启动前失败也要关闭 fd，最后关闭时由驱动清理资源。 */
    if (close(fd) < 0)
        perror("close");
    return 1;
}
