#ifndef EDGE_CRC16_H
#define EDGE_CRC16_H

/* ----------------------------------------------------------------------------
 * CRC16-MODBUS(多项式 0xA001 = 0x8005 反射,初值 0xFFFF,反射输入输出,末位不异或)
 *
 * 选它的理由:嵌入式串口事实标准;对 ≤1KB 帧检测能力优秀;查表法每字节约 10 个
 * 时钟周期,MCU 上开销可忽略。
 *
 * 设计为纯函数(状态由调用方持有,不引入对象),使同一份实现同时服务于:
 *   - 接收端 FSM:逐字节累加
 *   - 发送端组帧:一次算完整段
 *
 * 【不可改动】docs/protocol.md §4.3 的测试向量是两端的验收线,
 * 任一向量不匹配即实现错误,绝不能上线。tests/protocol 会逐条比对。
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_CRC16_INIT 0xFFFFu

/* 逐字节累加:传入当前 crc 与一个字节,返回更新后的 crc。
 * 用法:uint16_t crc = EDGE_CRC16_INIT; crc = edge_crc16_update(crc, b); ... */
uint16_t edge_crc16_update(uint16_t crc, uint8_t byte);

/* 一次性算整段 buffer 的 CRC(内部从 EDGE_CRC16_INIT 起循环累加) */
uint16_t edge_crc16(const uint8_t* data, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_CRC16_H */
