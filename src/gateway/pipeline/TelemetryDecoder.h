#pragma once

// 业务解码:一帧上行数据 → 若干条 (设备名, 数值) 记录。
//
// 纯函数,不访问任何外部资源。新增一种传感器只需增加一个 case 与一条测试,
// 无需改动装配代码。

#include "gateway/protocol/FrameCodec.h"

#include <string>
#include <vector>

namespace gateway {

// 一条待落库 / 待上云的记录。device 为逻辑设备名,温湿度帧会拆成两条。
struct Reading {
    std::string device;
    double      value;
};

// 解码一帧上行数据帧。
//
// 以下三种情形返回空 vector,均非错误,调用方无需区分:
//   - 心跳帧(0x03):不落库
//   - payload 长度不足:按 edge_min_payload_len 校验后记警告并丢弃
//   - 未知 TYPE:记警告并丢弃。TYPE 字典的合法性属业务层职责,协议层不校验
//
// 前置条件:应答帧(0x05 / 0x06)属命令链路,应由调用方先行分流,不应传入本函数。
std::vector<Reading> decodeTelemetry(const Frame& frame);

}  // namespace gateway
