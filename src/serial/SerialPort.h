#pragma once
#include <termios.h>

namespace gateway {

class SerialPort {
public:
    explicit SerialPort(const char* path, speed_t baud);
    ~SerialPort() noexcept;

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;

    int get() const noexcept;

private:
    void configure(speed_t baud);

    int fd_ = -1;
};

} // namespace gateway
