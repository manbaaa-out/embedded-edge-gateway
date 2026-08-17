// 非阻塞串口链路实现。
//
// 读侧一次事件必须排空内核缓冲区；写侧允许 SerialPort::write() 因 EAGAIN 返回短写，
// 但只用有限次数的 poll(POLLOUT) 续写，避免主 Reactor 被慢设备无限占用。

#include "gateway/link/NodeLink.h"

#include "gateway/core/log/Logger.h"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gateway {

namespace {

/** 初次 write 之外允许的续写次数。 */
constexpr int kWriteRetry = 3;
/** 每次等待串口可写的最长时间，单位毫秒。 */
constexpr int kWriteWaitMs = 20;

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

NodeLink::NodeLink(const std::string& path, int baud) {
    port_ = std::make_unique<SerialPort>(path.c_str(), toBaud(baud), /*nonblock=*/true);
    LOG_INFO("node link opened: %s @ %d", path.c_str(), baud);
}

void NodeLink::reopen(const std::string& path, int baud) {
    // 先构造新资源，保证打开或配置失败时仍保留旧串口。
    auto fresh = std::make_unique<SerialPort>( // 成功前仅由本作用域拥有的新候选串口。
        path.c_str(), toBaud(baud), /*nonblock=*/true);
    port_ = std::move(fresh);

    // parser_ 保留累计统计和当前 FSM 状态；切换发生在半帧中时，新链路的开头字节
    // 会先被旧状态消费，直到长度或 CRC 检查使解析器重新同步。
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

bool NodeLink::send(uint8_t type, const std::vector<uint8_t>& payload) {
    const auto frame = buildFrame(type, payload); // 拥有本次发送完整线上字节的临时帧。
    if (frame.empty()) {
        LOG_ERROR("buildFrame rejected type=0x%02X payload=%zu bytes", type, payload.size());
        return false;
    }

    // SerialPort::write 在 EAGAIN 时返回已写长度；等待 POLLOUT 后从断点继续。
    std::size_t sent = 0; // 已由成功返回的 SerialPort::write() 确认写出的帧前缀长度。
    for (int attempt = 0;; ++attempt) { // attempt=0 为首次写，之后最多 kWriteRetry 次续写。
        const ssize_t w = port_->write(frame.data() + sent, frame.size() - sent);
        // w 是本轮确认写出的剩余前缀长度；负值表示 SerialPort 遇到不可恢复错误。
        if (w < 0) {
            LOG_ERROR("serial write failed: type=0x%02X after %zu/%zu bytes: %s",
                      type, sent, frame.size(), strerror(errno));
            return false;
        }
        sent += static_cast<std::size_t>(w);
        if (sent == frame.size()) return true;

        if (attempt >= kWriteRetry) break;
        struct pollfd pfd { port_->get(), POLLOUT, 0 }; // 本轮等待当前串口恢复可写。
        if (::poll(&pfd, 1, kWriteWaitMs) <= 0) break;
    }

    // sent > 0 时链路上已留下不完整帧；调用方收到 false 后不会登记该命令。
    LOG_ERROR("serial write incomplete: type=0x%02X %zu/%zu bytes — a partial frame is on the wire",
              type, sent, frame.size());
    return false;
}

}  // namespace gateway
