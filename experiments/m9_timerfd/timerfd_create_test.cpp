#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cinttypes>

int main() {
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) { perror("timerfd_create"); return 1; }

    struct itimerspec value;
    value.it_value.tv_sec = 2;     // 首次 2 秒后到期
    value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = 1;  // 之后每 1 秒一次
    value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, 0, &value, NULL) == -1) { perror("settime"); return 1; }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;           // LT，定时器不需要 ET
    ev.data.fd = timerfd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timerfd, &ev);

    int round = 0;
    while (1) {
        struct epoll_event events;
        int n = epoll_wait(epoll_fd, &events, 1, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }
        if (events.data.fd == timerfd) {
            uint64_t expirations = 0;
            ssize_t r = read(timerfd, &expirations, sizeof(expirations));
            if (r != sizeof(expirations)) {
                if (errno == EAGAIN) continue;
                perror("read"); break;
            }
            round++;
            printf("第 %d 轮, 本次 expirations=%" PRIu64 "\n", round, expirations);
        }
    }

    close(timerfd);
    close(epoll_fd);
}