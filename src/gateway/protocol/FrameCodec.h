#pragma once

// 网关侧协议适配层。
//
// 共享的 edge_proto C 模块仍是帧格式、CRC 和状态机的唯一实现；本文件只把调用方提供
// 的定长缓冲区包装成 std::vector，把 C 函数指针包装成 std::function。解析器把 this
// 放入 C 状态机的 user 指针，因此对象地址必须稳定，类显式禁用复制和移动。

#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_proto.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace gateway {

/** 已通过长度与 CRC 校验、但尚未经过业务 TYPE/payload 校验的帧值对象。 */
struct Frame {
    uint8_t type;                /**< 线上 TYPE 原始值。 */
    std::vector<uint8_t> payload; /**< 拥有自己的 payload 副本，不依赖 C 解析器缓冲区。 */
};

/**
 * @brief 使用共享 C 编码器构造完整线上帧。
 * @param type 写入 TYPE 字段的原始值，本层不判断业务合法性。
 * @param payload 待编码字节；命令类 payload 的 seq 必须已由上层放入。
 * @return 帧头到 CRC 的完整字节；payload 超限时返回空 vector，绝不截断。
 * @throws std::bad_alloc 临时帧或返回 vector 分配失败。
 */
std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload);

/** 底层 edge_parser_t 的定址 C++ 外壳；所有 feed 和回调须由同一线程串行执行。 */
class FrameParser {
public:
    using OnFrameCallback = std::function<void(const Frame&)>; /**< 同步帧处理函数。 */

    /** 初始化底层状态机，并将 this 登记为 C 回调上下文。 */
    FrameParser() { edge_parser_init(&parser_, &FrameParser::trampoline, this); }

    /** 复制源对象不会被读取；底层 user 指针必须继续指向唯一对象。 */
    FrameParser(const FrameParser&)            = delete;
    /** 复制源对象不会被读取；解析状态和回调不共享。 */
    FrameParser& operator=(const FrameParser&) = delete;
    /** 移动源对象不会被读取；搬迁地址会使底层 user 指针悬空。 */
    FrameParser(FrameParser&&)                 = delete;
    /** 移动源对象不会被读取；本对象地址在整个解析生命周期内固定。 */
    FrameParser& operator=(FrameParser&&)      = delete;

    /**
     * @brief 替换帧处理函数。
     * @param cb 新回调；空 std::function 表示只解析和计数，不向上交付。
     * @pre 不得与 feed() 或另一次 setOnFrame() 并发。
     */
    void setOnFrame(OnFrameCallback cb) { on_frame_ = std::move(cb); }

    /**
     * @brief 输入一个线上字节。
     * @param byte 下一个按序到达的字节。
     * @throws std::bad_alloc 临时 Frame 分配失败。业务回调抛出的其他异常原样传播。
     */
    void feed(uint8_t byte) { edge_parser_feed(&parser_, byte); }
    /**
     * @brief 输入一段连续线上字节。
     * @param buf 指向至少 n 个可读字节；NULL 会由 C 层当作无操作。
     * @param n 输入字节数。
     * @throws std::bad_alloc 临时 Frame 分配失败。业务回调抛出的其他异常原样传播。
     */
    void feed(const uint8_t* buf, std::size_t n) { edge_parser_feed_buf(&parser_, buf, n); }

    /** @return 底层累计统计的只读引用；引用不得在本对象销毁后使用。 */
    const edge_parser_stats_t& stats() const noexcept { return parser_.stats; }

private:
    /**
     * @brief 将 C 回调转发给 user 指向的 FrameParser。
     * @param type 已通过帧校验的 TYPE。
     * @param payload C 解析器拥有的临时 payload。
     * @param len payload 字节数。
     * @param user 构造时登记的 this 指针。
     *
     * 转发前构造拥有 payload 副本的临时 Frame；业务回调若需在返回后保留内容，必须
     * 再复制该 Frame。
     */
    static void trampoline(uint8_t type, const uint8_t* payload, uint8_t len, void* user) {
        auto* self = static_cast<FrameParser*>(user); // 当前 C 状态机所属的 C++ 对象。
        if (!self->on_frame_) return;
        self->on_frame_(Frame{type, std::vector<uint8_t>(payload, payload + len)});
    }

    edge_parser_t parser_{};       /**< 内嵌的 C 状态机、payload 缓冲与累计统计。 */
    OnFrameCallback on_frame_;     /**< 可为空的业务回调，由 setOnFrame() 替换。 */
};

}  // namespace gateway
