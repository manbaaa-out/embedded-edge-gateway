#pragma once

// ============================================================
// MQTT 下行命令翻译:topic + payload → 一条待发的协议命令。
//
// 重构前这段逻辑长在 mosquitto 回调 lambda 里(GatewayApp::makeDownlinkHandler),
// 与「塞队列 + 戳 eventfd」揉在一起 —— 想验证「period 超范围会不会被挡住」,
// 得先连上 broker 发一条真消息。
//
// 现在它是纯函数,且把「翻译失败」变成了带原因的返回值:
// 原实现遇到未知命令名或非法参数只 LOG_WARN 一句就静默丢弃,
// 运维在 MQTT 那头看不到任何回音,只能猜。现在调用方可以据 error 回一条消息。
// ============================================================

#include "edge_proto/edge_proto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gateway {

// 一条待下发的命令。arg 不含 seq —— seq 由 CommandTracker 在发送时分配。
struct DownCmd {
    uint8_t              type;
    std::vector<uint8_t> arg;
};

struct TranslateResult {
    bool        ok = false;
    DownCmd     cmd{};
    std::string error;   // ok == false 时说明原因,可原样回给运维
};

// topic 形如 gateway/cmd/<命令名>,取最后一段作为命令名。
// 支持的命令名与 TYPE 的对应关系是本文件唯一的真相来源。
TranslateResult translateCommand(const std::string& topic, const std::string& payload);

}  // namespace gateway
