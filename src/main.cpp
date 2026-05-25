#include "Logger.h"
#include "SerialPort.h"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <unistd.h>
#include <termios.h>

int main() {
    LOG_INFO("gateway %s starting...", "v0.1.0");

    try {
        gateway::SerialPort port("/tmp/ttyV0", B115200);
        LOG_INFO("serial opened, fd = %d", port.get());

        uint8_t buf[10];
        ssize_t n = read(port.get(), buf, sizeof(buf));
        if (n == -1) {
            LOG_ERROR("%s", "read failed");
            return 1;
        }
        LOG_INFO("read %zd bytes", n);

    } catch (const std::exception& e) {
        LOG_ERROR("serial error: %s", e.what());
        return 1;
    }

    return 0;
}
