#pragma once

// 双写:每条记录在 worker 线程中同时落本地 SQLite 并向 MQTT 发布,不阻塞主 Reactor。
//
// shared_ptr 按值传递而非持有引用,是本类的关键约束。热加载会替换外层的
// db_ / client_;若 task 捕获引用或裸指针,旧对象可能在 task 仍在执行时被销毁。
// 按值捕获快照后,旧对象的生命周期被在飞的 task 延续至其完成。
//
// 同理,submit() 要求每次传入当前快照,而不在构造时存一份 —— 存一份等同于引用语义,
// 热加载后 pipeline 会继续写入旧库。

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

    // 把一组记录提交给线程池双写。db / client 须传入当前快照(见文件头)。
    // ts 由调用方在收帧时刻取得而非在 worker 中取:后者会让线程池积压
    // 整体推后时间戳。
    void submit(const std::vector<Reading>&   readings,
                std::shared_ptr<Database>     db,
                std::shared_ptr<MqttClient>   client,
                long                          ts);

private:
    ThreadPool& pool_;
};

}  // namespace gateway
