#pragma once
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
