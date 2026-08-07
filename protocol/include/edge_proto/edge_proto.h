#ifndef EDGE_PROTO_H
#define EDGE_PROTO_H

/*
 * 网关与 STM32 传感节点之间串口协议的权威定义。
 *
 * 本文件是协议的单一真相,两端编译同一份副本。docs/protocol.md 是其散文版说明,
 * 两者不一致时以本文件为准。协议版本 v1.2,变更记录见 docs/protocol.md §9。
 *
 * 可移植性契约(由 CI 的 arm-none-eabi 编译 job 保证):
 *   - 纯 C99,不含 malloc / stdio / 平台头,可直接编入 STM32 固件
 *   - 不做 I/O:错误只计数不打印,呈现方式由调用方决定
 *   - 所有缓冲区由调用方提供
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 协议版本 ---- */
#define EDGE_PROTO_VERSION_MAJOR 1
#define EDGE_PROTO_VERSION_MINOR 2

/*
 * 帧布局(docs/protocol.md §2、§3.2)
 *
 *   +------+------+-----+------+--------------+--------+--------+
 *   | 0xAA | 0x55 | LEN | TYPE | payload      | CRC_LO | CRC_HI |
 *   +------+------+-----+------+--------------+--------+--------+
 *                  └────── CRC16 覆盖范围 ────┘   (小端写入)
 *
 *   LEN = 1(TYPE) + payload 长度
 */
#define EDGE_HDR0 0xAAu /* 两字节帧头把帧头假阳性率从 1/256 降到 1/65536 */
#define EDGE_HDR1 0x55u

#define EDGE_LEN_MIN 1u  /* 只有 TYPE、无 payload(心跳帧) */
#define EDGE_LEN_MAX 64u /* 业务最大 33B 取 2 倍余量向上取 2 的幂(§3.2) */

#define EDGE_PAYLOAD_MAX (EDGE_LEN_MAX - 1u)         /* 63:LEN 含 TYPE 一字节 */
#define EDGE_FRAME_MIN (2u + 1u + 1u + 2u)           /* 6:心跳帧整帧长度 */
#define EDGE_FRAME_MAX (2u + 1u + EDGE_LEN_MAX + 2u) /* 69 */

/*
 * TYPE 字典(§3.3)
 *
 * 上行段(0x01~0x1F)与下行段(0x20~0x2F)数值不相交。两端共用同一套解析代码,
 * 而 Frame{type,payload} 不携带方向信息;分段后错帧无法伪装成合法对向帧,
 * 「不在字典即丢弃」得以成立。新增 TYPE 必须落在对应段内。
 */
typedef enum {
    /* ---- 上行:STM32 → 网关 ---- */
    EDGE_TYPE_DHT11 = 0x01,      /* 温湿度:温 2B + 湿 2B + 校验 1B(均 ×10 大端) */
    EDGE_TYPE_BH1750 = 0x02,     /* 光照:  2B 大端 uint16,单位 lux */
    EDGE_TYPE_HEARTBEAT = 0x03,  /* 心跳:  无 payload */
    EDGE_TYPE_STATUS = 0x04,     /* 设备状态:1B bitmask,见 EDGE_STATUS_BIT_* */
    EDGE_TYPE_QUERY_RESP = 0x05, /* 查询应答:[seq][rc][数据…] */
    EDGE_TYPE_ACK = 0x06,        /* 命令确认:[seq][rc] */

    /* ---- 下行:网关 → STM32 ---- */
    EDGE_TYPE_QUERY_LIGHT = 0x20, /* 查光照:  [seq] */
    EDGE_TYPE_QUERY_TH = 0x21,    /* 查温湿度:[seq] */
    EDGE_TYPE_SET_PERIOD = 0x22   /* 设采样周期:[seq][周期 2B 大端,单位秒] */
} edge_type_t;

/* 全 0 / 全 1 最易因空线、短路被误读,保留为非法值用作异常检测 */
#define EDGE_TYPE_INVALID_LO 0x00u
#define EDGE_TYPE_INVALID_HI 0xFFu

/* 方向判定。节点据此对上行 TYPE 回 EDGE_RC_UNSUPPORTED,而非漏到 switch 的 default */
#define EDGE_IS_UPLINK(t) ((uint8_t) (t) >= 0x01u && (uint8_t) (t) <= 0x1Fu)
#define EDGE_IS_DOWNLINK(t) ((uint8_t) (t) >= 0x20u && (uint8_t) (t) <= 0x2Fu)

/*
 * 结果码(§6.3)
 *
 * 节点收到无法成功执行的命令必须回带对应结果码的应答,不得静默丢弃或假装成功。
 */
typedef enum {
    EDGE_RC_OK = 0x00,          /* 成功 */
    EDGE_RC_BAD_PARAM = 0x01,   /* 参数非法 / 超范围(如周期 = 0) */
    EDGE_RC_UNSUPPORTED = 0x02, /* TYPE 不在下行字典(含收到上行 TYPE) */
    EDGE_RC_BUSY = 0x03 /* 设备忙 / 暂时无法执行(如传感器读失败),可稍后重试 */
} edge_rc_t;

/* ---- payload 布局与业务量纲 ---- */

/* 下行命令与上行应答的 payload 首字节恒为 seq(§6.2),应答第二字节为结果码 */
#define EDGE_OFF_SEQ 0u
#define EDGE_OFF_RC 1u

/* 采样周期(0x22)。单位秒 —— 单位写进标识符名以杜绝二义(v1.1 曾误记为 ms)。
 * uint16 秒 → 上限约 18.2 小时。 */
#define EDGE_PERIOD_MIN_S 1u
#define EDGE_PERIOD_MAX_S 65535u

/* 温湿度(0x01 / 0x05 回 0x21)定点传输:实际值 ×10 取整成 uint16 大端。
 * 25.3℃ → 253 → 0x00FD。MCU 无 FPU,且 ±0.05℃ 的量化误差远小于传感器本身精度。 */
#define EDGE_TEMP_SCALE 10
#define EDGE_HUMI_SCALE 10

/* 设备状态(0x04)bitmask。按位标志而非连续枚举,接收端必须逐位 AND */
#define EDGE_STATUS_BIT_DHT11 0x01u
#define EDGE_STATUS_BIT_BH1750 0x02u

/* ---- 请求-响应契约(§6.5)。协议层只定契约,定时器与重试的实现由各端自行决定 ---- */
#define EDGE_ACK_TIMEOUT_MS 500u /* 节点收到合法命令后的应答时限 */
#define EDGE_MAX_RETRY 3u /* 网关对单条命令的建议最大重试次数(重发复用同 seq) */

/* ---- 共享工具函数(header-only,两端零成本复用) ---- */

/* 按大端读取 p 指向的 uint16。payload 内多字节业务字段一律大端(§3.4)。
 * 强转不可省:uint8_t 会先整型提升到 int,16 位 int 平台上左移 8 位即溢出。 */
static inline uint16_t edge_u16_be_read(const uint8_t* p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

/* 按大端把 v 写入 p 指向的两字节 */
static inline void edge_u16_be_write(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) (v & 0xFFu);
}

/* 返回该 TYPE 要求的最小 payload 字节数;未知 TYPE 返回 -1。
 * 0x05 是变长应答,故只约束下限(seq + rc)。 */
static inline int edge_min_payload_len(uint8_t type) {
    switch (type) {
    case EDGE_TYPE_DHT11:
        return 5; /* 温 2B + 湿 2B + 校验 1B */
    case EDGE_TYPE_BH1750:
        return 2;
    case EDGE_TYPE_HEARTBEAT:
        return 0;
    case EDGE_TYPE_STATUS:
        return 1;
    case EDGE_TYPE_QUERY_RESP:
        return 2; /* [seq][rc],数据段变长 */
    case EDGE_TYPE_ACK:
        return 2; /* [seq][rc] */
    case EDGE_TYPE_QUERY_LIGHT:
        return 1; /* [seq] */
    case EDGE_TYPE_QUERY_TH:
        return 1; /* [seq] */
    case EDGE_TYPE_SET_PERIOD:
        return 3; /* [seq][周期 2B] */
    default:
        return -1;
    }
}

/* 判断 payload 长度是否满足该 TYPE 的最小要求;未知 TYPE 一律返回 false */
static inline int edge_payload_len_ok(uint8_t type, uint8_t payload_len) {
    int need = edge_min_payload_len(type);
    return (need >= 0) && ((int) payload_len >= need);
}

/* 判断采样周期是否合法(§6.3:周期 = 0 → EDGE_RC_BAD_PARAM) */
static inline int edge_period_s_valid(uint16_t period_s) {
    return period_s >= EDGE_PERIOD_MIN_S;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_PROTO_H */
