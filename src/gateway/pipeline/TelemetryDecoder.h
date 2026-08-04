#pragma once

// ============================================================
// 业务解码:一帧上行数据 → 若干条 (设备名, 数值) 记录。
//
// 重构前这是 GatewayApp.cpp 匿名 namespace 里的 decodeFrame(),
// 与线程池、数据库、MQTT 客户端搅在同一个文件里 —— 想验证「0x04 状态帧的
// bitmask 是否被正确拆成两路」,得先把整条链路跑起来。
//
// 现在它是个纯函数:输入一帧,输出一组记录,不碰任何外部资源。
// 加一种传感器 = 加一个 case + 一条测试,不必动装配代码。
// ============================================================

#include "gateway/protocol/FrameCodec.h"

#include <string>
#include <vector>

namespace gateway {

// 一条待落库 / 待上云的记录。device 是逻辑设备名(温湿度帧会拆成两条)。
struct Reading {
    std::string device;
    double      value;
};

// 解码一帧上行数据帧。
//
// 返回空 vector 的三种情形,都不是错误,调用方无需区分:
//   - 心跳帧(0x03):不落库
//   - payload 长度不足:已按 edge_min_payload_len 校验,记警告后丢弃
//   - 未知 TYPE:记警告后丢弃(协议层不管 TYPE 字典,这是业务层的责任)
//
// 应答帧(0x05 / 0x06)不该走到这里 —— 它们属于命令链路,由调用方先分流。
std::vector<Reading> decodeTelemetry(const Frame& frame);

}  // namespace gateway
