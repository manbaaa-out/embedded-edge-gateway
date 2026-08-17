#pragma once

/**
 * @file
 * MQTT 下行消息到设备协议命令的纯翻译层。
 *
 * 本层只解释 topic 和 payload，不访问 broker、串口或队列。调用方因此可以先完成
 * 语义校验，再决定如何排队和回送拒绝原因。
 */

#include "edge_proto/edge_proto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gateway {

/** 待下发的设备命令；序列号由链路层在实际发送时补充。 */
struct DownCmd {
    uint8_t              type;  ///< edge_proto 定义的命令类型。
    std::vector<uint8_t> arg;   ///< 不含序列号的协议参数字节。
};

/** 一次翻译的结果，同时表达成功命令或可回传的拒绝原因。 */
struct TranslateResult {
    bool        ok = false;  ///< true 表示 cmd 可下发。
    DownCmd     cmd{};       ///< ok 为 true 时有效。
    std::string error;       ///< ok 为 false 时说明拒绝原因。
};

/**
 * 以 topic 的最后一段识别命令名，并校验 payload。
 *
 * @param topic 完整 MQTT 主题，例如 gateway/cmd/set_period。
 * @param payload 命令参数原文；无参命令忽略该值。
 * @return 成功时携带协议类型和参数，失败时携带面向调用方的错误说明。
 */
TranslateResult translateCommand(const std::string& topic, const std::string& payload);

}  // namespace gateway
