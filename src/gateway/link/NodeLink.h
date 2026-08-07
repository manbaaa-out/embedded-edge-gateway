#pragma once

// 与 STM32 节点之间的串口链路:串口 fd、收帧 FSM 与组帧发送收拢为一个对象,
// 使组帧与短写处理只有一份实现。对应节点侧的 App/uart_link。
//
// 单一写者:send() 只允许主线程调用。跨线程的命令须先经队列 + eventfd 投递到
// 主线程再统一发出,SerialPort 内部因此无需加锁。该前提由调用方保证。

#include "gateway/io/serial/SerialPort.h"
#include "gateway/protocol/FrameCodec.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gateway {

class NodeLink {
public:
    using FrameHandler = FrameParser::OnFrameCallback;

    // 以非阻塞模式打开串口(epoll 的前提)。打开失败抛异常。
    NodeLink(const std::string& path, int baud);

    NodeLink(const NodeLink&)            = delete;
    NodeLink& operator=(const NodeLink&) = delete;

    // 收齐一帧且 CRC 通过时回调。调用方负责按 TYPE 分流(遥测 / 应答)。
    void setFrameHandler(FrameHandler cb) { parser_.setOnFrame(std::move(cb)); }

    int fd() const noexcept { return port_->get(); }

    // 循环读至 EAGAIN 并逐字节喂给 FSM。ET 模式下必须读空,
    // 否则残留字节不会再次触发事件。
    void drainAndParse();

    // 组帧并整帧写出,payload 需已含 seq。
    // 返回 false 表示未能完整写出(短写或出错),调用方据此决定是否登记在途表。
    bool send(uint8_t type, const std::vector<uint8_t>& payload);

    // 更换串口设备或波特率。前置条件:调用方须先把旧 fd 从 epoll 摘除,
    // 否则 epoll 中会残留一个已关闭的 fd。
    void reopen(const std::string& path, int baud);

    // 解析统计。协议层只计数,呈现由本层负责
    const edge_parser_stats_t& parserStats() const noexcept { return parser_.stats(); }

private:
    std::unique_ptr<SerialPort> port_;
    FrameParser                 parser_;   // 不可移动,本类因此也不可拷贝
};

}  // namespace gateway
