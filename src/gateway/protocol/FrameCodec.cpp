#include "gateway/protocol/FrameCodec.h"

namespace gateway {

std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    // 定长栈缓冲 → vector。edge_frame_encode 的契约是 out 容量 >= EDGE_FRAME_MAX,
    // 这里用编译期已知大小的数组满足它,不做运行期容量检查(见 edge_frame.h 注释)。
    uint8_t buf[EDGE_FRAME_MAX];
    const uint8_t n = edge_frame_encode(type,
                                        payload.empty() ? nullptr : payload.data(),
                                        static_cast<uint8_t>(payload.size()),
                                        buf);
    return std::vector<uint8_t>(buf, buf + n);   // n == 0(参数非法)时自然得到空 vector
}

}  // namespace gateway
