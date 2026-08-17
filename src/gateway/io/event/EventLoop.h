#pragma once

/**
 * @file
 * 基于 epoll 的单线程 Reactor 与事件源抽象。
 *
 * EventLoop 串行派发所有 fd 回调，使上层共享状态无需在回调之间加锁。回调可以移除
 * 自身或同批事件源：removeChannel() 先标记失效，再由 dying_ 延迟持有到本批事件
 * 结束，从而同时保证裸指针存活和已删除回调不再执行。
 */

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

/** 一个受 epoll 管理的文件描述符及其回调、输出状态和生命周期策略。 */
struct channel {
    int fd = -1;                    ///< 文件描述符；-1 表示尚未绑定资源。
    uint32_t events = 0;            ///< 注册给 epoll 的 EPOLL* 事件掩码。
    std::function<void()> on_read;  ///< EPOLLIN 就绪时调用；可为空。
    std::function<void()> on_write; ///< EPOLLOUT 就绪时调用；可为空。
    std::string out_buf;            ///< 非阻塞发送尚未写完的数据，按字节顺序保存。
    time_t last_active = 0;         ///< 最近一次业务活动的 Unix 秒时间戳。
    bool timeout_exempt = false;    ///< true 表示上层空闲扫描不应回收该事件源。

    bool owns_fd = true;  ///< false 表示仅借用 fd，例如生命周期由 SerialPort 管理。

    bool dead = false;  ///< 已从 epoll 摘除，等待本批事件结束后释放。

    channel() = default;  ///< 创建尚未绑定 fd 的空事件源。

    channel(const channel& /* other */) = delete;  ///< 不从其他事件源复制 fd 所有权。
    channel& operator=(const channel& /* other */) = delete;  ///< 不接管副本来源。

    /** owns_fd 为 true 时关闭 fd；借用资源由其实际所有者关闭。 */
    ~channel() { if (owns_fd && fd != -1) ::close(fd); }

    /** 若已登记读回调则执行。 */
    void handleRead() { if (on_read) on_read(); }
    /** 若已登记写回调则执行。 */
    void handleWrite() { if (on_write) on_write(); }
};

/** 独占一个 epoll 实例并在调用线程中串行派发 channel。 */
class EventLoop {
    public:
    /** @throws std::runtime_error epoll 实例创建失败。 */
    explicit EventLoop();

    EventLoop(const EventLoop& /* other */) = delete;  ///< 不从其他循环复制资源所有权。
    EventLoop& operator=(const EventLoop& /* other */) = delete;  ///< 不接管副本来源。

    /**
     * 转移 epoll 和 channel 所有权。
     * @param other 提供资源的事件循环，调用后不再持有 epoll 与 channel。
     * @pre 源对象未在执行 loop()；退出标志不作为资源状态转移。
     */
    EventLoop(EventLoop&& /* other */) noexcept;
    /**
     * 释放当前资源后接管源对象的 epoll 和 channel。
     * @param other 提供资源的事件循环，调用后不再持有 epoll 与 channel。
     * @return 当前对象。
     * @pre 两个对象均未在执行 loop()；目标对象保留自己的退出标志。
     */
    EventLoop& operator=(EventLoop&& /* other */) noexcept;

    /**
     * 阻塞派发事件，直至 quit() 生效。
     * @throws std::runtime_error epoll_wait 失败；回调抛出的其他异常也会原样传播。
     */
    void loop();

    /** 请求循环退出；只置原子标志，不会主动唤醒阻塞中的 epoll_wait。 */
    void quit() noexcept { running_.store(false, std::memory_order_relaxed); }

    /**
     * 注册并持有事件源。
     * @param ch 已填充 fd、events 和回调的事件源，不得为空。
     * @throws std::runtime_error EPOLL_CTL_ADD 失败。
     */
    void addChannel(std::shared_ptr<channel> /* ch */);
    /**
     * 从 epoll 和活动表移除事件源，并延迟释放到本批派发结束。
     * @param fd 待移除的文件描述符。
     */
    void removeChannel(int /* fd */);
    /**
     * 将 channel 当前的 events 掩码同步到 epoll。
     * @param ch 已注册且仍存活的事件源。
     * @throws std::runtime_error EPOLL_CTL_MOD 失败。
     */
    void modifyChannel(channel* /* ch */);

    /**
     * 依次访问当前活动事件源。
     * @param fn 对每个借用 channel 指针执行的回调；回调期间不得增删活动表。
     */
    void forEachChannel(std::function<void(channel*)> /* fn */);
    
    /** 关闭 epoll；各 channel 随成员容器销毁并按所有权策略关闭 fd。 */
    ~EventLoop() noexcept;

    private:
    int epoll_fd_ = -1;                     ///< 本对象独占的 epoll 文件描述符。
    std::atomic<bool> running_{true};        ///< loop() 的继续运行标志。
    std::map<int, std::shared_ptr<channel>> channels_;  ///< fd 到活动事件源的所有权表。
    std::vector<std::shared_ptr<channel>> dying_;       ///< 延迟到本批结束释放的事件源。

};

}
