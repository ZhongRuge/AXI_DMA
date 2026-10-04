#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* 按包数或时长接收数据，按需校验并打印统计。 */
int main(int argc, char *argv[])
{
    unsigned char buf[PKT_SIZE];
    struct timespec start, now;
    struct pollfd pfd;
    int count = 1;
    int seconds = 0;
    int count_set = 0;
    int verify = 0;
    int poll_mode = 0;
    int open_flags = O_RDONLY;
    int fd;
    int i;

    unsigned int success_cnt = 0;
    unsigned int data_err_cnt = 0;
    unsigned int read_err_cnt = 0;
    unsigned int packets = 0;
    unsigned int poll_events = 0;

    unsigned int first_err_packet = 0;
    unsigned int first_expected = 0;
    unsigned int first_actual = 0;
    int first_err_word = -1;

    /* 解析运行参数。 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = parse_positive(argv[++i]);
            count_set = 1;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = parse_positive(argv[++i]);
        } else if (strcmp(argv[i], "--verify") == 0) {
            verify = 1;
        } else if (strcmp(argv[i], "--poll") == 0) {
            poll_mode = 1;
        } else {
            goto usage;
        }
    }

    if (count <= 0 || seconds < 0)
        goto usage;
    if (seconds > 0 && (!poll_mode || count_set))
        goto usage;

    /* poll 模式使用非阻塞 read，由 poll 负责等待通知。 */
    if (poll_mode)
        open_flags |= O_NONBLOCK;

    fd = open(DEV_PATH, open_flags);
    if (fd < 0) {
        perror("open " DEV_PATH);
        return 1;
    }

    if (seconds > 0 && clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        perror("clock_gettime");
        close(fd);
        return 1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;

    /* 先 read，暂不可读时再 poll，取得整包后校验。 */
    while (seconds > 0 || packets < (unsigned int)count) {
        ssize_t n;
        int timeout_ms = 1000;
        int packet_ok = 1;
        int word_index;

        if (seconds > 0) {
            long long elapsed_ms;
            long long remaining_ms;

            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
                perror("clock_gettime");
                read_err_cnt++;
                break;
            }

            elapsed_ms = (long long)(now.tv_sec - start.tv_sec) * 1000 +
                         (now.tv_nsec - start.tv_nsec) / 1000000;
            remaining_ms = (long long)seconds * 1000 - elapsed_ms;
            if (remaining_ms <= 0)
                break;
            if (remaining_ms < timeout_ms)
                timeout_ms = (int)remaining_ms;
        }

        n = read(fd, buf, PKT_SIZE);
        if (n < 0 && errno == EAGAIN && poll_mode) {
            int poll_ret;

            pfd.revents = 0;
            poll_ret = poll(&pfd, 1, timeout_ms);
            if (poll_ret < 0) {
                perror("poll");
                read_err_cnt++;
                break;
            }
            if (poll_ret == 0) {
                /* 时长测试的最后一次短等待结束，正常退出。 */
                if (seconds > 0 && timeout_ms < 1000)
                    break;

                fprintf(stderr, "poll timeout: no data for 1 second\n");
                read_err_cnt++;
                break;
            }

            poll_events++;
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

        /* 每包独立校验，预期的 16 个数为 0～15。 */
        packets++;
        if (verify) {
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
                    packet_ok = 0;
                    if (first_err_word < 0) {
                        first_err_packet = packets;
                        first_err_word = word_index;
                        first_expected = expected;
                        first_actual = actual;
                    }
                }
            }
        }

        if (packet_ok)
            success_cnt++;
        else
            data_err_cnt++;
    }

    /* 关闭设备结束本轮接收，再输出用户态统计。 */
    if (close(fd) < 0) {
        perror("close");
        read_err_cnt++;
    }

    printf("success=%u data_error=%u read_error=%u\n",
           success_cnt, data_err_cnt, read_err_cnt);
    if (poll_mode)
        printf("packets=%u poll_events=%u\n", packets, poll_events);
    if (first_err_word >= 0)
        printf("first data error: packet=%u word=%d expected=%u actual=%u\n",
               first_err_packet, first_err_word, first_expected, first_actual);

    if (data_err_cnt || read_err_cnt)
        return 1;
    return 0;

usage:
    fprintf(stderr, "Usage: %s [--poll] [--count N | --seconds N] [--verify]\n",
            argv[0]);
    return 1;
}
