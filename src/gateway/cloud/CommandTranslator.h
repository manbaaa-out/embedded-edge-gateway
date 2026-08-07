#pragma once

// MQTT 下行命令翻译:topic + payload → 一条待发的协议命令。
//
// 纯函数,不访问 broker 与队列。翻译失败以带原因的返回值表达而非静默丢弃,
// 使调用方能把拒绝原因回给运维。

#include "edge_proto/edge_proto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gateway {

// 一条待下发的命令。arg 不含 seq:seq 由 CommandTracker 在发送时分配。
struct DownCmd {
    uint8_t              type;
    std::vector<uint8_t> arg;
};

struct TranslateResult {
    bool        ok = false;
    DownCmd     cmd{};
    std::string error;   // ok == false 时给出原因,可原样回给运维
};

// 翻译一条下行命令。topic 形如 gateway/cmd/<命令名>,取最后一段作为命令名。
// 命令名到 TYPE 的映射以本文件的实现为准。
TranslateResult translateCommand(const std::string& topic, const std::string& payload);

}  // namespace gateway
