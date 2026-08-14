// 在途命令表的时序:发命令登记、收 ACK 销账、定时扫描重发、重试耗尽判失败。
//
// 全程无锁 —— 这三件事都发生在主 Reactor 线程,单线程访问的前提由调用方保证,
// 写在头文件的注释里而不是靠加锁去兜底。时间由调用方注入,所以「重试三次后判死」
// 这种真机上要等 2.4 秒的场景,单测里注入假时钟瞬间就能跑完。

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
        // 用 at() 而不是 operator[]:后者找不到就【默认构造插入】,而这里的语义是
        // 「我确定它存在」(seq 刚从同一个 map 里遍历出来,中间没有删除)。
        // 意图与工具不匹配的代价是隐蔽的 —— 哪天有人在两个循环之间插一段清理逻辑,
        // operator[] 会静默插入一条 type=0x00 的空记录,再在下个 tick 里被当成超时
        // 命令发出去一帧垃圾,而 0x00 恰好是协议里保留的非法 TYPE。
        Entry& e = inflight_.at(seq);
        if (e.cmd.retry_count < max_retry_) {
            e.cmd.retry_count++;
            e.sent_at = now;
            // 复用同一个 seq,节点据此识别重发(§6.2)。
            // 但节点的幂等窗口只有最近一条,而本类不限在途条数 —— 若重发到达时中间已
            // 插进别的命令,旧 seq 已被顶出窗口,命令本体会被再执行一次。真正兜住这件事
            // 的是 §6.6:下行命令必须是查询型或绝对值设置型,重复执行无害。
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
