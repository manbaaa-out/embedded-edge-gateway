#include "gateway/io/serial/SerialPort.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace gateway {

namespace {
// termios 的 c_?flag 为无符号 tcflag_t,而 ICANON / CSIZE 等宏是 int:
// ~ICANON 得到负 int,直接 &= 到无符号字段构成符号转换。
// 在此集中处理取反与转换,避免下方散落 static_cast。
constexpr tcflag_t clearMask(unsigned int bits) noexcept {
    return static_cast<tcflag_t>(~bits);
}
}  // namespace

SerialPort::SerialPort(const char* path, speed_t baud, bool nonblock) {
    int flags = O_RDWR | O_NOCTTY;
    if (nonblock) flags |= O_NONBLOCK;
    fd_ = open(path, flags);
    if (fd_ == -1) {
        int saved = errno;
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
            fprintf(stderr, "warning: close(%d) failed: %s\n",
                    fd_, strerror(errno));
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
    size_t total = 0;
    const uint8_t* p = data;
    while (total < len) {
        ssize_t n = ::write(fd_, p + total, len - total);   // ::write 限定全局作用域,避免递归
        if (n > 0) {
            total += static_cast<size_t>(n);   // 短写:累加后继续写剩余部分(n > 0)
        }
        else if (n < 0 && errno == EINTR) {
            continue;                       // 被信号打断,重试
        }
        else if (n < 0 && errno == EAGAIN) {
            break;                          // 缓冲满,返回已写字节数(部分写)
        }
        else {
            return -1;
        }
    }
    return (ssize_t)total;                  // total == len 为全写完,否则为 EAGAIN 下的部分写
}

void SerialPort::configure(speed_t baud) {
    struct termios tio;
    if (tcgetattr(fd_, &tio) == -1) {
        int saved = errno;
        throw std::runtime_error(std::string("tcgetattr failed: ") + strerror(saved));
    }

    tio.c_cflag &= clearMask(CSIZE);
    tio.c_cflag |= CS8;
    tio.c_cflag &= clearMask(CSTOPB);    // 1 停止位
    tio.c_cflag &= clearMask(PARENB);    // 无校验
    tio.c_cflag |= CREAD | CLOCAL;       // 开接收 + 忽略 modem
    tio.c_cflag &= clearMask(CRTSCTS);   // 关硬件流控

    tio.c_iflag &= clearMask(IGNBRK | BRKINT | PARMRK | ISTRIP
                           | INLCR | IGNCR | ICRNL
                           | IXON | IXOFF | IXANY);

    tio.c_oflag &= clearMask(OPOST);

    tio.c_lflag &= clearMask(ICANON | ECHO | ECHOE | ECHOK | ECHONL
                           | ISIG | IEXTEN);

    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, baud) == -1 || cfsetospeed(&tio, baud) == -1) {
        int saved = errno;
        throw std::runtime_error(std::string("cfset?speed failed: ") + strerror(saved));
    }

    if (tcsetattr(fd_, TCSANOW, &tio) == -1) {
        int saved = errno;
        throw std::runtime_error(std::string("tcsetattr failed: ") + strerror(saved));
    }
}

} // namespace gateway
