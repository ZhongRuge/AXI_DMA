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
#define PKT_SIZE   64
#define PKT_WORDS  16

/* 参数只接受正整数，避免把拼错的参数当成正常测试。 */
static int parse_positive(const char *text)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || end == text || *end || value <= 0 || value > INT_MAX)
        return -1;
    return (int)value;
}

int main(int argc, char *argv[])
{
    unsigned char buf[PKT_SIZE];
    int count = 1, seconds = 0, count_set = 0;
    int verify = 0, poll_mode = 0;
    int i, fd;
    unsigned int success_cnt = 0, data_err_cnt = 0, read_err_cnt = 0;
    unsigned int packets = 0, poll_events = 0;
    unsigned int first_err_pkt = 0, first_exp = 0, first_act = 0;
    int first_err_word = -1;
    struct timespec start, now;
    struct pollfd pfd;

    /* 1. 解析参数：按包数或按时长测试，两种限制不混用。 */
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
    if (count <= 0 || seconds < 0 || (seconds && (!poll_mode || count_set)))
        goto usage;

    /* 2. 原模式阻塞打开；poll 模式使用非阻塞 read。 */
    fd = open(DEV_PATH, O_RDONLY | (poll_mode ? O_NONBLOCK : 0));
    if (fd < 0) {
        perror("open " DEV_PATH);
        return 1;
    }
    if (seconds && clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        perror("clock_gettime");
        close(fd);
        return 1;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;

    /* 3. 每次先 read：收到数据就校验，EAGAIN 就去 poll 等待。 */
    while (seconds || packets < (unsigned int)count) {
        ssize_t n;
        int timeout_ms = 1000;
        int pkt_ok = 1;
        int w;

        if (seconds) {
            long long elapsed_ms, remaining_ms;

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
            int ready;

            pfd.revents = 0;
            ready = poll(&pfd, 1, timeout_ms);
            if (ready < 0) {
                perror("poll");
                read_err_cnt++;
                break;
            }
            if (ready == 0) {
                /* 不足一秒的等待到期，表示本轮时长测试结束。 */
                if (seconds && timeout_ms < 1000)
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
            /* 回到循环顶部重新 read；不增加包数，也不重复打开设备。 */
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

        /* 4. 沿用原校验：每包 16 个 word，预期为 0～15。 */
        packets++;
        if (verify) {
            for (w = 0; w < PKT_WORDS; w++) {
                unsigned int exp = w;
                unsigned int act = (unsigned int)buf[w * 4] |
                    ((unsigned int)buf[w * 4 + 1] << 8) |
                    ((unsigned int)buf[w * 4 + 2] << 16) |
                    ((unsigned int)buf[w * 4 + 3] << 24);

                if (act != exp) {
                    pkt_ok = 0;
                    if (first_err_word < 0) {
                        first_err_pkt = packets;
                        first_err_word = w;
                        first_exp = exp;
                        first_act = act;
                    }
                }
            }
        }
        if (pkt_ok)
            success_cnt++;
        else
            data_err_cnt++;
    }

    /* 5. 关闭并汇总。Ctrl+C 使用默认行为：进程退出，内核释放文件引用。 */
    if (close(fd) < 0) {
        perror("close");
        read_err_cnt++;
    }
    printf("success=%u data_error=%u read_error=%u\n",
           success_cnt, data_err_cnt, read_err_cnt);
    /* poll_events 是 poll 返回就绪事件的次数，不是调度器唤醒次数。 */
    if (poll_mode)
        printf("packets=%u poll_events=%u\n", packets, poll_events);
    if (first_err_word >= 0)
        printf("first data error: packet=%u word=%d expected=%u actual=%u\n",
               first_err_pkt, first_err_word, first_exp, first_act);
    return (data_err_cnt || read_err_cnt) ? 1 : 0;

usage:
    fprintf(stderr, "Usage: %s [--poll] [--count N | --seconds N] [--verify]\n",
            argv[0]);
    return 1;
}
