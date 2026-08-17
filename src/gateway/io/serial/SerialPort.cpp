/** @file SerialPort 的系统调用和 8N1 原始模式配置实现。 */

#include "gateway/io/serial/SerialPort.h"
#include "gateway/core/log/Logger.h"

#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace gateway {

namespace {
/**
 * 在无符号的 tcflag_t 域内完成掩码取反。
 * @param bits 需要清除的 termios 标志组合。
 * @return 可直接与 tcflag_t 字段进行按位与的保留掩码。
 */
constexpr tcflag_t clearMask(unsigned int bits) noexcept {
    return static_cast<tcflag_t>(~bits);
}
}  // namespace

SerialPort::SerialPort(const char* path, speed_t baud, bool nonblock) {
    int flags = O_RDWR | O_NOCTTY;  // 打开模式：读写且不取得控制终端。
    if (nonblock) flags |= O_NONBLOCK;
    fd_ = open(path, flags);
    if (fd_ == -1) {
        int saved = errno;  // 构造异常文本前保留 open 的错误码。
        throw std::runtime_error(std::string("open '") + path + "' failed: " + strerror(saved));
    }

    try {
        configure(baud);
    } catch (...) {
        close(fd_);
        throw;
    }
}

SerialPort::~SerialPort() noexcept {
    if (fd_ != -1) {
        if (close(fd_) == -1) {
            LOG_WARN("close(serial fd=%d) failed: %s", fd_, strerror(errno));
        }
    }
}

SerialPort::SerialPort(SerialPort&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

int SerialPort::get() const noexcept {
    return fd_;
}

ssize_t SerialPort::write(const uint8_t* data, size_t len) noexcept {
    size_t total = 0;         // 本次调用已经成功写出的字节数。
    const uint8_t* p = data;  // 保持不变的输入缓冲区起点。
    while (total < len) {
        ssize_t n = ::write(fd_, p + total, len - total);  // 本轮系统调用结果。
        if (n > 0) {
            total += static_cast<size_t>(n);
        }
        else if (n < 0 && errno == EINTR) {
            continue;
        }
        else if (n < 0 && errno == EAGAIN) {
            break;
        }
        else {
            return -1;
        }
    }
    // 非阻塞模式遇到 EAGAIN 时返回已经写出的字节数。
    return (ssize_t)total;
}

void SerialPort::configure(speed_t baud) {
    struct termios tio;  // 从设备现有属性出发，逐项改为透明的 8N1 模式。
    if (tcgetattr(fd_, &tio) == -1) {
        int saved = errno;  // 异常构造前保存 tcgetattr 的错误。
        throw std::runtime_error(std::string("tcgetattr failed: ") + strerror(saved));
    }

    tio.c_cflag &= clearMask(CSIZE);
    tio.c_cflag |= CS8;
    tio.c_cflag &= clearMask(CSTOPB);
    tio.c_cflag &= clearMask(PARENB);
    tio.c_cflag |= CREAD | CLOCAL;
    tio.c_cflag &= clearMask(CRTSCTS);

    tio.c_iflag &= clearMask(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL
                           | IXON | IXOFF | IXANY);

    tio.c_oflag &= clearMask(OPOST);

    tio.c_lflag &= clearMask(ICANON | ECHO | ECHOE | ECHOK | ECHONL
                           | ISIG | IEXTEN);

    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, baud) == -1 || cfsetospeed(&tio, baud) == -1) {
        int saved = errno;  // 两个速度设置共享同一失败处理。
        throw std::runtime_error(std::string("cfset?speed failed: ") + strerror(saved));
    }

    if (tcsetattr(fd_, TCSANOW, &tio) == -1) {
        int saved = errno;  // 异常文本构造前保留 tcsetattr 错误。
        throw std::runtime_error(std::string("tcsetattr failed: ") + strerror(saved));
    }
}

}  // namespace gateway
