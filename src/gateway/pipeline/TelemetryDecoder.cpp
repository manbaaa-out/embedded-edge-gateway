#include "gateway/pipeline/TelemetryDecoder.h"

#include "gateway/core/log/Logger.h"

namespace gateway {

std::vector<Reading> decodeTelemetry(const Frame& f) {
    std::vector<Reading> out;
    const auto&          p = f.payload;

    // 长度校验统一走共享契约,不再各处手写 p.size() < 5 这样的魔数。
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
        // 定点传输:实际值 ×10 后取整成 uint16 大端(§3.4)。拆成温、湿两条记录。
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
        // 【按位标志,不是枚举】必须逐位 AND,不能把整字节当单一数值。
        // 拆成两路健康状态落库,1.0 = 在线,0.0 = 故障。
        const uint8_t st = p[0];
        out.push_back(Reading{"status_dht11", (st & EDGE_STATUS_BIT_DHT11) ? 1.0 : 0.0});
        out.push_back(Reading{"status_bh1750", (st & EDGE_STATUS_BIT_BH1750) ? 1.0 : 0.0});
        break;
    }

    case EDGE_TYPE_HEARTBEAT:
        break;   // 心跳只证明节点还活着,不落库

    default:
        // 结构合法但不是遥测帧(如应答帧走错了路)。调用方本该先分流,
        // 这里兜底记一笔,免得数据无声无息地消失。
        LOG_WARN("frame 0x%02X is not telemetry, dropped by decoder", f.type);
        break;
    }

    return out;
}

}  // namespace gateway
