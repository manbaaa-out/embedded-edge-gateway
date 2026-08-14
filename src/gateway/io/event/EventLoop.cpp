// 主循环与 channel 的增删。
//
// 全文的要害在 removeChannel 与 loop 的配合:回调函数本身就存在 channel 里,
// 在回调中当场析构自己所属的 channel,等于销毁正在执行的 std::function ——
// 在自己站着的树枝上锯。所以摘除只置标志并转入 dying_,真正的回收推迟到批末,
// 那时没有任何回调在栈上。退出判定同理放在批末,不半途 break。

#include "gateway/io/event/EventLoop.h"
#include "gateway/core/log/Logger.h"
#include <sys/socket.h>
#include <cerrno>
#include <unistd.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <stdexcept>
#include <cerrno>
#include <cstring>

namespace gateway{

    EventLoop::EventLoop(){
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1) {
            int saved = errno;
            throw std::runtime_error(std::string("epoll_create1") + " failed: " + strerror(saved));
        }
    }

    EventLoop::~EventLoop() noexcept{
        if (epoll_fd_ != -1){
            if (close(epoll_fd_) == -1) {
                LOG_WARN("close(epoll_fd=%d) failed: %s", epoll_fd_, strerror(errno));
            }
        }
    }

    // channels_ 与 dying_ 必须跟着一起搬。
    //
    // 只搬 epoll_fd_ 的话:目标对象管着 epoll,却看不到任何 channel —— removeChannel
    // 找不到条目、forEachChannel 遍历为空;而源对象析构时会把那些 channel 连同 fd
    // 一起关掉,等于把目标脚下的 fd 抽走。实测过:移动后源仍持有 1 条,目标是 0 条。
    //
    // 搬容器本身是安全的:channel 由 shared_ptr 持有,对象地址不变,内核里那些
    // data.ptr 仍然指向同一批对象。
    EventLoop::EventLoop(EventLoop&& other) noexcept{
        epoll_fd_ = other.epoll_fd_;
        other.epoll_fd_ = -1;
        channels_ = std::move(other.channels_);
        dying_    = std::move(other.dying_);
        other.channels_.clear();
        other.dying_.clear();
    }

    EventLoop& EventLoop::operator=(EventLoop&& other) noexcept{
        if (this != &other) {
            if (epoll_fd_ != -1) {
                close(epoll_fd_);
                epoll_fd_ = -1;
            }
            epoll_fd_ = other.epoll_fd_;
            other.epoll_fd_ = -1;
            channels_ = std::move(other.channels_);
            dying_    = std::move(other.dying_);
            other.channels_.clear();
            other.dying_.clear();
        }
        return *this;
    }

    void EventLoop::loop() {
        // 退出判定放在批末:本轮已派发的回调要跑完,dying_ 也要回收干净,
        // 之后才返回 —— 半途 break 会让回调栈上的指针失去 dying_ 的保护。
        while (running_.load(std::memory_order_relaxed)) {
            struct epoll_event events[1024];
            int n = epoll_wait(epoll_fd_, events, 1024, -1);
            if (n == -1) {
                int saved = errno;
                if (saved == EINTR) continue;   // 被信号打断,重试
                throw std::runtime_error(std::string("epoll_wait") + " failed: " + strerror(saved));
            }

            for (int i = 0; i < n; i++){
                channel* ch = static_cast<channel*>(events[i].data.ptr);
                uint32_t revents = events[i].events;

                // 同批内可能已被别的回调摘除(见 channel::dead)。指针仍有效,
                // 但这条 channel 已不该再被驱动。
                if (ch->dead) continue;

                if (revents & EPOLLIN) {
                    ch->handleRead();
                }
                // 必须复查 dead:handleRead 可能摘除了【自己】(HTTP 的 on_read 在
                // recv 返回 0 或解析出错时就是这么做的)。此时 fd 已被 EPOLL_CTL_DEL,
                // 而同一批事件里 EPOLLIN 与 EPOLLOUT 可以同时就绪 —— 继续跑
                // handleWrite,一旦它把 out_buf 发空就会去调 modifyChannel,
                // EPOLL_CTL_MOD 在已摘除的 fd 上返回 ENOENT,而那个函数是抛异常的,
                // 异常一路穿出 loop() 就是整条 HTTP 线程结束。
                if ((revents & EPOLLOUT) && !ch->dead) {
                    ch->handleWrite();
                }

            }
            dying_.clear();
        }

    }

    void EventLoop::addChannel(std::shared_ptr<channel> ch) {
        struct epoll_event ev;
        ev.events = ch->events;
        ev.data.ptr = ch.get();
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ch->fd, &ev) == -1) {
            int saved = errno;
            throw std::runtime_error(std::string("epoll_ctl") + " ADD failed: " + strerror(saved));
        }

        channels_[ch->fd] = ch;
    }

    void EventLoop::removeChannel(int fd) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL) == -1) {
            LOG_WARN("epoll_ctl DEL(fd=%d) failed: %s", fd, strerror(errno));
        }

        auto it = channels_.find(fd);
        if (it != channels_.end()) {
            it->second->dead = true;        // 同批后续事件不再驱动它
            dying_.push_back(it->second);   // 延迟回收,撑到批末
            channels_.erase(it);
        }
    }

    void EventLoop::modifyChannel(channel* ch) {
        struct epoll_event ev;
        ev.events = ch->events;
        ev.data.ptr = ch;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, ch->fd, &ev) == -1) {
            int saved = errno;
            throw std::runtime_error(std::string("epoll_ctl MOD failed: ") + strerror(saved));
        }
    }

    void EventLoop::forEachChannel(std::function<void(channel*)> fn) {
        for (auto &kv: channels_) {
            fn(kv.second.get());
        }
    }
}