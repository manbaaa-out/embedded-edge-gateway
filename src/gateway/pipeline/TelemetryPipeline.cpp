#include "gateway/pipeline/TelemetryPipeline.h"

#include "gateway/core/log/Logger.h"

#include <iterator>
#include <utility>

namespace gateway {

TelemetryPipeline::TelemetryPipeline(std::shared_ptr<Database> db) {
    // db 按值捕获后转交 writerLoop,自此只有写线程的栈上持有它。
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
    if (readings.empty()) return true;   // 心跳帧解出空集,不是失败

    Batch b;
    b.rows.reserve(readings.size());
    for (const auto& r : readings) {
        b.rows.push_back(DataRow{r.device, r.value, ts});
    }
    return queue_.try_push(Job{std::move(b)});
}

void TelemetryPipeline::swapDatabase(std::shared_ptr<Database> db) {
    queue_.push(Job{SwapDb{std::move(db)}});
}

void TelemetryPipeline::writerLoop(std::shared_ptr<Database> db) {
    std::vector<DataRow> rows;

    while (auto job = queue_.pop()) {
        // 内层循环做「机会式攒批」:先处理阻塞取到的这一条,再用 try_pop 把此刻
        // 队列里已经排着的一并取走,合进同一个事务。
        //
        // 关键是它不引入任何等待 —— 空闲时 try_pop 立刻返回空,一批就是一帧,
        // 落库延迟与逐条写完全相同;忙起来队列自然堆积,批量自动变大,事务开销
        // 随之摊薄。攒批的力度由负载自己决定,不需要配置攒批窗口。
        do {
            if (auto* swap = std::get_if<SwapDb>(&*job)) {
                // 换库前必须先把已攒的记录落进旧库:它们是热加载生效之前收到的,
                // 属于旧库。攒批不得跨越换库边界。
                if (!rows.empty()) {
                    db->insertBatch(rows);
                    rows.clear();
                }
                db = std::move(swap->db);
                LOG_INFO("%s", "telemetry writer switched to new database");
                break;
            }

            auto& b = std::get<Batch>(*job);
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
