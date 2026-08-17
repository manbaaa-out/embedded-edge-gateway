#pragma once

/**
 * @file
 * Reactor 到 SQLite 写线程之间的异步遥测流水线。
 *
 * 有界队列保护主事件循环不被磁盘 I/O 阻塞；单一消费者独占写连接，并将当前已积压
 * 的记录合并为事务。数据库切换与数据批次使用同一条有序队列，从结构上保证换库不会
 * 与写入并发，也不会让一个批次跨越换库边界。
 */

#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include "gateway/pipeline/TelemetryDecoder.h"
#include "gateway/storage/Database.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

namespace gateway {

/** 管理遥测写队列和唯一数据库写线程。 */
class TelemetryPipeline {
public:
    /** 最大待处理任务数；存储持续跟不上时拒绝新数据而非无限增长内存。 */
    static constexpr std::size_t kQueueCapacity = 1024;

    /** 机会式组批的刷写触发阈值；单个 Batch 自身较大时事务可超过该值。 */
    static constexpr std::size_t kMaxRowsPerTxn = 256;

    /**
     * 启动写线程并将连接交给该线程独占使用。
     * @param db 已完成初始化的可写数据库连接；不得为空。
     * @pre 调用方不再通过其他 shared_ptr 并发访问同一个 Database 对象。
     */
    explicit TelemetryPipeline(std::shared_ptr<Database> db);

    /** 关闭队列并等待写线程按顺序排空已经接收的任务。 */
    ~TelemetryPipeline();

    TelemetryPipeline(const TelemetryPipeline& /* other */) = delete;  ///< 写线程不可复制。
    TelemetryPipeline& operator=(const TelemetryPipeline& /* other */) = delete;  ///< 队列不可复制。

    /**
     * 将同一帧产生的读数作为一个任务非阻塞投递。
     * @param readings 待写入的业务读数；空集合视为成功的无操作。
     * @param ts 采集时间，Unix 秒。
     * @return true 表示任务已入队或无需写入；false 表示队列已满或关闭。
     */
    bool submit(const std::vector<Reading>& readings, long ts);

    /** 换库控制任务等待队列空间时传给 wait_for 的时限参数。 */
    static constexpr std::chrono::milliseconds kSwapTimeout{200};

    /**
     * 写连接真正切换后的通知函数。
     *
     * 回调在数据库写线程执行：此前排队的数据已经写入旧库，新的写连接已经成为当前
     * 连接。实现不得阻塞；异常会被 writerLoop 捕获并记录，不会终止写线程。
     */
    using SwapAppliedCallback = std::function<void()>;

    /**
     * 将新数据库连接作为有序控制任务交给写线程。
     * @param db 新的可写连接；任务执行后由写线程独占使用。
     * @param on_applied 切换真正生效后的可选通知；用于同步发布依赖新写库的读侧资源。
     * @return true 仅表示入队成功，不表示切换已经执行完毕。
     * @pre 入队成功后，调用方不再并发访问同一个 Database 对象。
     */
    bool swapDatabase(std::shared_ptr<Database> db, SwapAppliedCallback on_applied = {});

private:
    /** 一次 submit 产生的数据库行集合。 */
    struct Batch {
        std::vector<DataRow> rows;  ///< 保持原读数顺序的待写行。
    };
    /** 排在数据批次之间的数据库切换控制任务。 */
    struct SwapDb {
        std::shared_ptr<Database> db;   ///< 切换完成后由写线程持有的新连接。
        SwapAppliedCallback on_applied; ///< 新连接生效后在写线程执行的通知。
    };
    using Job = std::variant<Batch, SwapDb>;  ///< 队列中可出现的数据任务与控制任务。

    /**
     * 写线程入口，按队列顺序机会式组批。
     * @param db 当前可写连接，仅在写线程栈上替换和使用。
     */
    void writerLoop(std::shared_ptr<Database> db);

    ThreadSafeQueue<Job> queue_{kQueueCapacity};  ///< Reactor 与写线程之间的有界任务队列。
    std::thread writer_;                          ///< 唯一数据库写线程。
};

}  // namespace gateway
