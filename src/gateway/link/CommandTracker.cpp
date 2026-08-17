// CommandTracker 的纯状态转换实现。时间点只来自方法参数，不读取墙上时钟；返回动作
// 是当前状态变化的值副本，调用方可在离开 tracker 后安全执行串口和 MQTT 操作。

#include "gateway/link/CommandTracker.h"

namespace gateway {

uint8_t CommandTracker::track(uint8_t type, std::vector<uint8_t> arg, TimePoint now) {
    const uint8_t seq = seq_counter_++; // 本条命令在线上和 map 中使用的序号。
    trackWithSeq(seq, type, std::move(arg), now);
    return seq;
}

void CommandTracker::trackWithSeq(uint8_t seq, uint8_t type, std::vector<uint8_t> arg,
                                  TimePoint now) {
    // map 下标赋值明确采用“相同 seq 替换旧条目”的语义。
    inflight_[seq] = Entry{InflightCmd{type, std::move(arg), 0}, now};
}

bool CommandTracker::onAck(uint8_t seq) {
    auto it = inflight_.find(seq); // 与应答序号匹配的候选条目。
    if (it == inflight_.end()) {
        return false;
    }
    // 执行结果由业务层处理；收到应答即结束链路层等待。
    inflight_.erase(it);
    return true;
}

TrackerActions CommandTracker::tick(TimePoint now) {
    TrackerActions actions; // 本轮扫描积累的值语义输出。

    // 先收集 key，再修改或删除 map 中的条目。
    std::vector<uint8_t> expired; // 到期 seq 快照，避免遍历 map 时删除元素。
    for (const auto& kv : inflight_) { // kv 是按 seq 排序的一条只读在途记录。
        if (now - kv.second.sent_at >= timeout_) {
            expired.push_back(kv.first);
        }
    }

    for (uint8_t seq : expired) { // seq 来自同一 map 的只读扫描，当前必然仍存在。
        Entry& e = inflight_.at(seq); // 本轮要更新重试状态或删除的条目。
        if (e.cmd.retry_count < max_retry_) {
            e.cmd.retry_count++;
            e.sent_at = now;
            // 重发复用原 seq，以便节点识别重复请求。
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
