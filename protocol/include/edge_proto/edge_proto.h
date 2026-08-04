#ifndef EDGE_PROTO_H
#define EDGE_PROTO_H

/* ============================================================================
 * edge_proto —— 边缘网关 ⇄ STM32 传感节点 串口协议契约【单一真相】
 *
 * 【本文件的地位】
 *   本头文件是协议的唯一权威定义。docs/protocol.md 是它的散文版说明,
 *   两者若有出入,以本文件为准 —— 因为只有本文件会被编译器检查、被两端共用。
 *
 *   历史教训:TYPE 与结果码曾经以裸数字的形式散落在网关 GatewayApp.cpp、
 *   节点 cmd_service.c、模拟器 fake_stm32.cpp 三处,靠人肉对齐。结果是
 *   「0x22 采样周期的单位」在文档写 ms、两端代码写秒、README 写秒,
 *   四处三种说法而无人发觉。本文件的存在就是为了让这类漂移不可能再发生。
 *
 * 【可移植性契约】纯 C99,零依赖:
 *   - 不含 malloc / stdio / 平台头,可直接被 arm-none-eabi-gcc 编进固件
 *   - 不做任何 I/O(错误不打印,只计数,由调用方决定怎么呈现)
 *   - 所有缓冲区由调用方提供
 *   CI 里有一个 job 专门用 arm-none-eabi-gcc 编译本目录,用编译器保证这条契约。
 *
 * 协议版本 v1.2 —— 变更见 docs/protocol.md §9
 * ========================================================================= */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. 协议版本
 * ============================================================ */
#define EDGE_PROTO_VERSION_MAJOR 1
#define EDGE_PROTO_VERSION_MINOR 2

/* ============================================================
 * 2. 帧布局常量(docs/protocol.md §2、§3.2)
 *
 *   +------+------+-----+------+--------------+--------+--------+
 *   | 0xAA | 0x55 | LEN | TYPE | payload      | CRC_LO | CRC_HI |
 *   +------+------+-----+------+--------------+--------+--------+
 *                  └────── CRC16 覆盖范围 ────┘   (小端写入)
 *
 *   LEN = 1(TYPE) + payload 长度
 * ============================================================ */
#define EDGE_HDR0 0xAAu /* 帧头 1:10101010,位变化频繁,利于 UART 时钟同步 */
#define EDGE_HDR1 0x55u /* 帧头 2:01010101。两字节帧头把假阳性率从 1/256 降到 1/65536 */

#define EDGE_LEN_MIN 1u  /* LEN 下限:只有 TYPE、无 payload(心跳帧) */
#define EDGE_LEN_MAX 64u /* LEN 上限:业务最大 33B 取 2 倍余量向上取 2 的幂(§3.2) */

#define EDGE_PAYLOAD_MAX (EDGE_LEN_MAX - 1u)      /* 63:LEN 含 TYPE 一字节 */
#define EDGE_FRAME_MIN   (2u + 1u + 1u + 2u)      /* 6:心跳帧整帧长度 */
#define EDGE_FRAME_MAX   (2u + 1u + EDGE_LEN_MAX + 2u) /* 69 */

/* ============================================================
 * 3. TYPE 字典(§3.3)
 *
 * 【铁律】上行段(0x01~0x1F)与下行段(0x20~0x2F)数值不相交。
 *   物理上 TX/RX 是两根线,帧不会混流;但 TYPE 要穿过两端【同一套】解析代码,
 *   而 Frame{type,payload} 不携带方向信息。分段后,错帧无法伪装成合法对向帧,
 *   「不在字典即丢弃」自然成立。
 * ============================================================ */
typedef enum {
    /* ---- 上行:STM32 → 网关 ---- */
    EDGE_TYPE_DHT11      = 0x01, /* 温湿度:温 2B + 湿 2B + 校验 1B(均 ×10 大端) */
    EDGE_TYPE_BH1750     = 0x02, /* 光照:  2B 大端 uint16,单位 lux */
    EDGE_TYPE_HEARTBEAT  = 0x03, /* 心跳:  无 payload */
    EDGE_TYPE_STATUS     = 0x04, /* 设备状态:1B bitmask,见 EDGE_STATUS_BIT_* */
    EDGE_TYPE_QUERY_RESP = 0x05, /* 查询应答:[seq][rc][数据…] */
    EDGE_TYPE_ACK        = 0x06, /* 命令确认:[seq][rc] */

    /* ---- 下行:网关 → STM32 ---- */
    EDGE_TYPE_QUERY_LIGHT = 0x20, /* 查光照:  [seq] */
    EDGE_TYPE_QUERY_TH    = 0x21, /* 查温湿度:[seq] */
    EDGE_TYPE_SET_PERIOD  = 0x22  /* 设采样周期:[seq][周期 2B 大端,单位秒] */
} edge_type_t;

/* 0x00 与 0xFF 保留为非法:全 0 / 全 1 最易因空线、短路被误读,留作异常检测 */
#define EDGE_TYPE_INVALID_LO 0x00u
#define EDGE_TYPE_INVALID_HI 0xFFu

/* 方向判定 —— 把「上下行分段」这条口头铁律变成可调用的判断。
 * 节点收到上行 TYPE 时据此回 EDGE_RC_UNSUPPORTED,而不是靠 switch 漏网到 default。 */
#define EDGE_IS_UPLINK(t)   ((uint8_t) (t) >= 0x01u && (uint8_t) (t) <= 0x1Fu)
#define EDGE_IS_DOWNLINK(t) ((uint8_t) (t) >= 0x20u && (uint8_t) (t) <= 0x2Fu)

/* ============================================================
 * 4. 结果码字典(§6.3)
 *
 * 原则:节点收到无法成功执行的命令,绝不静默丢弃、也不假装成功,
 *       必须回一个携带对应结果码的应答,让运维据码定位。
 * ============================================================ */
typedef enum {
    EDGE_RC_OK          = 0x00, /* 成功 */
    EDGE_RC_BAD_PARAM   = 0x01, /* 参数非法 / 超范围(如周期 = 0) */
    EDGE_RC_UNSUPPORTED = 0x02, /* 不支持的命令(TYPE 不在下行字典,含收到上行 TYPE) */
    EDGE_RC_BUSY        = 0x03  /* 设备忙 / 暂时无法执行(如传感器读失败),可稍后重试 */
} edge_rc_t;

/* ============================================================
 * 5. payload 布局与业务量纲
 * ============================================================ */

/* 下行命令与上行应答的 payload 首字节恒为 seq(§6.2),应答第二字节为结果码 */
#define EDGE_OFF_SEQ 0u
#define EDGE_OFF_RC  1u

/* ---- 采样周期(0x22)----
 * 【单位:秒】。协议 v1.1 文档曾误写为 ms,而两端实现一直按秒处理;
 * v1.2 澄清为秒,并把单位写进每一个标识符的名字(_S 后缀),杜绝再次二义。
 * uint16 秒 → 上限约 18.2 小时,对采样周期绰绰有余。 */
#define EDGE_PERIOD_MIN_S 1u
#define EDGE_PERIOD_MAX_S 65535u

/* ---- 温湿度(0x01 / 0x05 回 0x21)----
 * 定点传输:实际值 ×10 后取整成 uint16 大端。25.3℃ → 253 → 0x00FD */
#define EDGE_TEMP_SCALE 10
#define EDGE_HUMI_SCALE 10

/* ---- 设备状态(0x04)bitmask ----
 * 【注意】这是按位标志,不是连续枚举。接收端必须逐位 AND,不能当单一数值解释。 */
#define EDGE_STATUS_BIT_DHT11  0x01u
#define EDGE_STATUS_BIT_BH1750 0x02u

/* ============================================================
 * 6. 请求-响应契约(§6.5)
 *
 * 协议层只定契约,不定实现:定时器怎么做、重试放在哪个线程,由各端自行决定。
 * ============================================================ */
#define EDGE_ACK_TIMEOUT_MS 500u /* 节点收到合法命令后的应答时限 */
#define EDGE_MAX_RETRY      3u   /* 网关对单条命令的建议最大重试次数(重发复用同 seq) */

/* ============================================================
 * 7. 共享工具函数(header-only,两端零成本复用)
 * ============================================================ */

/* payload 内部多字节业务字段一律【大端】(网络字节序),上下行一致(§3.4)。
 * 两端过去各自手写 (p[0] << 8) | p[1],移位与符号提升写错一次就是静默错值。 */
static inline uint16_t edge_u16_be_read(const uint8_t* p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

static inline void edge_u16_be_write(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) (v & 0xFFu);
}

/* 该 TYPE 要求的【最小】payload 字节数;未知 TYPE 返回 -1。
 * 统一两端各自散落的长度校验(网关曾写 p.size() < 5,节点曾写 len < 3),
 * 校验口径从此只有一处。0x05 是变长应答,故只约束下限(seq + rc)。 */
static inline int edge_min_payload_len(uint8_t type) {
    switch (type) {
    case EDGE_TYPE_DHT11:       return 5; /* 温 2B + 湿 2B + 校验 1B */
    case EDGE_TYPE_BH1750:      return 2;
    case EDGE_TYPE_HEARTBEAT:   return 0;
    case EDGE_TYPE_STATUS:      return 1;
    case EDGE_TYPE_QUERY_RESP:  return 2; /* [seq][rc],数据段变长 */
    case EDGE_TYPE_ACK:         return 2; /* [seq][rc] */
    case EDGE_TYPE_QUERY_LIGHT: return 1; /* [seq] */
    case EDGE_TYPE_QUERY_TH:    return 1; /* [seq] */
    case EDGE_TYPE_SET_PERIOD:  return 3; /* [seq][周期 2B] */
    default:                    return -1;
    }
}

/* payload 长度是否满足该 TYPE 的最小要求(未知 TYPE 一律 false) */
static inline int edge_payload_len_ok(uint8_t type, uint8_t payload_len) {
    int need = edge_min_payload_len(type);
    return (need >= 0) && ((int) payload_len >= need);
}

/* 采样周期合法性(§6.3:周期 = 0 → EDGE_RC_BAD_PARAM) */
static inline int edge_period_s_valid(uint16_t period_s) {
    return period_s >= EDGE_PERIOD_MIN_S;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_PROTO_H */
