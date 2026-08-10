#pragma once

// 遥测落库:主 Reactor 线程投递,一条专属写线程消费。
//
// ---- 为什么不是线程池 ----
// 这里曾经是「每条记录一个 task 丢进线程池」。那个设计的问题不在线程数,而在
// 两点:
//   1. 落库动作本身是串行的 —— N 个 worker 全撞在 Database 的那把 mutex 上,
//      并行度被锁又压回 1,只多付了锁竞争与上下文切换;
//   2. 线程池的 submit 收的是 std::function<void()>,一个不透明闭包。池看不见
//      闭包里在做什么,于是结构上不可能把 N 条记录合并进一个事务 —— 而事务
//      批处理恰恰是这条路径上最大的那块收益。
// 换成「有类型的队列 + 单一消费者」后,两个问题一起消失:没有共享就不需要锁,
// 队列元素有类型,消费者就能一次捞多条合进同一个事务。
//
// ---- 线程契约 ----
// 写连接的所有权完全属于写线程:它是 writerLoop 的参数,活在那条线程的栈上,
// 别处拿不到。热加载换库不是跨线程改指针,而是往同一条队列里投一条换库指令,
// 由写线程在自己的时间线上按顺序执行 —— 换库与落库因此天然互斥,不需要锁。

#include "gateway/core/concurrent/ThreadSafeQueue.h"
#include "gateway/pipeline/TelemetryDecoder.h"
#include "gateway/storage/Database.h"

#include <cstddef>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

namespace gateway {

class TelemetryPipeline {
public:
    // 队列上限。按当前负载(单节点 2 秒一帧)这相当于十几分钟的缓冲,
    // 真的撑满只可能是磁盘写不动了,那时丢新数据比无限堆内存正确。
    static constexpr std::size_t kQueueCapacity = 1024;

    // 单个事务的记录数上限。攒批的收益随批量增长很快见顶,而批越大,
    // 停机时最后一个事务要写的东西越多。取一个够用又不至于拖长停机的值。
    static constexpr std::size_t kMaxRowsPerTxn = 256;

    // db 的所有权就此交给写线程,调用方不应再调用它的任何方法。
    explicit TelemetryPipeline(std::shared_ptr<Database> db);

    // 关队列并 join 写线程。ThreadSafeQueue::pop 在 shutdown 后仍会先把队列里
    // 剩下的取空才返回 nullopt,故在飞的记录不会因为停机而丢。
    ~TelemetryPipeline();

    TelemetryPipeline(const TelemetryPipeline&)            = delete;
    TelemetryPipeline& operator=(const TelemetryPipeline&) = delete;

    // 主 Reactor 线程调用。返回 false 表示队列已满、本批被丢弃。
    //
    // 用 try_push 而非 push:生产者是主 Reactor 线程,阻塞它会让串口收帧、
    // ACK 配对、超时重发、SIGTERM 一起停摆。丢一条传感器读数是可接受的,
    // Reactor 停住不是。
    bool submit(const std::vector<Reading>& readings, long ts);

    // SIGHUP 改了 db_path 时调用,把新连接交给写线程。
    //
    // 这条用阻塞 push:换库指令丢了,写线程会一直往旧库写,而旧库正是本次
    // 热加载要换掉的东西 —— 与遥测记录不同,它不可丢。
    void swapDatabase(std::shared_ptr<Database> db);

private:
    // 一批待落库的记录。在 Reactor 线程就转成 DataRow,使写线程只依赖 storage 层。
    struct Batch {
        std::vector<DataRow> rows;
    };
    // 换库指令。走队列而不是直接改成员,换库便自动排进写线程的时间线。
    struct SwapDb {
        std::shared_ptr<Database> db;
    };
    using Job = std::variant<Batch, SwapDb>;

    // db 按值传入并只存在于本函数的栈上 —— 这就是「写线程独占连接」的实现方式。
    void writerLoop(std::shared_ptr<Database> db);

    ThreadSafeQueue<Job> queue_{kQueueCapacity};
    std::thread          writer_;   // 最后声明:构造完成后才起线程
};

}  // namespace gateway
