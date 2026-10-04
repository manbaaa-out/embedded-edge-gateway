#include "edge_proto/edge_sr.h"

static void clear_entries(edge_sr_node_t* node) {
    unsigned i;
    for (i = 0; i < EDGE_SR_RESULT_CAPACITY; ++i)
        node->entries[i].state = 0;
}

void edge_sr_node_init(edge_sr_node_t* node) {
    node->session = 0;
    node->base = 0;
    node->active = 0;
    clear_entries(node);
}

static edge_sr_entry_t* find_entry(edge_sr_node_t* node, uint8_t seq) {
    unsigned i;
    for (i = 0; i < EDGE_SR_RESULT_CAPACITY; ++i) {
        edge_sr_entry_t* e = &node->entries[i];
        if (e->state != 0u && e->seq == seq) return e;
    }
    return NULL;
}

static void emit_received(const edge_sr_node_t* node, uint8_t seq, const edge_sr_hooks_t* hooks,
                          void* user) {
    uint8_t p[9];
    edge_u64_be_write(p, node->session);
    p[8] = seq;
    hooks->emit(EDGE_TYPE_SR_RECEIVED, p, (uint8_t) sizeof p, user);
}

static void emit_result(const edge_sr_node_t* node, const edge_sr_entry_t* e,
                        const edge_sr_hooks_t* hooks, void* user) {
    uint8_t p[EDGE_PAYLOAD_MAX];
    unsigned i;
    edge_u64_be_write(p, node->session);
    p[8] = e->seq;
    p[9] = e->command;
    for (i = 0; i < e->result_len; ++i)
        p[10u + i] = e->result[i];
    hooks->emit(EDGE_TYPE_SR_RESULT, p, (uint8_t) (10u + e->result_len), user);
}

/* 有任何超时缺口就关闭整会话；不能跳过缺口执行后面的命令。 */
static void expire(edge_sr_node_t* node, uint32_t now) {
    unsigned i;
    const int gap = node->base < 256u && find_entry(node, (uint8_t) node->base) == NULL;
    for (i = 0; i < EDGE_SR_RESULT_CAPACITY; ++i) {
        edge_sr_entry_t* e = &node->entries[i];
        if (gap && e->state == 1u &&
            (uint32_t) (now - e->received_at) >= EDGE_COMMAND_LIFETIME_MS) {
            node->active = 0;
            clear_entries(node);
            return;
        }
        if (e->state == 2u && (uint32_t) (now - e->completed_at) >= EDGE_SR_RESULT_TTL_MS)
            e->state = 0;
    }
}

static void deliver(edge_sr_node_t* node, const edge_sr_hooks_t* hooks, void* user) {
    while (node->base < 256u) {
        edge_sr_entry_t* e;
        expire(node, hooks->now_ms(user));
        if (node->active == 0u) return;
        e = find_entry(node, (uint8_t) node->base);
        if (e == NULL || e->state != 1u) return;
        e->result_len = hooks->execute(e->seq, e->command, e->args, e->args_len, e->result, user);
        if (e->result_len == 0u || e->result_len > EDGE_SR_RESULT_MAX) {
            e->result[0] = EDGE_RC_UNSUPPORTED;
            e->result_len = 1;
        }
        e->completed_at = hooks->now_ms(user);
        e->last_sent_at = e->completed_at;
        e->retries = 0;
        e->state = 2;
        ++node->base;
        /* 先存结果、推进窗口，再尝试发送；TX 队列满也不会重复执行业务。 */
        emit_result(node, e, hooks, user);
    }
}

void edge_sr_node_on_frame(edge_sr_node_t* node, uint8_t type, const uint8_t* payload, uint8_t len,
                           const edge_sr_hooks_t* hooks, void* user) {
    uint64_t session;
    uint8_t seq;
    edge_sr_entry_t* e;
    unsigned i;
    uint32_t now = hooks->now_ms(user);
    expire(node, now);
    if (payload == NULL || len > EDGE_PAYLOAD_MAX || len < 8u) return;
    session = edge_u64_be_read(payload);
    if (type == EDGE_TYPE_SR_OPEN) {
        uint8_t p[17];
        uint8_t rc = EDGE_RC_BAD_PARAM;
        if (len != 8u) return;
        if (session > node->session) {
            clear_entries(node);
            node->session = session;
            node->base = 0;
            node->active = 1;
        }
        if (session == node->session && node->active != 0u) rc = EDGE_RC_OK;
        edge_u64_be_write(p, session);
        edge_u64_be_write(p + 8, node->session);
        p[16] = rc;
        hooks->emit(EDGE_TYPE_SR_OPEN_ACK, p, (uint8_t) sizeof p, user);
        return;
    }
    if (node->active == 0u || session != node->session || len < 9u) return;
    seq = payload[8];
    e = find_entry(node, seq);
    if (type == EDGE_TYPE_SR_RESULT_ACK) {
        if (len == 9u && e != NULL && e->state == 2u) e->state = 0;
        return;
    }
    if (type != EDGE_TYPE_SR_COMMAND || len < 10u) return;
    if (e != NULL) {
        /* 相同 (session, seq) 不允许改变业务内容。 */
        if (e->command != payload[9] || e->args_len != len - 10u) return;
        for (i = 0; i < e->args_len; ++i)
            if (e->args[i] != payload[10u + i]) return;
        emit_received(node, seq, hooks, user);
        if (e->state == 2u) emit_result(node, e, hooks, user);
        return;
    }
    if (seq < node->base) {
        /* 即使结果缓存已经释放，窗口左侧的旧序号也永远不会再执行。 */
        emit_received(node, seq, hooks, user);
        return;
    }
    if ((uint16_t) seq >= node->base + EDGE_SR_WINDOW_SIZE) return;
    for (i = 0; i < EDGE_SR_RESULT_CAPACITY; ++i) {
        if (node->entries[i].state == 0u) {
            e = &node->entries[i];
            break;
        }
    }
    if (e == NULL) return; /* 不 ACK 未保存的命令，由发送方重试或关闭会话。 */
    e->state = 1;
    e->seq = seq;
    e->command = payload[9];
    e->args_len = (uint8_t) (len - 10u);
    e->received_at = now;
    for (i = 0; i < e->args_len; ++i)
        e->args[i] = payload[10u + i];
    emit_received(node, seq, hooks, user);
    deliver(node, hooks, user);
}

void edge_sr_node_tick(edge_sr_node_t* node, const edge_sr_hooks_t* hooks, void* user) {
    unsigned i;
    uint32_t now = hooks->now_ms(user);
    expire(node, now);
    if (node->active == 0u) return;
    for (i = 0; i < EDGE_SR_RESULT_CAPACITY; ++i) {
        edge_sr_entry_t* e = &node->entries[i];
        if (e->state == 2u && e->retries < EDGE_MAX_RETRY &&
            (uint32_t) (now - e->last_sent_at) >= EDGE_ACK_TIMEOUT_MS) {
            ++e->retries;
            e->last_sent_at = now;
            emit_result(node, e, hooks, user);
        }
    }
}
