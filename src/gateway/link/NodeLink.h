#pragma once

// ============================================================
// NodeLink —— 与 STM32 节点之间那条串口链路的唯一化身。
//
// 对标节点侧的 App/uart_link:
//     open  ↔ 构造          read  ↔ drainAndParse
//     write ↔ send          重开  ↔ reopen(热加载)
//
// 把「串口 fd + 收帧 FSM + 组帧发送」收成一个对象的理由:
// 这三件事本来就是一条链路的两个方向,拆开放在 GatewayApp 里,
// 结果是 port_->write 散落在三个不同的回调里(发命令、超时重发),
// 每处都要自己组帧、自己判断短写。
//
// 【单一写者】send() 只允许主线程调用。跨线程的命令要先经队列 + eventfd
// 投递到主线程,由主线程统一发 —— 这样 SerialPort 内部无需加锁。
// 这条前提由调用方保证,本类不做强制。
// ============================================================

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

    // 打开串口(非阻塞,epoll 的前提)。打不开抛异常 —— 没有串口的网关没有意义。
    NodeLink(const std::string& path, int baud);

    NodeLink(const NodeLink&)            = delete;
    NodeLink& operator=(const NodeLink&) = delete;

    // 收齐一帧且 CRC 通过时回调。调用方负责按 TYPE 分流(遥测 / 应答)。
    void setFrameHandler(FrameHandler cb) { parser_.setOnFrame(std::move(cb)); }

    int fd() const noexcept { return port_->get(); }

    // ET 模式下必须一次读干净:循环读到 EAGAIN,否则剩下的字节不会再触发事件。
    void drainAndParse();

    // 组帧并整帧写出。payload 需已含 seq。
    // 返回 false 表示没能完整写出(短写 / 出错),调用方据此决定要不要登记在途表。
    bool send(uint8_t type, const std::vector<uint8_t>& payload);

    // 热加载:换串口设备或波特率。旧 fd 由调用方先从 epoll 摘掉再调本函数
    // (先摘后换,否则 epoll 里留着一个已关闭的 fd)。
    void reopen(const std::string& path, int baud);

    // 解析统计:协议层只计数,呈现在这一层做
    const edge_parser_stats_t& parserStats() const noexcept { return parser_.stats(); }

private:
    std::unique_ptr<SerialPort> port_;
    FrameParser                 parser_;   // 不可移动,故本类也不可拷贝
};

}  // namespace gateway
