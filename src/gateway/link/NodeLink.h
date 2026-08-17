#pragma once

// STM32 串口链路抽象。
//
// NodeLink 把一个非阻塞 SerialPort、共享帧编码器和增量解析器组合成单一链路对象，
// 上层只处理完整 Frame。它不拥有事件循环，也不加锁；fd 注册、读、写和 reopen 必须
// 由同一 Reactor 线程串行协调。接收采用边沿触发时，drainAndParse() 必须读到 EAGAIN。

#include "gateway/io/serial/SerialPort.h"
#include "gateway/protocol/FrameCodec.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gateway {

/** 拥有串口 fd 和接收解析状态的单线程链路对象。 */
class NodeLink {
public:
    using FrameHandler = FrameParser::OnFrameCallback; /**< CRC 正确帧的同步处理函数。 */

    /**
     * @brief 以非阻塞模式打开并配置 STM32 串口。
     * @param path 串口设备路径。
     * @param baud 人类可读的 bit/s 数值；不支持的值会告警并使用 115200。
     * @throws std::runtime_error 打开或 termios 配置失败。
     * @throws std::bad_alloc SerialPort 对象分配失败。
     */
    NodeLink(const std::string& path, int baud);

    /** 复制源对象不会被读取；串口 fd 与解析状态均为独占资源。 */
    NodeLink(const NodeLink&)            = delete;
    /** 复制源对象不会被读取；禁止多个对象管理同一串口。 */
    NodeLink& operator=(const NodeLink&) = delete;

    /** @param cb CRC 正确后的新回调；空回调表示只解析和统计。 */
    void setFrameHandler(FrameHandler cb) { parser_.setOnFrame(std::move(cb)); }

    /**
     * @return 当前 SerialPort 拥有的 fd 借用值。
     * @warning reopen() 成功或对象析构后，先前取得的 fd 不再有效。
     */
    int fd() const noexcept { return port_->get(); }

    /**
     * @brief 循环读取当前 fd，直到 EAGAIN、EOF 或不可恢复错误。
     *
     * 每批字节按序交给 parser_；EINTR 会重试。帧回调在本函数栈内同步执行。
     * 临时 Frame 分配或业务回调产生的异常会向调用方传播。
     */
    void drainAndParse();

    /**
     * @brief 编码并尝试完整写出一帧。
     * @param type 线上 TYPE 原始值。
     * @param payload 完整业务 payload；命令 seq 由上层提前放入。
     * @return 全帧交给内核时为 true；编码失败、写错误或有界短写耗尽时为 false。
     *
     * false 且已有字节写出时，线上可能残留半帧；调用方不应把该命令登记为正常在途。
     * 构造临时帧的内存分配失败会向调用方传播。
     */
    bool send(uint8_t type, const std::vector<uint8_t>& payload);

    /**
     * @brief 强异常保证地更换串口设备或波特率。
     * @param path 新设备路径。
     * @param baud 新 bit/s 数值；不支持的值退回 115200。
     * @throws std::runtime_error 新串口打开或配置失败；此时旧 port_ 保持不变。
     * @throws std::bad_alloc 新 SerialPort 对象分配失败；旧 port_ 同样保持不变。
     *
     * 调用方须先从事件循环注销旧 fd，随后无论成功或异常都登记 fd() 的实际值。
     * parser_ 不会重置：若切换发生在半帧中，新设备开头字节会先被旧 FSM 状态消费。
     */
    void reopen(const std::string& path, int baud);

    /** @return parser_ 自构造以来的累计统计引用；生命周期与本对象相同。 */
    const edge_parser_stats_t& parserStats() const noexcept { return parser_.stats(); }

private:
    std::unique_ptr<SerialPort> port_; /**< 独占当前串口 fd；替换或析构时关闭。 */
    FrameParser parser_;               /**< 跨 read 保存半帧状态和累计解析统计。 */
};

}  // namespace gateway
