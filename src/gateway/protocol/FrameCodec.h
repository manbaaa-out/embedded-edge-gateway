#pragma once

// edge_proto 的 C++ 封装。协议实现位于仓库顶层 protocol/,与 STM32 侧共享。
//
// 本文件不含任何协议知识(无帧头常量、无 CRC 多项式、无状态机),
// 只把 C 接口的定长缓冲与函数指针包装成 std::vector 与 std::function,
// 使上层无需自行管理缓冲区。

#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_proto.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace gateway {

// 一帧已校验通过的数据。type 的取值见 edge_type_t。
struct Frame {
    uint8_t              type;
    std::vector<uint8_t> payload;
};

// 组装一帧并返回完整字节,可直接交给 SerialPort::write;参数非法时返回空 vector。
// payload 长度须 <= EDGE_PAYLOAD_MAX,超出即属参数非法,绝不会被截断后组成帧。
// payload 须已包含 seq —— seq 由上层命令管理分配,重发时复用同一个值。
std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload);

// 收帧 FSM 的 RAII 外壳。
//
// 不可拷贝也不可移动:edge_parser_t 内部持有指回本对象的 user 指针,对象一旦被搬移
// 该指针即悬空。本类的使用场景是长期持有的成员,不需要搬移,故直接禁用而非
// 实现移动构造去修正指针。
class FrameParser {
public:
    using OnFrameCallback = std::function<void(const Frame&)>;

    FrameParser() { edge_parser_init(&parser_, &FrameParser::trampoline, this); }

    FrameParser(const FrameParser&)            = delete;
    FrameParser& operator=(const FrameParser&) = delete;
    FrameParser(FrameParser&&)                 = delete;
    FrameParser& operator=(FrameParser&&)      = delete;

    void setOnFrame(OnFrameCallback cb) { on_frame_ = std::move(cb); }

    void feed(uint8_t byte) { edge_parser_feed(&parser_, byte); }
    void feed(const uint8_t* buf, std::size_t n) { edge_parser_feed_buf(&parser_, buf, n); }

    // 解析统计。协议层只计数不打印,呈现方式由调用方决定
    const edge_parser_stats_t& stats() const noexcept { return parser_.stats; }

private:
    // C 回调到 C++ 成员的跳板。user 即 this,协议层因此无需任何全局状态。
    static void trampoline(uint8_t type, const uint8_t* payload, uint8_t len, void* user) {
        auto* self = static_cast<FrameParser*>(user);
        if (!self->on_frame_) return;
        self->on_frame_(Frame{type, std::vector<uint8_t>(payload, payload + len)});
    }

    edge_parser_t   parser_{};
    OnFrameCallback on_frame_;
};

}  // namespace gateway
