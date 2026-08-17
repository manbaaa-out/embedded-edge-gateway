// std::vector 到共享 C 编码器的适配实现。唯一需要在 C++ 边界额外处理的是长度类型：
// vector::size() 为 size_t，而 C 接口为 uint8_t，必须先按 size_t 校验再做窄化转换。

#include "gateway/protocol/FrameCodec.h"

namespace gateway {

std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    // 在转换为 uint8_t 前检查 size_t，避免窄化后将超长 payload 误判为合法长度。
    if (payload.size() > EDGE_PAYLOAD_MAX) return {};

    // 固定大小数组满足 C 编码器“至少 EDGE_FRAME_MAX 字节”的输出容量契约。
    uint8_t buf[EDGE_FRAME_MAX]; // 当前调用独占的临时线上帧缓冲区。
    const uint8_t n = edge_frame_encode(type, // C 编码器返回的实际帧字节数。
                                        payload.empty() ? nullptr : payload.data(),
                                        static_cast<uint8_t>(payload.size()),
                                        buf);
    return std::vector<uint8_t>(buf, buf + n);
}

}  // namespace gateway
