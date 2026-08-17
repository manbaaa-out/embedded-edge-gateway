#ifndef EDGE_PROTO_H
#define EDGE_PROTO_H

/**
 * @file edge_proto.h
 * @brief 网关与 STM32 共用的线上协议定义。
 *
 * 本文件集中定义双方必须一致的字节布局、量纲、类型区间和请求时序默认值。
 * 内容保持 C99、无动态分配、无平台 I/O；多字节业务字段统一采用大端，帧尾 CRC
 * 单独采用低字节在前的 MODBUS 表示。协议文档应解释这些定义，而不另建一套常量。
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 不兼容的线上格式变更时递增。 */
#define EDGE_PROTO_VERSION_MAJOR 1
/** 向后兼容的协议能力扩展时递增。 */
#define EDGE_PROTO_VERSION_MINOR 3

/*
 * 帧布局：
 *
 *   +------+------+-----+------+--------------+--------+--------+
 *   | 0xAA | 0x55 | LEN | TYPE | payload      | CRC_LO | CRC_HI |
 *   +------+------+-----+------+--------------+--------+--------+
 *                  └────── CRC16 覆盖范围 ────┘   （CRC 低字节在前）
 *
 *   LEN = 1(TYPE) + payload 长度
 */
#define EDGE_HDR0 0xAAu /**< 两字节帧头的首字节。 */
#define EDGE_HDR1 0x55u /**< 两字节帧头的次字节。 */

#define EDGE_LEN_MIN 1u  /**< LEN 最小值：只有一个 TYPE 字节。 */
#define EDGE_LEN_MAX 64u /**< LEN 最大值：TYPE 与 payload 合计最多 64 字节。 */

#define EDGE_PAYLOAD_MAX (EDGE_LEN_MAX - 1u)         /**< payload 最大 63 字节。 */
#define EDGE_FRAME_MIN (2u + 1u + 1u + 2u)           /**< 无 payload 帧共 6 字节。 */
#define EDGE_FRAME_MAX (2u + 1u + EDGE_LEN_MAX + 2u) /**< 最大完整帧共 69 字节。 */

/**
 * 消息类型按方向划分为互不重叠的数值区间。解析器只恢复原始 type，业务层再结合
 * 接收方向判断是否合法；新增消息必须使用对应方向的保留区间。
 */
typedef enum {
    /* STM32 -> gateway */
    EDGE_TYPE_DHT11 = 0x01,      /* [temperature:u16be][humidity:u16be]，均按 0.1 缩放。 */
    EDGE_TYPE_BH1750 = 0x02,     /* [lux:u16be] */
    EDGE_TYPE_HEARTBEAT = 0x03,  /* 无 payload。 */
    EDGE_TYPE_STATUS = 0x04,     /* [sensor_status:bitmask] */
    EDGE_TYPE_QUERY_RESP = 0x05, /* [seq][rc][data...] */
    EDGE_TYPE_ACK = 0x06,        /* [seq][rc] */

    /* gateway -> STM32 */
    EDGE_TYPE_QUERY_LIGHT = 0x20, /* [seq] */
    EDGE_TYPE_QUERY_TH = 0x21,    /* [seq] */
    EDGE_TYPE_SET_PERIOD = 0x22   /* [seq][period_s:u16be] */
} edge_type_t;

/** 低端保留值，不属于任一传输方向。 */
#define EDGE_TYPE_INVALID_LO 0x00u
/** 高端保留值，不属于任一传输方向。 */
#define EDGE_TYPE_INVALID_HI 0xFFu

/** 判断原始类型值是否位于 STM32 到网关的上行保留区间。 */
#define EDGE_IS_UPLINK(t) ((uint8_t) (t) >= 0x01u && (uint8_t) (t) <= 0x1Fu)
/** 判断原始类型值是否位于网关到 STM32 的下行保留区间。 */
#define EDGE_IS_DOWNLINK(t) ((uint8_t) (t) >= 0x20u && (uint8_t) (t) <= 0x2Fu)

/** 节点对下行命令给出的业务执行结果。收到任何结果都结束该次链路层等待。 */
typedef enum {
    EDGE_RC_OK = 0x00,          /**< 命令已成功执行。 */
    EDGE_RC_BAD_PARAM = 0x01,   /**< payload 缺失、长度错误或参数超出合法范围。 */
    EDGE_RC_UNSUPPORTED = 0x02, /**< 节点不支持该 TYPE 或接收方向错误。 */
    EDGE_RC_BUSY = 0x03         /**< 节点暂时无法完成，可由上层决定是否稍后重试。 */
} edge_rc_t;

/** 下行命令和对应上行应答中 seq 的字节偏移。 */
#define EDGE_OFF_SEQ 0u
/** ACK/QUERY_RESP 中结果码的字节偏移。 */
#define EDGE_OFF_RC 1u

/** SET_PERIOD 可接受的最小周期，单位秒。 */
#define EDGE_PERIOD_MIN_S 1u
/** SET_PERIOD 可接受的最大周期，单位秒；等于 uint16_t 上限。 */
#define EDGE_PERIOD_MAX_S 65535u

/** 温度无符号定点值相对摄氏度的倍率，例如 253 表示 25.3 ℃。 */
#define EDGE_TEMP_SCALE 10
/** 湿度无符号定点值相对百分比的倍率，例如 618 表示 61.8%。 */
#define EDGE_HUMI_SCALE 10

/** STATUS bitmask 中 DHT11 可用标志。 */
#define EDGE_STATUS_BIT_DHT11 0x01u
/** STATUS bitmask 中 BH1750 可用标志。 */
#define EDGE_STATUS_BIT_BH1750 0x02u

/** 网关发送后等待 ACK/QUERY_RESP 的默认时限，单位毫秒。 */
#define EDGE_ACK_TIMEOUT_MS 500u
/** 首次发送之外允许的最大重发次数；每次重发复用原 seq。 */
#define EDGE_MAX_RETRY 3u

/**
 * @brief 从 payload 读取一个大端 uint16_t。
 * @param p 指向至少两个可读字节的非空指针。
 * @return 由 p[0] 作为高字节、p[1] 作为低字节组成的值。
 */
static inline uint16_t edge_u16_be_read(const uint8_t* p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

/**
 * @brief 将 uint16_t 写成 payload 使用的大端字节序。
 * @param p 指向至少两个可写字节的非空指针。
 * @param v 待编码的无符号 16 位值。
 */
static inline void edge_u16_be_write(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) (v & 0xFFu);
}

/**
 * @brief 查询消息类型所需的最小 payload 长度。
 * @param type 待查询的原始 TYPE。
 * @return 已知类型的最小字节数；未知或保留类型返回 -1。
 *
 * QUERY_RESP 的 data 部分可变，因此只要求固定的 seq 与 rc 两字节。
 */
static inline int edge_min_payload_len(uint8_t type) {
    switch (type) {
    case EDGE_TYPE_DHT11:
        return 4;
    case EDGE_TYPE_BH1750:
        return 2;
    case EDGE_TYPE_HEARTBEAT:
        return 0;
    case EDGE_TYPE_STATUS:
        return 1;
    case EDGE_TYPE_QUERY_RESP:
        return 2;
    case EDGE_TYPE_ACK:
        return 2;
    case EDGE_TYPE_QUERY_LIGHT:
        return 1;
    case EDGE_TYPE_QUERY_TH:
        return 1;
    case EDGE_TYPE_SET_PERIOD:
        return 3;
    default:
        return -1;
    }
}

/**
 * @brief 判断 payload 长度是否满足已知消息类型的下限。
 * @param type 原始 TYPE。
 * @param payload_len 实际 payload 字节数。
 * @return 满足下限时返回非零；未知类型或长度不足时返回 0。
 */
static inline int edge_payload_len_ok(uint8_t type, uint8_t payload_len) {
    int need = edge_min_payload_len(type); /* 负值同时表示 type 未定义。 */
    return (need >= 0) && ((int) payload_len >= need);
}

/**
 * @brief 验证 SET_PERIOD 的秒数。
 * @param period_s 线上 uint16_t 周期，单位秒。
 * @return 位于 [EDGE_PERIOD_MIN_S, EDGE_PERIOD_MAX_S] 时返回非零。
 */
static inline int edge_period_s_valid(uint16_t period_s) {
    return period_s >= EDGE_PERIOD_MIN_S;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_PROTO_H */
