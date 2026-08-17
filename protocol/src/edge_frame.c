#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_crc16.h"

/*
 * 共享帧实现保持两个方向对称：编码器按线序构造字节，解析器用相同顺序累计 CRC。
 * 状态机只判断物理帧是否完整，不判断 TYPE 是否适用于当前设备或 payload 业务内容。
 */

/* CRC 覆盖 LEN、TYPE 和 payload；帧头不参与计算，帧尾按 CRC 低字节在前写入。 */
uint8_t edge_frame_encode(uint8_t type, const uint8_t* payload, uint8_t payload_len, uint8_t* out) {
    if (out == NULL) {
        return 0;
    }
    if (payload_len > EDGE_PAYLOAD_MAX) {
        return 0;
    }
    if (payload == NULL && payload_len != 0u) {
        return 0;
    }

    uint8_t n = 0;                    /* out 中下一个写入位置，也是最终帧长。 */
    uint16_t crc = EDGE_CRC16_INIT;   /* 从 LEN 开始累计的发送侧 CRC。 */

    out[n++] = (uint8_t) EDGE_HDR0;
    out[n++] = (uint8_t) EDGE_HDR1;

    const uint8_t len = (uint8_t) (1u + payload_len); /* TYPE 一字节加 payload 长度。 */
    out[n++] = len;
    crc = edge_crc16_update(crc, len);

    out[n++] = type;
    crc = edge_crc16_update(crc, type);

    for (uint8_t i = 0; i < payload_len; ++i) { /* i 为 payload 的线序下标。 */
        out[n++] = payload[i];
        crc = edge_crc16_update(crc, payload[i]);
    }

    out[n++] = (uint8_t) (crc & 0xFFu);
    out[n++] = (uint8_t) (crc >> 8);

    return n;
}

/** @param p 非空解析器；将其切回帧头搜索状态并累计一次显式重同步。 */
static void edge_parser_resync(edge_parser_t* p) {
    p->state = EDGE_ST_WAIT_HDR0;
    p->stats.resync++;
}

/** @param p 已收齐 CRC 的非空解析器；校验成功则同步交付，否则丢弃当前帧。 */
static void edge_parser_deliver(edge_parser_t* p) {
    const uint16_t received =
        (uint16_t) (((uint16_t) p->crc_hi << 8) | p->crc_lo); /* 线上 CRC 合并值。 */

    if (received == p->crc) {
        p->stats.frames_ok++;
        if (p->on_frame != NULL) {
            p->on_frame(p->type, p->payload, (uint8_t) (p->len - 1u), p->user);
        }
        p->state = EDGE_ST_WAIT_HDR0;
    } else {
        p->stats.crc_err++;
        edge_parser_resync(p);
    }
}

void edge_parser_init(edge_parser_t* p, edge_frame_cb_t cb, void* user) {
    if (p == NULL) {
        return;
    }
    p->state = EDGE_ST_WAIT_HDR0;
    p->len = 0;
    p->type = 0;
    p->received = 0;
    p->crc = EDGE_CRC16_INIT;
    p->crc_lo = 0;
    p->crc_hi = 0;
    p->on_frame = cb;
    p->user = user;

    p->stats.frames_ok = 0;
    p->stats.len_err = 0;
    p->stats.crc_err = 0;
    p->stats.resync = 0;
}

void edge_parser_feed(edge_parser_t* p, uint8_t byte) {
    if (p == NULL) {
        return;
    }

    switch (p->state) {
    case EDGE_ST_WAIT_HDR0:
        /* 搜索帧头时丢弃普通噪声；这属于正常扫描，不增加错误统计。 */
        if (byte == EDGE_HDR0) {
            p->state = EDGE_ST_WAIT_HDR1;
        }
        break;

    case EDGE_ST_WAIT_HDR1:
        if (byte == EDGE_HDR1) {
            p->crc = EDGE_CRC16_INIT;
            p->state = EDGE_ST_WAIT_LEN;
        } else if (byte == EDGE_HDR0) {
            /* 保留最后一个 0xAA，使 AA AA 55 能从第二个字节开始匹配。 */
        } else {
            p->state = EDGE_ST_WAIT_HDR0;
        }
        break;

    case EDGE_ST_WAIT_LEN:
        /* 在后续任何定长缓冲区写入前验证 LEN。 */
        if (byte < EDGE_LEN_MIN || byte > EDGE_LEN_MAX) {
            p->stats.len_err++;
            edge_parser_resync(p);
        } else {
            p->len = byte;
            p->crc = edge_crc16_update(p->crc, byte);
            p->state = EDGE_ST_WAIT_TYPE;
        }
        break;

    case EDGE_ST_WAIT_TYPE:
        /* 解析器只验证帧结构；TYPE 和方向由业务层判断。 */
        p->type = byte;
        p->crc = edge_crc16_update(p->crc, byte);
        p->received = 0;
        p->state = (p->len == EDGE_LEN_MIN) ? EDGE_ST_WAIT_CRC_LO : EDGE_ST_READ_PAYLOAD;
        break;

    case EDGE_ST_READ_PAYLOAD:
        /* received 在写入后递增；LEN 上限保证最大有效下标为 EDGE_PAYLOAD_MAX - 1。 */
        p->payload[p->received] = byte;
        p->received++;
        p->crc = edge_crc16_update(p->crc, byte);
        if (p->received >= (uint8_t) (p->len - 1u)) {
            p->state = EDGE_ST_WAIT_CRC_LO;
        }
        break;

    case EDGE_ST_WAIT_CRC_LO:
        p->crc_lo = byte;
        p->state = EDGE_ST_WAIT_CRC_HI;
        break;

    case EDGE_ST_WAIT_CRC_HI:
        p->crc_hi = byte;
        p->state = EDGE_ST_DELIVER;
        edge_parser_deliver(p);
        break;

    case EDGE_ST_DELIVER:
    default:
        /* DELIVER 不应跨调用保留；未知状态同样按损坏状态恢复。 */
        edge_parser_resync(p);
        break;
    }
}

void edge_parser_feed_buf(edge_parser_t* p, const uint8_t* buf, size_t n) {
    if (p == NULL || buf == NULL) {
        return;
    }
    for (size_t i = 0; i < n; ++i) { /* i 为本批输入的线序下标。 */
        edge_parser_feed(p, buf[i]);
    }
}
