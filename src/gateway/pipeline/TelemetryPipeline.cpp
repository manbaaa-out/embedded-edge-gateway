/**
 * @file
 * 遥测写线程实现。消费者阻塞等待首个任务，随后只合并已经到达的任务，不设置额外
 * 攒批窗口：空闲时保持低延迟，积压时自动提高事务批量。
 */

#include "gateway/pipeline/TelemetryPipeline.h"

#include "gateway/core/log/Logger.h"

#include <exception>
#include <iterator>
#include <utility>

namespace gateway {

TelemetryPipeline::TelemetryPipeline(std::shared_ptr<Database> db) {
    // 将 shared_ptr 按值移入线程入口，使连接生命周期覆盖整个消费循环。
    writer_ = std::thread([this, db = std::move(db)]() mutable {
        writerLoop(std::move(db));
    });
    LOG_INFO("%s", "telemetry writer thread started");
}

TelemetryPipeline::~TelemetryPipeline() {
    queue_.shutdown();
    if (writer_.joinable()) writer_.join();
}

bool TelemetryPipeline::submit(const std::vector<Reading>& readings, long ts) {
    if (readings.empty()) return true;

    Batch b;  // 当前帧对应的一组数据库行。
    b.rows.reserve(readings.size());
    for (const auto& r : readings) {  // r 是一条已完成定标的业务读数。
        b.rows.push_back(DataRow{r.device, r.value, ts});
    }
    return queue_.try_push(Job{std::move(b)});
}

bool TelemetryPipeline::swapDatabase(std::shared_ptr<Database> db, SwapAppliedCallback on_applied) {
    return queue_.push_for(Job{SwapDb{std::move(db), std::move(on_applied)}}, kSwapTimeout);
}

void TelemetryPipeline::writerLoop(std::shared_ptr<Database> db) {
    std::vector<DataRow> rows;  // 当前事务候选行，不跨越 SwapDb 任务。

    while (auto job = queue_.pop()) {  // job 是本轮首先阻塞取得、随后尝试取得的任务。
        // 阻塞取得第一项后，仅合并此刻已经排队的批次。
        do {
            if (auto* swap = std::get_if<SwapDb>(&*job)) {  // 非空表示遇到换库边界。
                // 换库任务是批次边界，之前积累的记录仍写入旧连接。
                if (!rows.empty()) {
                    db->insertBatch(rows);
                    rows.clear();
                }
                db = std::move(swap->db);
                LOG_INFO("%s", "telemetry writer switched to new database");
                if (swap->on_applied) {
                    try {
                        // 通知发生在写连接替换之后，因此读侧不会先于写侧切到新库。
                        swap->on_applied();
                    } catch (const std::exception& e) {
                        LOG_ERROR("database swap callback failed: %s", e.what());
                    } catch (...) {
                        LOG_ERROR("%s", "database swap callback failed with unknown exception");
                    }
                }
                break;
            }

            auto& b = std::get<Batch>(*job);  // 当前待合并的数据批次。
            rows.insert(rows.end(),
                        std::make_move_iterator(b.rows.begin()),
                        std::make_move_iterator(b.rows.end()));

            if (rows.size() >= kMaxRowsPerTxn) break;
            job = queue_.try_pop();
        } while (job);

        if (!rows.empty()) {
            db->insertBatch(rows);
            rows.clear();
        }
    }

    LOG_INFO("%s", "telemetry writer thread exiting");
}

}  // namespace gateway
