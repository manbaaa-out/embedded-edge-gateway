#pragma once

#include "edge_proto/edge_proto.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gateway {

// 输出队列中的一次发送。会话和 attempt 同时充当完成回调的身份标识。
struct CommandSend {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    uint64_t session = 0;
    uint8_t seq = 0;
    unsigned attempt = 0;
};

struct CommandCompletion {
    uint8_t seq;
    uint8_t type;
    std::vector<uint8_t> result; // [rc][data...]；收到执行结果才产生成功。
    std::string error;           // timeout / session_aborted / send_failed。
};

struct TrackerActions {
    std::vector<CommandSend> send;
    std::vector<CommandCompletion> finished;
};

// 单线程 SR 发送端，无 I/O。窗口左沿仅跨过连续的接收确认；每帧独立重传。
// 节点返回执行结果后才向业务层完成命令。任何无法恢复的缺口都关闭整会话，
// 更高会话 OPEN 被确认前不能发送新命令；旧命令不会自动搬到新会话重新执行。
class CommandTracker {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    enum class State { Closed, Opening, Ready, Failed };

    TrackerActions start(uint64_t session, TimePoint now);
    std::optional<CommandSend> submit(uint8_t type, std::vector<uint8_t> args, TimePoint now);
    TrackerActions onFrame(uint8_t type, const std::vector<uint8_t>& payload, TimePoint now);
    TrackerActions onWritten(const CommandSend& send, bool success, TimePoint now);
    TrackerActions tick(TimePoint now);
    bool canSend(const CommandSend& send, TimePoint now) const;
    bool canSubmit() const;

    State state() const { return state_; }
    uint64_t session() const { return session_; }
    uint16_t sendBase() const { return base_; }
    uint16_t nextSeq() const { return next_; }
    std::size_t inflightCount() const { return entries_.size(); }
    bool has(uint8_t seq) const { return entries_.count(seq) != 0; }

private:
    struct Entry {
        uint8_t type;
        std::vector<uint8_t> args;
        TimePoint queued_at;
        TimePoint sent_at{};
        unsigned sends = 0;
        bool pending = true;
        bool received = false;
    };
    CommandSend openSend() const;
    CommandSend dataSend(uint8_t seq, const Entry& entry) const;
    CommandSend resultAck(uint8_t seq) const;
    void receive(uint8_t seq);
    TrackerActions abort(uint8_t failed_seq, const char* reason, TimePoint now);
    void begin(uint64_t session, TimePoint now, TrackerActions& actions);
    void rollover(TimePoint now, TrackerActions& actions);

    State state_ = State::Closed;
    uint64_t session_ = 0;
    uint16_t base_ = 0;
    uint16_t next_ = 0;
    std::array<bool, 256> received_{};
    std::array<bool, 256> completed_{};
    std::array<uint8_t, 256> types_{};
    std::map<uint8_t, Entry> entries_;
    TimePoint opening_at_{};
    TimePoint open_sent_at_{};
    unsigned open_sends_ = 0;
    bool open_pending_ = false;
};

} // namespace gateway
