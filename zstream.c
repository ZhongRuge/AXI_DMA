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

#define DEV_PATH   "/dev/zynq_stream0"
#define PKT_WORDS  16
#define PKT_SIZE   (PKT_WORDS * 4)

/* 解析正整数参数，格式或范围错误时返回 -1。 */
static int parse_positive(const char *text)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value <= 0 || value > INT_MAX)
        return -1;

    return (int)value;
}

/* 校验一包小端数据是否为 0～15，发现第一处错误就报告。 */
static int verify_packet(const unsigned char *buf, unsigned int packet)
{
    int word_index;

    for (word_index = 0; word_index < PKT_WORDS; word_index++) {
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

/* 按包数接收数据，按需校验，结束前查询驱动统计。 */
int main(int argc, char *argv[])
{
    unsigned char buf[PKT_SIZE];
    struct pollfd pfd;
    struct stream_rx_stats stats;
    int stats_ret = 0;
    int count = 1;
    int verify = 0;
    int poll_mode = 0;
    int open_flags = O_RDONLY;
    int fd;
    int i;

    unsigned int packets = 0;
    unsigned int data_err_cnt = 0;
    unsigned int read_err_cnt = 0;

    /* 解析运行参数。 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = parse_positive(argv[++i]);
        } else if (strcmp(argv[i], "--verify") == 0) {
            verify = 1;
        } else if (strcmp(argv[i], "--poll") == 0) {
            poll_mode = 1;
        } else {
            goto usage;
        }
    }

    if (count <= 0)
        goto usage;

    /* poll 模式使用非阻塞 read，由 poll 负责等待通知。 */
    if (poll_mode)
        open_flags |= O_NONBLOCK;

    fd = open(DEV_PATH, open_flags);
    if (fd < 0) {
        perror("open " DEV_PATH);
        return 1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;

    /* 先 read，暂不可读时再 poll，取得整包后校验。 */
    while (packets < (unsigned int)count) {
        ssize_t n;

        n = read(fd, buf, PKT_SIZE);
        if (n < 0 && errno == EAGAIN && poll_mode) {
            int poll_ret;

            pfd.revents = 0;
            poll_ret = poll(&pfd, 1, 1000);
            if (poll_ret < 0) {
                perror("poll");
                read_err_cnt++;
                break;
            }
            if (poll_ret == 0) {
                fprintf(stderr, "poll timeout: no data for 1 second\n");
                read_err_cnt++;
                break;
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "poll error: revents=0x%x\n",
                        (unsigned int)(unsigned short)pfd.revents);
                read_err_cnt++;
                break;
            }

            /* poll 只报告事件，回到循环顶部由 read 真正取数据。 */
            continue;
        }

        if (n != PKT_SIZE) {
            if (n < 0)
                perror("read");
            else
                fprintf(stderr, "short read: %zd bytes\n", n);

            read_err_cnt++;
            break;
        }

        /* 收到整包才计数；校验失败时立即结束本轮接收。 */
        packets++;
        if (verify && verify_packet(buf, packets) < 0) {
            data_err_cnt++;
            break;
        }
    }

    /* 使用本轮仍然打开的 fd 查询统计。 */
    stats_ret = ioctl(fd, STREAM_IOC_GET_STATS, &stats);
    if (stats_ret < 0) {
        perror("ioctl GET_STATS");
    } else {
        printf("driver: rx_packets=%llu read_packets=%llu\n",
               (unsigned long long)stats.rx_packets,
               (unsigned long long)stats.read_packets);
    }

    /* 关闭设备结束本轮接收，再输出用户态统计。 */
    if (close(fd) < 0) {
        perror("close");
        read_err_cnt++;
    }

    printf("success=%u data_error=%u read_error=%u\n",
           packets - data_err_cnt, data_err_cnt, read_err_cnt);

    if (data_err_cnt || read_err_cnt || stats_ret < 0)
        return 1;
    return 0;

usage:
    fprintf(stderr, "Usage: %s [--poll] [--count N] [--verify]\n",
            argv[0]);
    return 1;
}
