/** @file EventLoop 的资源管理、epoll 派发和延迟回收实现。 */

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
            int saved = errno;  // 在构造错误文本前保留系统调用错误码。
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

    // 所有权容器随 epoll 一起移动；shared_ptr 管理的 channel 地址保持不变。
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
        while (running_.load(std::memory_order_relaxed)) {
            struct epoll_event events[1024];  // 单次系统调用接收的就绪事件批次。
            int n = epoll_wait(epoll_fd_, events, 1024, -1);  // 本批有效元素数量。
            if (n == -1) {
                int saved = errno;  // 日志/异常构造前保存 errno。
                if (saved == EINTR) continue;
                throw std::runtime_error(std::string("epoll_wait") + " failed: " + strerror(saved));
            }

            for (int i = 0; i < n; i++){  // i 是当前批次中的事件索引。
                channel* ch = static_cast<channel*>(events[i].data.ptr);  // epoll 借用指针。
                uint32_t revents = events[i].events;  // 本次实际发生的事件掩码。

                // 同批较早的回调可能已经移除了该 channel。
                if (ch->dead) continue;

                if (revents & EPOLLIN) {
                    ch->handleRead();
                }
                // 读回调可能移除自身，因此写回调前再次检查状态。
                if ((revents & EPOLLOUT) && !ch->dead) {
                    ch->handleWrite();
                }

            }
            dying_.clear();
        }

    }

    void EventLoop::addChannel(std::shared_ptr<channel> ch) {
        struct epoll_event ev;  // 交给 EPOLL_CTL_ADD 的注册描述。
        ev.events = ch->events;
        ev.data.ptr = ch.get();
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ch->fd, &ev) == -1) {
            int saved = errno;  // 保留 ctl 失败原因。
            throw std::runtime_error(std::string("epoll_ctl") + " ADD failed: " + strerror(saved));
        }

        channels_[ch->fd] = ch;
    }

    void EventLoop::removeChannel(int fd) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL) == -1) {
            LOG_WARN("epoll_ctl DEL(fd=%d) failed: %s", fd, strerror(errno));
        }

        auto it = channels_.find(fd);  // 活动表中对应的所有权条目。
        if (it != channels_.end()) {
            it->second->dead = true;
            dying_.push_back(it->second);
            channels_.erase(it);
        }
    }

    void EventLoop::modifyChannel(channel* ch) {
        struct epoll_event ev;  // 以 channel 最新掩码覆盖内核注册项。
        ev.events = ch->events;
        ev.data.ptr = ch;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, ch->fd, &ev) == -1) {
            int saved = errno;  // 异常构造前保留系统调用错误。
            throw std::runtime_error(std::string("epoll_ctl MOD failed: ") + strerror(saved));
        }
    }

    void EventLoop::forEachChannel(std::function<void(channel*)> fn) {
        for (auto &kv: channels_) {  // kv=(fd, 活动 channel 所有权)。
            fn(kv.second.get());
        }
    }
}
