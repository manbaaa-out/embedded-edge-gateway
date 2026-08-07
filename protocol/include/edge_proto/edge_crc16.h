#ifndef EDGE_CRC16_H
#define EDGE_CRC16_H

/*
 * CRC16-MODBUS:多项式 0xA001(0x8005 反射),初值 0xFFFF,反射输入输出,末位不异或。
 *
 * 选型理由:嵌入式串口事实标准,对 ≤16 位的突发错误 100% 检出 —— 而突发正是串口的
 * 典型故障形态;查表法每字节约 10 个时钟周期,MCU 上开销可忽略。
 *
 * 接口设计为纯函数(状态由调用方持有),使同一份实现同时服务接收端 FSM 的逐字节累加
 * 与发送端组帧的整段计算,两条路径不可能算出不同结果。
 *
 * docs/protocol.md §4.3 的测试向量是两端的验收线,任一向量不匹配即实现错误;
 * vectors/crc16.csv 与 tests/protocol 会逐条比对。
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGE_CRC16_INIT 0xFFFFu

/* 把一个字节累加进 crc,返回更新后的值。
 * 用法:uint16_t crc = EDGE_CRC16_INIT; crc = edge_crc16_update(crc, b); ... */
uint16_t edge_crc16_update(uint16_t crc, uint8_t byte);

/* 计算整段 buffer 的 CRC,内部自 EDGE_CRC16_INIT 起累加 */
uint16_t edge_crc16(const uint8_t* data, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_CRC16_H */
