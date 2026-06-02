#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cinttypes>
#include "EventLoop.h"

int main() {
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) { perror("timerfd_create"); return 1; }

    struct itimerspec value;
    value.it_value.tv_sec = 2;     // 首次 2 秒后到期
    value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = 1;  // 之后每 1 秒一次
    value.it_interval.tv_nsec = 0;
    if (timerfd_settime(timerfd, 0, &value, NULL) == -1) { perror("settime"); return 1; }

    m7::EventLoop loop;

    std::shared_ptr<m7::channel> ch = std::make_shared<m7::channel>();
    ch->fd = timerfd;
    ch->events = EPOLLIN;
    int round = 0;
    ch->on_read = [timerfd, &round](){
        uint64_t expirations = 0;
        ssize_t r = read(timerfd, &expirations, sizeof(expirations));
        if (r != sizeof(expirations)) {
            perror("read");
        }
        round++;
        printf("第 %d 轮, 本次 expirations=%" PRIu64 "\n", round, expirations);
    };
    loop.addChannel(ch);

    loop.loop();
}