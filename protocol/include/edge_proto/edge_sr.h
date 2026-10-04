#ifndef EDGE_SR_H
#define EDGE_SR_H

#include "edge_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单线程、无堆分配的 SR 接收端。执行回调必须同步返回，不能另起任务并发执行。
 * emit 必须复制 payload，不能保留栈指针，也不能重入本状态机。 */
typedef struct {
    uint32_t (*now_ms)(void* user);
    /* 返回 [rc][data...] 的长度，至少 1，最多 EDGE_SR_RESULT_MAX。 */
    uint8_t (*execute)(uint8_t seq, uint8_t command, const uint8_t* args, uint8_t len,
                       uint8_t* result, void* user);
    void (*emit)(uint8_t type, const uint8_t* payload, uint8_t len, void* user);
} edge_sr_hooks_t;

typedef struct {
    uint32_t received_at;
    uint32_t completed_at;
    uint32_t last_sent_at;
    uint8_t state; /* 0 free, 1 buffered, 2 executed awaiting result ACK */
    uint8_t seq;
    uint8_t command;
    uint8_t args_len;
    uint8_t result_len;
    uint8_t retries;
    uint8_t args[EDGE_SR_ARGS_MAX];
    uint8_t result[EDGE_SR_RESULT_MAX];
} edge_sr_entry_t;

typedef struct {
    uint64_t session; /* 本次启动期间的高水位；关闭会话时也不能清零。 */
    uint16_t base;    /* 下一个待执行序号，256 表示本会话已用完。 */
    uint8_t active;
    edge_sr_entry_t entries[EDGE_SR_RESULT_CAPACITY];
} edge_sr_node_t;

void edge_sr_node_init(edge_sr_node_t* node);
void edge_sr_node_on_frame(edge_sr_node_t* node, uint8_t type, const uint8_t* payload, uint8_t len,
                           const edge_sr_hooks_t* hooks, void* user);
/* 无流量时也必须定期调用；时间使用可回绕的 32 位毫秒单调时钟。 */
void edge_sr_node_tick(edge_sr_node_t* node, const edge_sr_hooks_t* hooks, void* user);

#ifdef __cplusplus
}
#endif
#endif
