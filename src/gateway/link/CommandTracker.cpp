#include "gateway/link/CommandTracker.h"

#include <limits>
#include <utility>

namespace gateway {
namespace {
using Ms = std::chrono::milliseconds;

bool resultValid(uint8_t type, const std::vector<uint8_t>& p) {
    if (p.size() < 11 || p[10] > EDGE_RC_BUSY) return false;
    if (p[10] != EDGE_RC_OK) return p.size() == 11;
    switch (type) {
    case EDGE_TYPE_QUERY_LIGHT:
        return p.size() == 13;
    case EDGE_TYPE_QUERY_TH:
        return p.size() == 15;
    case EDGE_TYPE_SET_PERIOD:
        return p.size() == 11;
    default:
        return false;
    }
}
} // namespace

CommandSend CommandTracker::openSend() const {
    CommandSend send{EDGE_TYPE_SR_OPEN, std::vector<uint8_t>(8), session_, 0, open_sends_};
    edge_u64_be_write(send.payload.data(), session_);
    return send;
}

CommandSend CommandTracker::dataSend(uint8_t seq, const Entry& entry) const {
    CommandSend send{EDGE_TYPE_SR_COMMAND, std::vector<uint8_t>(10), session_, seq, entry.sends};
    edge_u64_be_write(send.payload.data(), session_);
    send.payload[8] = seq;
    send.payload[9] = entry.type;
    send.payload.insert(send.payload.end(), entry.args.begin(), entry.args.end());
    return send;
}

CommandSend CommandTracker::resultAck(uint8_t seq) const {
    CommandSend send{EDGE_TYPE_SR_RESULT_ACK, std::vector<uint8_t>(9), session_, seq, 0};
    edge_u64_be_write(send.payload.data(), session_);
    send.payload[8] = seq;
    return send;
}

void CommandTracker::begin(uint64_t session, TimePoint now, TrackerActions& actions) {
    // 不允许回到用过的会话，包括串口重开和系统时间回拨。
    if (session <= session_) {
        if (session_ == std::numeric_limits<uint64_t>::max()) {
            state_ = State::Failed;
            return;
        }
        session = session_ + 1;
    }
    session_ = session;
    base_ = next_ = 0;
    received_.fill(false);
    completed_.fill(false);
    types_.fill(0);
    opening_at_ = now;
    open_sends_ = 0;
    open_pending_ = true;
    state_ = State::Opening;
    actions.send.push_back(openSend());
}

TrackerActions CommandTracker::start(uint64_t session, TimePoint now) {
    TrackerActions actions;
    for (const auto& [seq, e] : entries_)
        actions.finished.push_back({seq, e.type, {}, "session_aborted"});
    entries_.clear();
    begin(session, now, actions);
    return actions;
}

bool CommandTracker::canSubmit() const {
    return state_ == State::Ready && next_ < 256 && next_ < base_ + EDGE_SR_WINDOW_SIZE &&
           entries_.size() < EDGE_SR_WINDOW_SIZE;
}

std::optional<CommandSend> CommandTracker::submit(uint8_t type, std::vector<uint8_t> args,
                                                  TimePoint now) {
    if (!canSubmit() || args.size() > EDGE_SR_ARGS_MAX) return std::nullopt;
    const auto seq = static_cast<uint8_t>(next_++);
    auto [it, inserted] = entries_.emplace(seq, Entry{type, std::move(args), now});
    (void) inserted;
    types_[seq] = type;
    return dataSend(seq, it->second);
}

void CommandTracker::receive(uint8_t seq) {
    received_[seq] = true;
    const auto it = entries_.find(seq);
    if (it != entries_.end()) it->second.received = true;
    while (base_ < next_ && received_[base_])
        ++base_;
}

void CommandTracker::rollover(TimePoint now, TrackerActions& actions) {
    if (next_ == 256 && entries_.empty()) begin(session_, now, actions);
}

TrackerActions CommandTracker::abort(uint8_t failed_seq, const char* reason, TimePoint now) {
    TrackerActions actions;
    for (const auto& [seq, e] : entries_)
        actions.finished.push_back(
            {seq, e.type, {}, seq == failed_seq ? reason : "session_aborted"});
    entries_.clear();
    begin(session_, now, actions);
    return actions;
}

bool CommandTracker::canSend(const CommandSend& send, TimePoint now) const {
    if (send.session != session_) return false;
    if (send.type == EDGE_TYPE_SR_OPEN)
        return state_ == State::Opening && open_pending_ && send.attempt == open_sends_ &&
               now - opening_at_ < Ms(EDGE_COMMAND_LIFETIME_MS);
    if (state_ != State::Ready) return false;
    if (send.type == EDGE_TYPE_SR_RESULT_ACK) return completed_[send.seq];
    const auto it = entries_.find(send.seq);
    return send.type == EDGE_TYPE_SR_COMMAND && it != entries_.end() && it->second.pending &&
           !it->second.received && send.attempt == it->second.sends &&
           now - it->second.queued_at < Ms(EDGE_COMMAND_LIFETIME_MS);
}

TrackerActions CommandTracker::onWritten(const CommandSend& send, bool success, TimePoint now) {
    if (send.session != session_) return {};
    if (send.type == EDGE_TYPE_SR_OPEN) {
        if (state_ != State::Opening || !open_pending_ || send.attempt != open_sends_) return {};
        open_pending_ = false;
        if (!success || now - opening_at_ >= Ms(EDGE_COMMAND_LIFETIME_MS)) {
            state_ = State::Failed;
            return {};
        }
        ++open_sends_;
        open_sent_at_ = now;
        return {};
    }
    if (state_ != State::Ready || send.type != EDGE_TYPE_SR_COMMAND) return {};
    auto it = entries_.find(send.seq);
    if (it == entries_.end() || !it->second.pending || it->second.received ||
        send.attempt != it->second.sends)
        return {};
    auto& e = it->second;
    if (now - e.queued_at >= Ms(EDGE_COMMAND_LIFETIME_MS)) return abort(send.seq, "timeout", now);
    if (!success) return abort(send.seq, "send_failed", now);
    e.pending = false;
    ++e.sends;
    e.sent_at = now;
    return {};
}

TrackerActions CommandTracker::onFrame(uint8_t type, const std::vector<uint8_t>& p, TimePoint now) {
    if (p.size() < 8 || p.size() > EDGE_PAYLOAD_MAX || edge_u64_be_read(p.data()) != session_)
        return {};
    TrackerActions actions;
    if (type == EDGE_TYPE_SR_OPEN_ACK) {
        if (p.size() != 17 || state_ != State::Opening || open_sends_ == 0) return {};
        if (now - opening_at_ >= Ms(EDGE_COMMAND_LIFETIME_MS)) {
            state_ = State::Failed;
            return {};
        }
        const auto high_water = edge_u64_be_read(p.data() + 8);
        if (p[16] == EDGE_RC_OK && high_water == session_) {
            state_ = State::Ready;
            open_pending_ = false;
        } else if (p[16] == EDGE_RC_BAD_PARAM && high_water >= session_) {
            // 拒绝的 OPEN 也消耗握手预算；不能被连续拒绝拖成无限重试。
            if (high_water == std::numeric_limits<uint64_t>::max() ||
                open_sends_ > EDGE_MAX_RETRY) {
                state_ = State::Failed;
            } else {
                session_ = high_water + 1;
                open_pending_ = true;
                actions.send.push_back(openSend());
            }
        }
        return actions;
    }
    if (state_ != State::Ready || p.size() < 9) return {};
    const uint8_t seq = p[8];
    if (seq >= next_) return {};
    const auto it = entries_.find(seq);
    if (type == EDGE_TYPE_SR_RECEIVED) {
        if (p.size() == 9 && it != entries_.end() && it->second.sends != 0) receive(seq);
        return {};
    }
    if (type != EDGE_TYPE_SR_RESULT || p.size() < 11 || p[9] != types_[seq] ||
        !resultValid(p[9], p))
        return {};
    if (completed_[seq]) {
        actions.send.push_back(resultAck(seq));
        return actions;
    }
    if (it == entries_.end() || it->second.sends == 0) return {};
    receive(seq); // 执行结果同时证明接收成功，允许 RECEIVED 自身丢失。
    completed_[seq] = true;
    actions.finished.push_back({seq, it->second.type, {p.begin() + 10, p.end()}, {}});
    entries_.erase(it);
    actions.send.push_back(resultAck(seq));
    rollover(now, actions);
    return actions;
}

TrackerActions CommandTracker::tick(TimePoint now) {
    TrackerActions actions;
    if (state_ == State::Opening) {
        if (now - opening_at_ >= Ms(EDGE_COMMAND_LIFETIME_MS)) {
            state_ = State::Failed;
        } else if (!open_pending_ && now - open_sent_at_ >= Ms(EDGE_ACK_TIMEOUT_MS)) {
            if (open_sends_ > EDGE_MAX_RETRY)
                state_ = State::Failed;
            else {
                open_pending_ = true;
                actions.send.push_back(openSend());
            }
        }
        return actions;
    }
    if (state_ != State::Ready) return actions;
    // 先找不可恢复的缺口，避免同一轮先排入旧会话重试、随后才取消会话。
    for (const auto& [seq, e] : entries_) {
        if ((!e.received && now - e.queued_at >= Ms(EDGE_COMMAND_LIFETIME_MS)) ||
            now - e.queued_at >= Ms(EDGE_SR_RESULT_TIMEOUT_MS) ||
            (!e.received && !e.pending && e.sends > EDGE_MAX_RETRY &&
             now - e.sent_at >= Ms(EDGE_ACK_TIMEOUT_MS)))
            return abort(seq, "timeout", now);
    }
    for (auto& [seq, e] : entries_) {
        if (!e.received && !e.pending && now - e.sent_at >= Ms(EDGE_ACK_TIMEOUT_MS)) {
            e.pending = true;
            actions.send.push_back(dataSend(seq, e));
        }
    }
    return actions;
}

} // namespace gateway
