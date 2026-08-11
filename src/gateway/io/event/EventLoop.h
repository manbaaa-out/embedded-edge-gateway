#pragma once

// epoll Reactor:一条线程管住所有 fd。本项目最重要的一条设计,后面很多东西是它的推论。
//
// 这个网关要同时盯住四样事:串口来数据、云端下发命令、超时该重发了、收到 SIGHUP。
// 朴素做法是一样一条线程各自阻塞读,代价是四条线程要共享在途命令表与串口,于是需要锁,
// 有了锁就有死锁与竞态。Reactor 把它反过来:不让线程去等 fd,而让 fd 来通知线程 ——
// 四件事在同一条线程上串行发生,共享状态因此不需要锁。
//
// channel 是核心抽象:把「一个 fd + 它就绪时该干什么」打包成一个对象,主循环因此
// 不必知道 fd 背后是串口还是信号。它身上两个标志各管一半,别混 ——
// dying_ 管内存安全(对象活到批末,回调栈上的指针不悬空),dead 管语义正确
// (这条 channel 已不该再被驱动)。

#include <atomic>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <unistd.h>
#include <string>
#include <ctime>

namespace gateway{

struct channel {
    int fd = -1;          // -1 表示未持有 fd,析构时据此决定是否 close
    uint32_t events = 0;
    std::function<void()> on_read;
    std::function<void()> on_write;
    std::string out_buf;
    time_t last_active = 0;
    bool timeout_exempt = false;

    // fd 的所有权。默认 true:eventfd / timerfd / signalfd / socket 都是由本对象
    // 独占持有的,随它析构而关闭。
    //
    // 置 false 用于 fd 另有主人的场合 —— 目前只有串口:fd 归 SerialPort 这个 RAII
    // 对象所有,channel 只是借来挂进 epoll。若在此也 close,就是同一个 fd 两个所有者,
    // 热加载换串口时会出事:removeChannel 把旧 channel 转入 dying_(批末才析构),
    // 其间 SerialPort 先 close 掉旧 fd 号、再 open 新串口通常又拿到同一个号,
    // 于是批末旧 channel 析构关掉的正是【新串口】的 fd —— epoll 中该项随之失效,
    // 采集静默中断,而日志刚打完 "reload done"。
    bool owns_fd = true;

    // 已被 removeChannel 摘除,正等批末回收。
    //
    // 一批 epoll 事件里,前一个回调可能摘掉后一个 channel(HTTP 的空闲扫描摘连接就是
    // 这样)。此时后者的 data.ptr 仍然有效——dying_ 保着它——于是回调照常被派发,
    // 却作用在一条已经不在 epoll、上下文也已被清掉的连接上。dying_ 保证的是内存安全,
    // 不保证语义正确,这个标志补上后一半。
    bool dead = false;

    channel() = default;

    // fd 是独占资源,禁用拷贝以避免两个 channel 关闭同一个 fd
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // 仅在持有所有权时关闭;否则由真正的持有者负责
    ~channel() { if (owns_fd && fd != -1) ::close(fd); }

    void handleRead() { if (on_read) on_read(); }
    void handleWrite() { if (on_write) on_write(); }
};

class EventLoop {
    public:
    explicit EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    EventLoop(EventLoop&&) noexcept;
    EventLoop& operator=(EventLoop&&) noexcept;

    void loop();

    // 请求主循环退出:本轮事件派发与批末回收结束后 loop() 返回。
    //
    // 约定只在 loop 自己所在的线程里调用 —— 本项目两处都是如此(主 Reactor 在
    // signalfd 回调里调,HTTP 在自己的 1 秒 timerfd 回调里调)。别的线程调它只是
    // 置上标志,epoll_wait 仍阻塞在 -1 超时上,要等下一个事件才会醒来发现。
    // 真要支持跨线程即时唤醒,得再挂一个 eventfd —— 那是另一件事,当前两处都
    // 有自己的周期性事件兜底,不需要。
    void quit() noexcept { running_.store(false, std::memory_order_relaxed); }

    void addChannel(std::shared_ptr<channel>);
    void removeChannel(int);
    void modifyChannel(channel*);

    void forEachChannel(std::function<void(channel*)>);
    
    ~EventLoop() noexcept;

    private:
    int epoll_fd_ = -1;
    std::atomic<bool> running_{true};
    std::map<int, std::shared_ptr<channel>> channels_;
    std::vector<std::shared_ptr<channel>> dying_;

};

}