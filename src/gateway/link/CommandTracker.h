#pragma once

// 下行命令的在途状态机。
//
// 该类只保存“已成功发出、正在等待应答”的命令，不直接访问串口或 MQTT。调用方注入
// steady_clock 时间并执行 tick() 返回的重发/失败动作，因此计时逻辑可独立测试。
// 类内部无锁，所有方法必须由同一线程串行调用。seq 只有 8 位且分配器自然回绕；
// trackWithSeq() 对重复 key 采用替换语义，所以调用方必须限制在途数量或跳过占用中的 seq。

#include "edge_proto/edge_proto.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace gateway {

/** 一条已成功发出、等待 ACK 或 QUERY_RESP 的命令快照。 */
struct InflightCmd {
    uint8_t type;                /**< 下行 TYPE。 */
    std::vector<uint8_t> arg;    /**< 不含 seq 的业务参数，重发时原样复用。 */
    int retry_count = 0;         /**< 已安排的重发次数，不包含首次发送。 */
};

/** tick() 计算出的外部动作；调用方负责真正发送和发布最终失败结果。 */
struct TrackerActions {
    /** 一条到达超时时限、仍可继续尝试的命令。 */
    struct Resend {
        uint8_t seq;             /**< 与首次发送相同的链路序号。 */
        uint8_t type;            /**< 下行 TYPE。 */
        std::vector<uint8_t> arg; /**< 不含 seq 的业务参数副本。 */
        int attempt;             /**< 从 1 开始的本次重试编号。 */
    };
    /** 一条已耗尽全部重试并从在途表移除的命令。 */
    struct Failed {
        uint8_t seq;  /**< 失败命令的链路序号。 */
        uint8_t type; /**< 失败命令的 TYPE，供日志与业务响应使用。 */
    };

    std::vector<Resend> resend; /**< 本轮需要重发的动作，按 seq 的 map 顺序排列。 */
    std::vector<Failed> failed; /**< 本轮终止等待的动作，按 seq 的 map 顺序排列。 */
};

/** 单线程的命令关联与重试调度器。 */
class CommandTracker {
public:
    using Clock = std::chrono::steady_clock; /**< 不受系统时间校准影响的计时源。 */
    using TimePoint = Clock::time_point;      /**< 由调用方传入的单调时间点。 */

    /**
     * @brief 创建空在途表。
     * @param timeout 每次发送或重发后等待应答的时长，必须为正值。
     * @param max_retry 首次发送之外允许的重发次数，必须不小于 0。
     */
    explicit CommandTracker(std::chrono::milliseconds timeout =
                                std::chrono::milliseconds(EDGE_ACK_TIMEOUT_MS),
                            int max_retry = static_cast<int>(EDGE_MAX_RETRY))
        : timeout_(timeout), max_retry_(max_retry) {}

    /**
     * @brief 分配 seq 并登记一条已发送命令。
     * @param type 下行 TYPE。
     * @param arg 不含 seq 的业务参数，移动到在途表。
     * @param now 首次成功发送时刻。
     * @return 分配的 8 位 seq；回绕后可能替换仍占用同一 seq 的旧条目。
     * @throws std::bad_alloc 在途 map 分配失败；此时 seq 计数器已经推进。
     */
    uint8_t track(uint8_t type, std::vector<uint8_t> arg, TimePoint now);

    /** @return 下一个 8 位 seq 并推进计数器；不修改在途表。 */
    uint8_t nextSeq() { return seq_counter_++; }

    /**
     * @brief 登记一条已成功发送且 seq 已由调用方分配的命令。
     * @param seq 线上链路序号；已有同 key 条目会被无提示替换。
     * @param type 下行 TYPE。
     * @param arg 不含 seq 的业务参数，移动到在途表。
     * @param now 成功发送时刻，用作首次超时起点。
     * @throws std::bad_alloc 插入新 seq 时 map 分配失败。
     */
    void trackWithSeq(uint8_t seq, uint8_t type, std::vector<uint8_t> arg, TimePoint now);

    /**
     * @brief 按 seq 结束链路层等待。
     * @param seq ACK/QUERY_RESP 携带的序号。
     * @return 找到并移除条目时为 true；迟到、重复或未知 seq 为 false。
     */
    bool onAck(uint8_t seq);

    /**
     * @brief 扫描所有在途条目并推进超时状态。
     * @param now 本轮调度使用的单调时间点。
     * @return 需要调用方执行的重发与最终失败动作。
     *
     * 生成 Resend 时即更新 sent_at/retry_count，与随后串口发送是否成功无关；生成
     * Failed 时立即移除条目。
     * 生成动作副本时的 vector 分配异常会传播；该路径不提供强异常保证。
     */
    TrackerActions tick(TimePoint now);

    /** @return 当前等待应答的命令数量。仅限所属线程使用。 */
    std::size_t inflightCount() const noexcept { return inflight_.size(); }
    /** @param seq 待查询序号。@return 该序号当前是否在途。 */
    bool        has(uint8_t seq) const { return inflight_.count(seq) != 0; }

private:
    /** map 中保存的命令内容及本轮超时起点。 */
    struct Entry {
        InflightCmd cmd;  /**< 类型、参数与已安排重试次数。 */
        TimePoint sent_at; /**< 首次发送或最近一次安排重发的时间点。 */
    };

    std::map<uint8_t, Entry> inflight_; /**< 以 seq 唯一索引的在途条目。 */
    uint8_t seq_counter_ = 0;           /**< nextSeq() 下一次返回值，自然模 256 回绕。 */
    std::chrono::milliseconds timeout_; /**< 每轮等待应答的固定时限。 */
    int max_retry_;                     /**< 允许安排的重发总次数。 */
};

}  // namespace gateway
