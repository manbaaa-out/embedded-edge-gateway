#pragma once

// ============================================================
// 双写:每条记录同时落本地 SQLite 与向 MQTT 上行发布,两件事在 worker 线程做,
// 互不阻塞主 Reactor。
//
// 【为什么按值传 shared_ptr 而不是持有引用】—— 这是本文件唯一的要害。
//
// 热加载会 reset 外层的 db_ / client_(换库路径、换 broker)。若 task 捕获的是
// 引用或裸指针,旧对象在 task 还在飞的时候就被销毁了 —— use-after-free。
// 按值捕获 shared_ptr 快照后,旧对象的生命被在飞的 task 续着,干完才析构。
//
// 所以 submit() 每次都要求调用方传入【当前的】快照,而不是在构造时存一份:
// 存一份就等于持有了引用语义,热加载后 pipeline 还在往旧库里写。
// ============================================================

#include "gateway/cloud/MqttClient.h"
#include "gateway/core/concurrent/ThreadPool.h"
#include "gateway/pipeline/TelemetryDecoder.h"
#include "gateway/storage/Database.h"

#include <memory>
#include <vector>

namespace gateway {

class TelemetryPipeline {
public:
    explicit TelemetryPipeline(ThreadPool& pool) : pool_(pool) {}

    // 把一组记录提交给线程池双写。db / client 传【当前快照】(见文件头)。
    // ts 由调用方在收帧时刻打,不在 worker 里取 —— 否则线程池积压会让
    // 时间戳整体后移,曲线上看是数据"迟到"。
    void submit(const std::vector<Reading>&   readings,
                std::shared_ptr<Database>     db,
                std::shared_ptr<MqttClient>   client,
                long                          ts);

private:
    ThreadPool& pool_;
};

}  // namespace gateway
