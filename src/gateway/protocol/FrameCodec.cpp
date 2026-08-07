#include "gateway/protocol/FrameCodec.h"

namespace gateway {

std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    // edge_frame_encode 要求 out 容量 >= EDGE_FRAME_MAX。此处以编译期已知大小的
    // 栈数组满足该前置条件,无需运行期容量检查。
    uint8_t buf[EDGE_FRAME_MAX];
    const uint8_t n = edge_frame_encode(type,
                                        payload.empty() ? nullptr : payload.data(),
                                        static_cast<uint8_t>(payload.size()),
                                        buf);
    return std::vector<uint8_t>(buf, buf + n);   // n == 0(参数非法)时得到空 vector
}

}  // namespace gateway
