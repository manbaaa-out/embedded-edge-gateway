#include "SerialPort.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace gateway {

SerialPort::SerialPort(const char* path, speed_t baud) {
    fd_ = open(path, O_RDWR | O_NOCTTY);
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

void SerialPort::configure(speed_t baud) {
    struct termios tio;
    if (tcgetattr(fd_, &tio) == -1) {
        int saved = errno;
        throw std::runtime_error(std::string("tcgetattr failed: ") + strerror(saved));
    }

    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~CSTOPB;        // 1 停止位
    tio.c_cflag &= ~PARENB;        // 无校验
    tio.c_cflag |= CREAD | CLOCAL; // 开接收 + 忽略 modem
    tio.c_cflag &= ~CRTSCTS;       // 关硬件流控

    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                   | INLCR | IGNCR | ICRNL
                   | IXON | IXOFF | IXANY);

    tio.c_oflag &= ~OPOST;

    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL
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
