#ifndef EDGE_CRC16_H
#define EDGE_CRC16_H

/**
 * @file edge_crc16.h
 * @brief CRC-16/MODBUS 的流式与整段计算接口。
 *
 * 两个接口共用同一更新规则：编码器可逐字节更新，调用方也可一次校验完整缓冲区。
 * 实现不分配内存、不执行 I/O，适合网关和 MCU 共用。
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-16/MODBUS 初值；反射多项式为 0xA001，最终结果不再异或。 */
#define EDGE_CRC16_INIT 0xFFFFu

/**
 * @brief 将一个字节累加到已有 CRC 状态。
 * @param crc 上一轮 CRC；首字节应传 EDGE_CRC16_INIT。
 * @param byte 本轮参与校验的字节。
 * @return 累加后的 CRC，可继续传给下一次调用。
 */
uint16_t edge_crc16_update(uint16_t crc, uint8_t byte);

/**
 * @brief 从 EDGE_CRC16_INIT 开始计算一段连续数据的 CRC。
 * @param data 输入缓冲区；len 大于 0 时必须指向至少 len 个有效字节。
 * @param len 输入字节数；为 0 时返回初值，data 可为 NULL。
 * @return 整段数据的 CRC-16/MODBUS 值。
 */
uint16_t edge_crc16(const uint8_t* data, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EDGE_CRC16_H */
