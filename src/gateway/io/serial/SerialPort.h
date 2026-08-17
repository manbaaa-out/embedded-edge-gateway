#pragma once

/**
 * @file
 * 串口文件描述符的独占 RAII 封装。
 *
 * 本层只负责设备生命周期、termios 和字节写入，不解析协议帧。fd 可被 EventLoop
 * 借用，但关闭权始终留在 SerialPort，从而避免热加载时重复关闭被复用的 fd 编号。
 */

#include <termios.h>
#include <cstdint>
#include <sys/types.h>

namespace gateway {

/** 独占一个已配置为原始字节通道的串口文件描述符。 */
class SerialPort {
public:
    /**
     * 打开并配置串口。
     * @param path 串口设备路径，调用期间必须有效。
     * @param baud termios 波特率常量，例如 B115200。
     * @param nonblock 是否以 O_NONBLOCK 打开。
     * @throws std::runtime_error 打开或 termios 配置失败。
     */
    explicit SerialPort(const char* path, speed_t baud, bool nonblock = false);
    /** 关闭仍由本对象持有的串口 fd。 */
    ~SerialPort() noexcept;

    SerialPort(const SerialPort& /* other */) = delete;  ///< 不从其他实例复制 fd。
    SerialPort& operator=(const SerialPort& /* other */) = delete;  ///< 不接管副本来源。

    /** @param other 提供 fd 的源对象；调用后被置为无资源状态。 */
    SerialPort(SerialPort&& other) noexcept;
    /**
     * 关闭当前 fd 后接管源对象资源。
     * @param other 提供 fd 的源对象；调用后被置为无资源状态。
     * @return 当前对象。
     */
    SerialPort& operator=(SerialPort&& other) noexcept;

    /** @return 供系统调用或 EventLoop 借用的 fd；所有权不转移。 */
    int get() const noexcept;
    /**
     * 尽量写完一段字节。
     * @param data 待写缓冲区起点，至少包含 len 字节。
     * @param len 请求写入的字节数。
     * @return 已写字节数；非阻塞 EAGAIN 可返回短写，失败返回 -1。
     */
    ssize_t write(const uint8_t* data, size_t len) noexcept;

private:
    /** @param baud 要设置到输入和输出方向的 termios 波特率常量。 */
    void configure(speed_t baud);

    int fd_ = -1;  ///< 本对象独占的串口 fd；-1 表示已移走或尚未取得。
};

}  // namespace gateway
