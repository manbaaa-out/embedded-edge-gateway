#ifndef EDGE_FRAME_H
#define EDGE_FRAME_H

/**
 * @file edge_frame.h
 * @brief 无动态分配的帧编码器和增量接收状态机。
 *
 * 协议层只处理字节结构：编码由调用方提供输出缓冲区，解析由调用方逐字节驱动。
 * 它既不读写串口，也不记录日志；解析结果通过同步回调和累计计数交给上层。
 */

#include "edge_proto/edge_proto.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将消息类型和 payload 编码为完整线上帧。
 * @param type 写入 TYPE 字段的原始值；本函数不判断业务方向或类型字典。
 * @param payload payload 首字节地址；仅当 payload_len 为 0 时可为 NULL。
 *                非空 payload 不得与 out 的写入区间重叠。
 * @param payload_len payload 字节数，最大为 EDGE_PAYLOAD_MAX。
 * @param out 调用方拥有的输出缓冲区，容量必须不少于 EDGE_FRAME_MAX。
 * @return 实际帧长；out 为 NULL、payload_len 超限或非空 payload 缺失时返回 0，
 *         且不写 out。缓冲区重叠不会被运行期检测。
 *
 * seq 属于业务 payload，由命令层分配；编码器不会生成或修改它。
 */
uint8_t edge_frame_encode(uint8_t type, const uint8_t* payload, uint8_t payload_len, uint8_t* out);

/** 增量解析器下一步期望接收的字段。 */
typedef enum {
    EDGE_ST_WAIT_HDR0 = 0, /**< 搜索帧头首字节 0xAA。 */
    EDGE_ST_WAIT_HDR1,     /**< 已收到 0xAA，等待帧头次字节 0x55。 */
    EDGE_ST_WAIT_LEN,      /**< 等待并校验 LEN。 */
    EDGE_ST_WAIT_TYPE,     /**< 等待 TYPE，并根据 LEN 决定是否继续读取 payload。 */
    EDGE_ST_READ_PAYLOAD,  /**< 按 LEN 收集 payload。 */
    EDGE_ST_WAIT_CRC_LO,   /**< 等待线上 CRC 的低字节。 */
    EDGE_ST_WAIT_CRC_HI,   /**< 等待线上 CRC 的高字节。 */
    EDGE_ST_DELIVER        /**< 瞬态：校验并回调，不会跨正常 feed 调用保留。 */
} edge_state_t;

/** 从最近一次 edge_parser_init() 起累计的解析统计；超过 uint32_t 上限后回绕。 */
typedef struct {
    uint32_t frames_ok; /**< CRC 正确并进入交付路径的帧数，即使回调为空也计数。 */
    uint32_t len_err;   /**< LEN 小于 EDGE_LEN_MIN 或大于 EDGE_LEN_MAX 的次数。 */
    uint32_t crc_err;   /**< 收到的 CRC 与本地累计值不一致的次数。 */
    uint32_t resync;    /**< 显式错误使状态机回到 EDGE_ST_WAIT_HDR0 的次数。 */
} edge_parser_stats_t;

/**
 * @brief CRC 校验通过后的同步回调类型。
 * @param type 已解析的 TYPE 原始值。
 * @param payload 解析器内部缓冲区，只在本次回调返回前有效。
 * @param payload_len payload 字节数，可为 0。
 * @param user 初始化解析器时登记的调用方上下文，不由协议层解释或释放。
 */
typedef void (*edge_frame_cb_t)(uint8_t type, const uint8_t* payload, uint8_t payload_len,
                                void* user);

/** 增量解析器的全部可持久状态；调用前必须通过 edge_parser_init() 初始化。 */
typedef struct {
    edge_state_t state; /**< 当前接收状态。 */
    uint8_t len;        /**< 当前帧的 LEN，包含 TYPE、不包含帧头和 CRC。 */
    uint8_t type;       /**< 当前帧的 TYPE 原始值。 */
    uint8_t payload[EDGE_PAYLOAD_MAX]; /**< 当前帧 payload 的固定容量存储。 */
    uint8_t received;   /**< 已写入 payload 的字节数。 */
    uint16_t crc;       /**< 从 LEN 起逐字节累计的本地 CRC。 */
    uint8_t crc_lo;     /**< 线上帧尾收到的 CRC 低字节。 */
    uint8_t crc_hi;     /**< 线上帧尾收到的 CRC 高字节。 */

    edge_frame_cb_t on_frame; /**< 可为空的同步交付回调。 */
    void* user;                /**< 原样传给 on_frame 的调用方上下文。 */
    edge_parser_stats_t stats; /**< 当前解析器的累计统计。 */
} edge_parser_t;

/**
 * @brief 初始化逻辑解析状态并清空统计。
 * @param p 待初始化对象；为 NULL 时无操作。
 * @param cb CRC 正确时调用的函数；可为 NULL，此时仍更新统计但不交付帧。
 * @param user 传给 cb 的上下文指针；协议层不取得所有权。
 *
 * 该操作使先前半帧不再可继续，但不为无效的 payload 数组字节清零。
 */
void edge_parser_init(edge_parser_t* p, edge_frame_cb_t cb, void* user);

/**
 * @brief 向状态机输入一个字节。
 * @param p 已初始化的解析器；为 NULL 时无操作。
 * @param byte 按线上顺序到达的下一个字节。
 *
 * 回调（若有）会在本函数返回前同步执行；同一解析器不可重入调用。
 */
void edge_parser_feed(edge_parser_t* p, uint8_t byte);

/**
 * @brief 按顺序输入连续缓冲区，语义等同于逐字节调用 edge_parser_feed()。
 * @param p 已初始化的解析器；为 NULL 时无操作。
 * @param buf 输入缓冲区；为 NULL 时无论 n 为何都不执行操作。
 * @param n 输入字节数。
 */
void edge_parser_feed_buf(edge_parser_t* p, const uint8_t* buf, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_FRAME_H */
