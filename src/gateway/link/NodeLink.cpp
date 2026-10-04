// 非阻塞串口链路实现。
//
// 读侧一次事件必须排空内核缓冲区；写侧把帧及短写偏移保存在输出队列，遇到 EAGAIN
// 立即返回。上层只在队列非空时监听 EPOLLOUT，整帧写完后再开始等待命令应答。

#include "gateway/link/NodeLink.h"

#include "gateway/core/log/Logger.h"

#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gateway {

namespace {

/**
 * @param baud 配置中的 bit/s 整数值。
 * @return 对应 termios speed_t；未列出的值记录告警并返回 B115200。
 */
speed_t toBaud(int baud) {
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:
        LOG_WARN("unsupported baud %d, falling back to 115200", baud);
        return B115200;
    }
}

}  // namespace

NodeLink::NodeLink(const std::string& path, int baud, bool defer_configuration) : baud_(toBaud(baud)) {
    port_ = std::make_unique<SerialPort>(path.c_str(), baud_, true, defer_configuration);
    LOG_INFO("node link opened: %s @ %d", path.c_str(), baud);
}

void NodeLink::activate() { port_->configure(baud_); }

void NodeLink::reopen(const std::string& path, int baud) {
    // 先构造新资源，保证打开或配置失败时仍保留旧串口。
    auto fresh = std::make_unique<SerialPort>( // 成功前仅由本作用域拥有的新候选串口。
        path.c_str(), toBaud(baud), /*nonblock=*/true);
    port_ = std::move(fresh);
    baud_ = toBaud(baud);

    // 新 fd 是新的字节流边界，旧半帧不能由其字节续接；仅丢弃临时 FSM 进度，累计
    // 统计和业务回调继续保留。该操作在候选端口成功提交后才发生，失败路径不受影响。
    parser_.resetStream();
    // 旧 fd 的半帧不能在新流中续写；通知上层释放其排队状态和序列号占用。
    discardOutput();
    LOG_INFO("node link reopened: %s @ %d", path.c_str(), baud);
}

void NodeLink::drainAndParse() {
    uint8_t buf[256]; // 单次 read 的栈缓冲区；解析器会在需要时复制 payload 字节。
    while (true) {
        const ssize_t n = ::read(port_->get(), buf, sizeof(buf)); // 本轮实际读取字节数。
        if (n < 0) {
            if (errno == EINTR)  continue;
            if (errno == EAGAIN) break;
            LOG_ERROR("serial read error: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            LOG_WARN("%s", "serial EOF (peer closed?)");
            break;
        }
        parser_.feed(buf, static_cast<std::size_t>(n));
    }
}

bool NodeLink::send(uint8_t type, const std::vector<uint8_t>& payload,
                    SendHandler on_complete, SendPredicate can_send) {
    auto frame = buildFrame(type, payload); // 入队后由 PendingWrite 持有完整线上字节。
    if (frame.empty()) {
        LOG_ERROR("buildFrame rejected type=0x%02X payload=%zu bytes", type, payload.size());
        return false;
    }

    output_.push_back(PendingWrite{std::move(frame), 0, std::move(on_complete),
                                  std::move(can_send)});
    return true;
}

void NodeLink::flushOutput() {
    while (!output_.empty()) {
        auto& pending = output_.front(); // 后续帧必须等待本帧写完，不能交错发送。
        if (pending.offset == 0 && pending.can_send && !pending.can_send()) {
            completeFront(false);
            continue;
        }

        // n 是单次非阻塞 write 接受的字节数；逐次保存偏移，避免短写后重复发送前缀。
        const ssize_t n = ::write(port_->get(), pending.frame.data() + pending.offset,
                                  pending.frame.size() - pending.offset);
        if (n > 0) {
            pending.offset += static_cast<std::size_t>(n);
            if (pending.offset == pending.frame.size()) completeFront(true);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;

        const int saved = n < 0 ? errno : EIO; // 零长度进展也按写失败处理，避免空转。
        LOG_ERROR("serial write failed after %zu/%zu bytes: %s",
                  pending.offset, pending.frame.size(), strerror(saved));
        discardOutput();
        return;
    }
}

void NodeLink::completeFront(bool sent) {
    auto on_complete = std::move(output_.front().on_complete); // 移除队首前取得回调所有权。
    output_.pop_front();
    if (on_complete) on_complete(sent);
}

void NodeLink::discardOutput() {
    std::deque<PendingWrite> discarded; // 先摘下旧队列，回调不会访问即将失效的队首引用。
    discarded.swap(output_);
    for (auto& pending : discarded) {
        if (pending.on_complete) pending.on_complete(false);
    }
}

}  // namespace gateway
