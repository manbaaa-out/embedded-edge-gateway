#include "gateway/link/NodeLink.h"

#include "gateway/core/log/Logger.h"

#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gateway {

namespace {

// 配置里存人类可读的 115200,termios 要 Bxxxx 宏。转换隔离在链路层,
// 不让配置层掺串口实现细节。配置校验已挡掉 <= 0,default 给安全兜底。
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
    // 先析构旧 port(关掉旧 fd)再开新的。解析器状态一并重置:
    // 换设备后残留的半截帧毫无意义,留着只会让第一帧被当成噪声丢掉。
    port_.reset();
    port_ = std::make_unique<SerialPort>(path.c_str(), toBaud(baud), /*nonblock=*/true);
    LOG_INFO("node link reopened: %s @ %d", path.c_str(), baud);
}

void NodeLink::drainAndParse() {
    uint8_t buf[256];
    while (true) {
        const ssize_t n = ::read(port_->get(), buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)  continue;   // 被信号打断,重试
            if (errno == EAGAIN) break;      // 读空,ET 下的正常退出条件
            LOG_ERROR("serial read error: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            LOG_WARN("%s", "serial EOF (peer closed?)");   // 对端关闭(socat 那头)
            break;
        }
        // 批量读、逐字节喂 FSM
        parser_.feed(buf, static_cast<std::size_t>(n));
    }
}

bool NodeLink::send(uint8_t type, const std::vector<uint8_t>& payload) {
    const auto frame = buildFrame(type, payload);
    if (frame.empty()) {
        LOG_ERROR("buildFrame rejected type=0x%02X payload=%zu bytes", type, payload.size());
        return false;
    }

    const ssize_t w = port_->write(frame.data(), frame.size());
    if (w != static_cast<ssize_t>(frame.size())) {
        LOG_WARN("serial write incomplete: type=0x%02X ret=%zd/%zu", type, w, frame.size());
        return false;
    }
    return true;
}

}  // namespace gateway
