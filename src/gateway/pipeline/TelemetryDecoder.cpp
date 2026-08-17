/** @file 协议帧到业务读数的纯转换实现。 */

#include "gateway/pipeline/TelemetryDecoder.h"

#include "gateway/core/log/Logger.h"

namespace gateway {

std::vector<Reading> decodeTelemetry(const Frame& f) {
    std::vector<Reading> out;       // 按帧内字段顺序收集解码结果。
    const auto&          p = f.payload;  // 经过统一长度校验后再按偏移读取。

    // 载荷长度由共享协议定义统一校验。
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
        // 节点以大端定点数传输温度和湿度。
        const double temperature =  // 摄氏温度。
            edge_u16_be_read(&p[0]) / double(EDGE_TEMP_SCALE);
        const double humidity =  // 相对湿度百分比。
            edge_u16_be_read(&p[2]) / double(EDGE_HUMI_SCALE);
        out.push_back(Reading{"temperature", temperature});
        out.push_back(Reading{"humidity", humidity});
        break;
    }

    case EDGE_TYPE_BH1750:
        out.push_back(Reading{"illuminance", static_cast<double>(edge_u16_be_read(&p[0]))});
        break;

    case EDGE_TYPE_STATUS: {
        // 状态位拆成两条 0/1 读数，便于分别查询和展示。
        const uint8_t st = p[0];  // 共享协议定义的传感器健康位图。
        out.push_back(Reading{"status_dht11", (st & EDGE_STATUS_BIT_DHT11) ? 1.0 : 0.0});
        out.push_back(Reading{"status_bh1750", (st & EDGE_STATUS_BIT_BH1750) ? 1.0 : 0.0});
        break;
    }

    case EDGE_TYPE_HEARTBEAT:
        break;

    default:
        // 结构合法但不属于遥测的数据在此兜底记录。
        LOG_WARN("frame 0x%02X is not telemetry, dropped by decoder", f.type);
        break;
    }

    return out;
}

}  // namespace gateway
