#pragma once

// ============================================================
// 协议编解码的 C++ 外衣 —— 真正的实现在 protocol/(与 STM32 共享的 C99 核心)。
//
// 本文件【不含任何协议知识】:没有 0xAA、没有 CRC 多项式、没有状态机。
// 它只做一件事:把 edge_proto 的 C 接口(定长缓冲 + 函数指针)包成网关侧
// 顺手的形态(std::vector + std::function),让上层代码不必自己管缓冲区。
//
// 重构前这里是 CRC16.{h,cpp} + FrameParser.{h,cpp} + FrameBuilder.{h,cpp}
// 三对文件、约 220 行,与 STM32 侧的 C 实现是同一套逻辑的两份手抄。
// 现在两端编译同一份源码,这里只剩 60 行胶水。
// ============================================================

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

// 组帧:payload 里应已包含 seq(由上层命令管理分配,重发要复用同 seq)。
// 返回完整帧字节,可直接交给 SerialPort::write。参数非法时返回空 vector。
std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload);

// 收帧 FSM 的 RAII 外壳。
//
// 【不可拷贝也不可移动】edge_parser_t 内部存着指回本对象的 user 指针,
// 一旦对象被搬走,那个指针就悬空了。与其写移动构造去修指针(还得记得每次
// 加成员都同步),不如直接禁掉 —— 本类的使用场景是长期持有的成员,不需要搬。
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

    // 解析统计:协议层只计数不打印,由调用方决定怎么呈现(网关走 LOG_WARN)
    const edge_parser_stats_t& stats() const noexcept { return parser_.stats; }

private:
    // C 回调 → C++ 成员的跳板。user 就是 this,故协议层无需任何全局状态。
    static void trampoline(uint8_t type, const uint8_t* payload, uint8_t len, void* user) {
        auto* self = static_cast<FrameParser*>(user);
        if (!self->on_frame_) return;
        self->on_frame_(Frame{type, std::vector<uint8_t>(payload, payload + len)});
    }

    edge_parser_t   parser_{};
    OnFrameCallback on_frame_;
};

}  // namespace gateway
