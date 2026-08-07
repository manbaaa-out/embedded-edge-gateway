#ifndef EDGE_FRAME_H
#define EDGE_FRAME_H

/*
 * 帧编解码:组帧(发送侧)与逐字节收帧 FSM(接收侧),互为逆运算。
 *
 * 结构取定长数组、无堆分配,MCU 与 Linux 通用;网关侧在其上包一层 C++ RAII 即可
 * 拿回 vector / std::function 的接口手感。
 *
 * 本层不做 I/O:解析错误只累加到 edge_parser_stats_t,呈现方式由调用方决定 ——
 * 网关走 LOG_WARN,节点可塞进 0x04 状态帧上报。
 */

#include "edge_proto/edge_proto.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 组帧(发送侧) ---- */

/* 把 type + payload 组装成完整帧写入 out,返回写入的字节数;参数非法返回 0。
 *
 * 前置条件:out 容量 >= EDGE_FRAME_MAX(调用方用定长数组 + 静态断言钉死,
 *   不为此增设一个运行期参数去比较两个编译期已知的数)。
 *   payload 可为 NULL,当且仅当 payload_len == 0;payload_len > EDGE_PAYLOAD_MAX 视为非法。
 *
 * 本函数不分配 seq:seq 属于请求-响应模型(§6.2),归上层命令管理,
 *   重发必须复用原 seq,这个决定权不能落在组帧函数手里。 */
uint8_t edge_frame_encode(uint8_t type, const uint8_t* payload, uint8_t payload_len, uint8_t* out);

/* ---- 收帧 FSM(接收侧) ---- */

/* 8 状态,与 docs/protocol.md §5.1 的错误处理对照表一一对应 */
typedef enum {
    EDGE_ST_WAIT_HDR0 = 0,
    EDGE_ST_WAIT_HDR1,
    EDGE_ST_WAIT_LEN,
    EDGE_ST_WAIT_TYPE,
    EDGE_ST_READ_PAYLOAD,
    EDGE_ST_WAIT_CRC_LO,
    EDGE_ST_WAIT_CRC_HI,
    EDGE_ST_DELIVER /* 瞬态:交付后立即 resync,正常不会停留于此 */
} edge_state_t;

/* 解析统计。协议层只计数不打印 */
typedef struct {
    uint32_t frames_ok; /* 交付成功的帧数 */
    uint32_t len_err;   /* LEN 越界(§5.3) */
    uint32_t crc_err;   /* CRC 不匹配 */
    uint32_t resync;    /* 异常路径回到起点的次数(含 DELIVER 态收到意外字节) */
} edge_parser_stats_t;

/* 收齐一帧且 CRC 校验通过时回调。
 * user 透传调用方上下文:节点侧传 NULL 即可,网关侧靠它把 C 回调弹回 C++ 对象,
 * 避免在协议层出现全局变量。 */
typedef void (*edge_frame_cb_t)(uint8_t type, const uint8_t* payload, uint8_t payload_len,
                                void* user);

typedef struct {
    edge_state_t state;
    uint8_t len;  /* 本帧 LEN */
    uint8_t type; /* 本帧 TYPE */
    uint8_t payload[EDGE_PAYLOAD_MAX];
    uint8_t received; /* payload 已收字节数 */
    uint16_t crc;     /* CRC 累加值 */
    uint8_t crc_lo;
    uint8_t crc_hi;

    edge_frame_cb_t on_frame;
    void* user;
    edge_parser_stats_t stats;
} edge_parser_t;

/* 置初态并装载回调。cb 可为 NULL,此时只统计不交付(如做链路探测) */
void edge_parser_init(edge_parser_t* p, edge_frame_cb_t cb, void* user);

/* 喂一个字节,驱动一次状态转移 */
void edge_parser_feed(edge_parser_t* p, uint8_t byte);

/* 喂一段字节,等价于逐字节调用 edge_parser_feed */
void edge_parser_feed_buf(edge_parser_t* p, const uint8_t* buf, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_FRAME_H */
