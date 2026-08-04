#pragma once

// ============================================================
// 下行命令的在途表:seq 分配 · 应答配对 · 超时重发 · 重试耗尽判失败。
//
// 【本类不碰任何 fd、不认识串口、不认识 MQTT】——
// 它只回答一个问题:「现在有哪些命令在等应答,谁该重发,谁该判死」。
// 发送这个动作由调用方做,时间由调用方喂。
//
// 这样拆的理由很实在:重构前这套逻辑长在 GatewayApp 的三个 epoll 回调里
// (发命令在 eventfd 回调、收 ACK 在解析回调、超时扫描在 timerfd 回调),
// 想测「重发时是否复用了同一个 seq」就得起 epoll、开串口、连 broker。
// 于是它从来没被测过。现在它是个纯函数式的小状态机,注入假时钟即可跑遍
// 全部路径 —— 包括「重试 3 次仍无应答」这种真机上要等好几秒的场景。
//
// 【线程模型】仅主线程访问(发命令、收 ACK、超时扫描同在主 Reactor 线程),
// 故内部无锁。这条前提由调用方保证,不是本类能强制的。
// ============================================================

#include "edge_proto/edge_proto.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace gateway {

// 一条在途命令。payload 不含 seq —— seq 由本类分配,重发时原样复用(§6.2)。
struct InflightCmd {
    uint8_t              type;
    std::vector<uint8_t> arg;
    int                  retry_count = 0;
};

// tick() 的结论:该重发哪些、该判死哪些。
// 刻意只返回「结论」而不直接动手发:发送要碰串口,一碰就没法单测了。
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

    // 超时时限与最大重试次数来自协议契约(§6.5),不在这里另立一套数字。
    //
    // 重构前这里用的是 time(nullptr) 的【秒级】精度,代码注释自陈
    // 「协议是 500ms,但秒级精度有限,取 1s 保守稳妥」—— 等于把协议契约
    // 打了个两倍的折。改用 steady_clock 毫秒后可以如实按 500ms 执行,
    // 且 steady_clock 不受系统时间调整影响(改系统时钟不会让在途命令集体超时)。
    explicit CommandTracker(std::chrono::milliseconds timeout =
                                std::chrono::milliseconds(EDGE_ACK_TIMEOUT_MS),
                            int max_retry = static_cast<int>(EDGE_MAX_RETRY))
        : timeout_(timeout), max_retry_(max_retry) {}

    // 登记一条刚发出去的命令,返回分配给它的 seq。
    // seq 是 uint8_t,自然按 0xFF → 0x00 回绕(§6.2)。
    uint8_t track(uint8_t type, std::vector<uint8_t> arg, TimePoint now);

    // 分配下一个 seq 但不登记 —— 供调用方「先组帧、发失败就不登记」的流程使用。
    uint8_t nextSeq() { return seq_counter_++; }

    // 显式登记一个已分配 seq 的命令(配合 nextSeq 使用)
    void trackWithSeq(uint8_t seq, uint8_t type, std::vector<uint8_t> arg, TimePoint now);

    // 收到应答:配对成功返回 true 并销账;seq 不在表里返回 false
    // (迟到的、重复的、或伪造的应答 —— 调用方据此记警告)。
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
