// 蹦床的落地处。这里唯一的技术点是 C 回调怎么弹回 C++ 成员:
// 注册一个静态函数,把 this 通过 void* user 带进去,静态函数里再转回来。
// 没有这个 user 参数,C 回调想找到对应的 C++ 对象就只能靠全局指针 ——
// 那样一个进程里就只能有一个解析器实例。

#include "gateway/protocol/FrameCodec.h"

namespace gateway {

std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& payload) {
    // 长度检查必须在此,不能只依赖 edge_frame_encode 内部那道。
    //
    // C 接口的 payload_len 是 uint8_t,size() 是 size_t —— 窄化会把 256 的整数倍截成
    // 合法小值:size()==256 截成 0,组出 LEN=1 的空 payload 帧;size()==300 截成 44,
    // 组出只含前 44 字节的帧。两者 CRC 都自洽,收帧端无从察觉,而 send() 还会报成功。
    // 拒绝的条件因此必须按 size_t 判,轮不到 uint8_t 参数去把关。
    if (payload.size() > EDGE_PAYLOAD_MAX) return {};

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
