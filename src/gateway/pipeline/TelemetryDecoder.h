#pragma once

/**
 * @file
 * 协议帧到业务遥测读数的纯转换接口。
 *
 * 协议层只保证帧结构，本层解释传感器定标、状态位和逻辑设备名称。一帧可扇出为
 * 多条 Reading，后续存储与上云路径因此无需了解具体传感器帧格式。
 */

#include "gateway/protocol/FrameCodec.h"

#include <string>
#include <vector>

namespace gateway {

/** 一条可独立落库和发布的逻辑设备读数。 */
struct Reading {
    std::string device;  ///< 稳定的逻辑设备标识，也是数据库与 MQTT 路径中的名称。
    double      value;   ///< 已按协议定标还原的数值；状态量使用 0.0/1.0。
};

/**
 * 解码一帧遥测数据。
 *
 * @param frame 已通过帧校验的协议帧。
 * @return 解出的零到多条读数。心跳、载荷长度错误和非遥测类型返回空集合。
 *
 * 命令应答应由调用方预先分流；若误传入，本函数会记录警告并返回空集合。
 */
std::vector<Reading> decodeTelemetry(const Frame& frame);

}  // namespace gateway
