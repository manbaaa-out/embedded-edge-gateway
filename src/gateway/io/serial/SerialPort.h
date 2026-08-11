#pragma once

// 串口 fd 的 RAII 封装:构造即 open + termios 配置,析构即 close。
//
// 只做「一个 fd 的生命周期」这一件事,不含帧、不含协议 —— 组帧与收帧在 NodeLink。
//
// 所有权是这个类唯一的要害。fd 是可复用的小整数而不是指针:悬空的指针大概率当场崩,
// 悬空的 fd 却会静默指向别人的资源。所以它禁拷贝、移动时把源置 -1;挂进 epoll 的
// channel 也必须置 owns_fd = false —— 两个 RAII 对象管同一个 fd,RAII 就成了
// 双重释放的加速器。

#include <termios.h>
#include <cstdint>
#include <sys/types.h>

namespace gateway {

class SerialPort {
public:
    explicit SerialPort(const char* path, speed_t baud, bool nonblock = false);
    ~SerialPort() noexcept;

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;

    int get() const noexcept;
    ssize_t write(const uint8_t* data, size_t len) noexcept;

private:
    void configure(speed_t baud);

    int fd_ = -1;
};

} // namespace gateway
