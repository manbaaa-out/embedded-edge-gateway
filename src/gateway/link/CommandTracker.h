#pragma once

// 下行命令的在途表:seq 分配、应答配对、超时重发、重试耗尽判失败。
//
// 本类不持有 fd,也不感知串口与 MQTT;它只回答「当前有哪些命令在等应答,
// 谁该重发,谁该判死」,发送动作由调用方执行,当前时间由调用方注入。
// 由此可注入假时钟遍历全部路径,包括重试耗尽这类真机上需等待数秒的场景。
//
// 线程模型:仅主线程访问(发命令、收 ACK、超时扫描同在主 Reactor 线程),
// 故内部无锁。该前提由调用方保证,本类不做强制。

#include "edge_proto/edge_proto.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace gateway {

// 一条在途命令。arg 不含 seq:seq 由本类分配,重发时原样复用(§6.2)。
struct InflightCmd {
    uint8_t              type;
    std::vector<uint8_t> arg;
    int                  retry_count = 0;
};

// tick() 的结论:该重发哪些、该判死哪些。
// 只返回结论而不直接发送,使本类与串口解耦、可单测。
struct TrackerActions {
    struct Resend {
        uint8_t              seq;
        uint8_t              type;
        std::vector<uint8_t> arg;
        int                  attempt;    // 第几次重试(1-based),仅供日志
    };
    struct Failed {
        uint8_t seq;
        uint8_t type;
    };

    std::vector<Resend> resend;
    std::vector<Failed> failed;
};

class CommandTracker {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // 超时时限与最大重试次数取自协议契约(§6.5),不在此另立一套数字。
    // 计时用 steady_clock 而非 time():毫秒精度可如实执行 500ms 时限,
    // 且不受系统时间调整影响,改系统时钟不会导致在途命令集体超时。
    explicit CommandTracker(std::chrono::milliseconds timeout =
                                std::chrono::milliseconds(EDGE_ACK_TIMEOUT_MS),
                            int max_retry = static_cast<int>(EDGE_MAX_RETRY))
        : timeout_(timeout), max_retry_(max_retry) {}

    // 登记一条刚发出去的命令,返回分配给它的 seq。
    // seq 是 uint8_t,自然按 0xFF → 0x00 回绕(§6.2)。
    uint8_t track(uint8_t type, std::vector<uint8_t> arg, TimePoint now);

    // 分配下一个 seq 但不登记,供「先组帧、发送失败则不登记」的流程使用。
    uint8_t nextSeq() { return seq_counter_++; }

    // 显式登记一个已分配 seq 的命令(配合 nextSeq 使用)
    void trackWithSeq(uint8_t seq, uint8_t type, std::vector<uint8_t> arg, TimePoint now);

    // 处理应答:配对成功返回 true 并销账;seq 不在表中返回 false,
    // 对应迟到、重复或伪造的应答,由调用方决定如何记录。
    bool onAck(uint8_t seq);

    // 推进时间,返回该重发与该判死的命令。
    // 重发的条目会就地更新计时与重试计数,判死的条目会被移除。
    TrackerActions tick(TimePoint now);

    std::size_t inflightCount() const noexcept { return inflight_.size(); }
    bool        has(uint8_t seq) const { return inflight_.count(seq) != 0; }

private:
    struct Entry {
        InflightCmd cmd;
        TimePoint   sent_at;
    };

    std::map<uint8_t, Entry>  inflight_;
    uint8_t                   seq_counter_ = 0;
    std::chrono::milliseconds timeout_;
    int                       max_retry_;
};

}  // namespace gateway
