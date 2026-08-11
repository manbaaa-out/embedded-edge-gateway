#include "gateway/link/NodeLink.h"

#include "gateway/core/log/Logger.h"

#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gateway {

namespace {

// 配置中存人类可读的 115200,termios 需要 Bxxxx 宏。转换隔离在链路层,
// 使配置层不掺入串口实现细节。配置校验已排除 <= 0,default 分支为兜底。
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
    // 先把新口开出来,成功了才替换 —— 强异常保证。
    //
    // 反过来写(先 port_.reset() 再 make_unique)看着更省一个 fd,但代价是:新路径
    // 打不开时抛出的那一刻 port_ 已经是空的,而 fd() 就是 port_->get() —— 对空
    // unique_ptr 解引用。于是调用方即便接住了异常也无路可走:旧链路已经没了,
    // 新链路没建起来,对象处于既不能用也不能修的中间态。
    // 现在失败时 port_ 原封不动,旧链路继续收数据,调用方 catch 住就能接着跑。
    //
    // 只有 baud 变、path 没变时,这里会短暂同时持有同一个 tty 的两个 fd。Linux 上
    // 普通 tty 不带 O_EXCL,允许如此;且 termios 是每 tty 而非每 fd 的,新 fd 上的
    // configure 同时作用于旧 fd —— 而旧 fd 下一行就被丢弃了,无影响。
    auto fresh = std::make_unique<SerialPort>(path.c_str(), toBaud(baud), /*nonblock=*/true);
    port_ = std::move(fresh);

    // 注意 parser_ 的 FSM 状态【没有】被重置(此处曾有一句注释声称重置了,那是把
    // port_.reset() 看成了重置解析器 —— 它重置的是 unique_ptr)。后果有限:换设备时
    // FSM 若停在 READ_PAYLOAD,新链路的头几个字节会被当作旧半截帧的尾巴吃掉,
    // 但 FSM 的"所有异常路径都回到 WAIT_HDR0"这条不变量保证它在 1-2 帧内自愈。
    // 要真正修掉得给 protocol/ 加一个"只重置 FSM、不清统计"的接口 ——
    // edge_parser_init 会把 frames_ok / crc_err 一并清零,而那是监控要用的累计值。
    LOG_INFO("node link reopened: %s @ %d", path.c_str(), baud);
}

void NodeLink::drainAndParse() {
    uint8_t buf[256];
    while (true) {
        const ssize_t n = ::read(port_->get(), buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)  continue;   // 被信号打断,重试
            if (errno == EAGAIN) break;      // 已读空,ET 下的正常退出条件
            LOG_ERROR("serial read error: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            LOG_WARN("%s", "serial EOF (peer closed?)");
            break;
        }
        // 批量读入,逐字节驱动 FSM
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
