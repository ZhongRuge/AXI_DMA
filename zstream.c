#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_PATH   "/dev/zynq_stream0"
#define PKT_SIZE   64          /* 一包字节数，对应内核 STREAM_RX_BUF_SIZE */
#define PKT_WORDS  16          /* 一包含 16 个 u32 */

int main(int argc, char *argv[])
{
    unsigned char buf[PKT_SIZE];
    int count = 1;
    int verify = 0;
    int i;
    int fd;

    unsigned int success_cnt = 0;   /* 读取成功且（未 verify 或数据正确）的包数 */
    unsigned int data_err_cnt = 0;  /* 数据校验失败的包数 */
    unsigned int read_err_cnt = 0;  /* read 系统调用失败的包数 */
    int first_err_pkt = -1;         /* 首个数据错误包号（从 1 起），-1=无 */
    int first_err_word = -1;        /* 首个错误的 word 下标 */
    unsigned int first_exp = 0;     /* 首个错误的期望值 */
    unsigned int first_act = 0;     /* 首个错误的实际值 */

    /* 解析 --count N */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
        }
        if (strcmp(argv[i], "--verify") == 0) {
            verify = 1;
        }
    }

    if (count <= 0) {
        fprintf(stderr, "invalid count: %d\n", count);
        return 1;
    }

    /* 打开设备 */
    fd = open(DEV_PATH, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", DEV_PATH, strerror(errno));
        return 1;
    }

    /* 循环读取 count 包 */
    for (i = 0; i < count; i++) {
        ssize_t n;
        int w;
        int pkt_ok;

    retry:
        n = read(fd, buf, PKT_SIZE);

        if (n == -1 && errno == EINTR)
            goto retry;                 /* 被信号打断：重试，不算错误 */

        if (n != PKT_SIZE) {
            if (n < 0)
                fprintf(stderr, "packet %d: read failed: %s\n", i + 1,
                        strerror(errno));
            else
                fprintf(stderr, "packet %d: short read %zd bytes\n", i + 1, n);
            read_err_cnt++;
            continue;
        }

        /* 读取成功；开启 --verify 时校验 16 个 word 是否为 0..15 */
        pkt_ok = 1;
        if (verify) {
            for (w = 0; w < PKT_WORDS; w++) {
                unsigned int exp = w;
                unsigned int act = buf[w * 4] | (buf[w * 4 + 1] << 8) |
                                   (buf[w * 4 + 2] << 16) |
                                   (buf[w * 4 + 3] << 24);
                if (act != exp) {
                    pkt_ok = 0;
                    if (first_err_pkt < 0) {
                        first_err_pkt = i + 1;
                        first_err_word = w;
                        first_exp = exp;
                        first_act = act;
                    }
                }
            }
        }

        if (verify && !pkt_ok)
            data_err_cnt++;
        else
            success_cnt++;
    }

    close(fd);

    printf("success=%u data_error=%u read_error=%u\n",
           success_cnt, data_err_cnt, read_err_cnt);
    if (first_err_pkt >= 0) {
        printf("first data error: packet=%d word=%d expected=%u actual=%u\n",
               first_err_pkt, first_err_word, first_exp, first_act);
    }

    return (data_err_cnt || read_err_cnt) ? 1 : 0;
}
