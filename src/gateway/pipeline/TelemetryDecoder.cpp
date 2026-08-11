// 帧 → 记录的纯函数。一帧温湿度扇出成两条记录,这个 1→N 的扇出正是 pipeline 层
// 存在的理由。不碰 I/O、不碰时间,所以可以直接单测。

#include "gateway/pipeline/TelemetryDecoder.h"

#include "gateway/core/log/Logger.h"

namespace gateway {

std::vector<Reading> decodeTelemetry(const Frame& f) {
    std::vector<Reading> out;
    const auto&          p = f.payload;

    // 长度校验统一走共享契约,避免各处手写魔数。
    // 未知 TYPE 时 edge_payload_len_ok 返回 false,与长度不足合并处理。
    if (!edge_payload_len_ok(f.type, static_cast<uint8_t>(p.size()))) {
        if (edge_min_payload_len(f.type) < 0) {
            LOG_WARN("unknown frame type: 0x%02X, dropped", f.type);
        } else {
            LOG_WARN("frame 0x%02X payload too short: %zu < %d, dropped",
                     f.type, p.size(), edge_min_payload_len(f.type));
        }
        return out;
    }

    switch (f.type) {
    case EDGE_TYPE_DHT11: {
        // 定点传输:实际值 ×10 取整成 uint16 大端(§3.4),拆成温、湿两条记录
        const double temperature = edge_u16_be_read(&p[0]) / double(EDGE_TEMP_SCALE);
        const double humidity    = edge_u16_be_read(&p[2]) / double(EDGE_HUMI_SCALE);
        out.push_back(Reading{"temperature", temperature});
        out.push_back(Reading{"humidity", humidity});
        break;
    }

    case EDGE_TYPE_BH1750:
        out.push_back(Reading{"illuminance", static_cast<double>(edge_u16_be_read(&p[0]))});
        break;

    case EDGE_TYPE_STATUS: {
        // 按位标志而非枚举,必须逐位 AND,不可把整字节当作单一数值。
        // 拆成两路健康状态落库:1.0 为在线,0.0 为故障。
        const uint8_t st = p[0];
        out.push_back(Reading{"status_dht11", (st & EDGE_STATUS_BIT_DHT11) ? 1.0 : 0.0});
        out.push_back(Reading{"status_bh1750", (st & EDGE_STATUS_BIT_BH1750) ? 1.0 : 0.0});
        break;
    }

    case EDGE_TYPE_HEARTBEAT:
        break;   // 心跳仅表明节点存活,不落库

    default:
        // 结构合法但非遥测帧,例如应答帧未被调用方分流。
        // 此处兜底并记录,避免帧被静默丢弃。
        LOG_WARN("frame 0x%02X is not telemetry, dropped by decoder", f.type);
        break;
    }

    return out;
}

}  // namespace gateway
