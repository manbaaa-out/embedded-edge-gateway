#include "gateway/link/CommandTracker.h"

namespace gateway {

uint8_t CommandTracker::track(uint8_t type, std::vector<uint8_t> arg, TimePoint now) {
    const uint8_t seq = seq_counter_++;
    trackWithSeq(seq, type, std::move(arg), now);
    return seq;
}

void CommandTracker::trackWithSeq(uint8_t seq, uint8_t type, std::vector<uint8_t> arg,
                                  TimePoint now) {
    // 同 seq 覆盖:seq 回绕一圈后旧命令若仍在途,说明它早已应被判死;
    // 保留旧记录只会让后续应答配对到一条无人关心的条目上。
    inflight_[seq] = Entry{InflightCmd{type, std::move(arg), 0}, now};
}

bool CommandTracker::onAck(uint8_t seq) {
    auto it = inflight_.find(seq);
    if (it == inflight_.end()) {
        return false;   // 迟到 / 重复 / 伪造的应答,由调用方决定如何记录
    }
    // 配对成功即销账,不检查结果码:命令成功还是被拒属业务语义,
    // 不影响「它已不在途」这一事实。
    inflight_.erase(it);
    return true;
}

TrackerActions CommandTracker::tick(TimePoint now) {
    TrackerActions actions;

    // 分两阶段:先只读地挑出到期条目,再统一改动。
    // 在遍历中直接 erase 会使迭代器失效。
    std::vector<uint8_t> expired;
    for (const auto& kv : inflight_) {
        if (now - kv.second.sent_at >= timeout_) {
            expired.push_back(kv.first);
        }
    }

    for (uint8_t seq : expired) {
        Entry& e = inflight_[seq];
        if (e.cmd.retry_count < max_retry_) {
            e.cmd.retry_count++;
            e.sent_at = now;
            // 复用同一个 seq。§6.2 据此保证节点侧幂等:节点见到刚处理过的 seq
            // 只补发应答,不重复执行命令本体。
            actions.resend.push_back(
                TrackerActions::Resend{seq, e.cmd.type, e.cmd.arg, e.cmd.retry_count});
        } else {
            actions.failed.push_back(TrackerActions::Failed{seq, e.cmd.type});
            inflight_.erase(seq);
        }
    }

    return actions;
}

}  // namespace gateway
