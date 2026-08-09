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

    EventLoop::EventLoop(EventLoop&& other) noexcept{
        epoll_fd_ = other.epoll_fd_;
        other.epoll_fd_ = -1;
    }

    EventLoop& EventLoop::operator=(EventLoop&& other) noexcept{
        if (this != &other) {
            if (epoll_fd_ != -1) {
                close(epoll_fd_);
                epoll_fd_ = -1;
            }
            epoll_fd_ = other.epoll_fd_;
            other.epoll_fd_ = -1;
        }
        return *this;
    }

    void EventLoop::loop() {
        while (1) {
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

                // 同批内可能已被别的回调摘除（见 channel::dead）。指针仍有效，
                // 但这条 channel 已不该再被驱动。
                if (ch->dead) continue;

                if (revents & EPOLLIN) {
                    ch->handleRead();
                }
                if (revents & EPOLLOUT) {
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
            dying_.push_back(it->second);   // 延迟回收，撑到批末
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