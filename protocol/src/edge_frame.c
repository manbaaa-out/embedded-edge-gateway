#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_crc16.h"

/* ============================================================
 * 组帧:AA 55 | LEN | TYPE | payload | CRC_LO CRC_HI
 *   CRC 覆盖 LEN..payload(帧头是固定值,损坏由 FSM 在头部阶段丢弃,
 *   不进 CRC;CRC 字段本身也不自校验)。CRC 按小端写入帧尾。
 * ============================================================ */
uint8_t edge_frame_encode(uint8_t type, const uint8_t* payload, uint8_t payload_len, uint8_t* out) {
    if (out == NULL) {
        return 0;
    }
    if (payload_len > EDGE_PAYLOAD_MAX) {
        return 0; /* 超过 LEN 上限,组不出合法帧 */
    }
    if (payload == NULL && payload_len != 0u) {
        return 0; /* payload 为 NULL 时长度必须为 0 */
    }

    uint8_t n = 0;
    uint16_t crc = EDGE_CRC16_INIT;

    out[n++] = (uint8_t) EDGE_HDR0; /* 帧头不进 CRC */
    out[n++] = (uint8_t) EDGE_HDR1;

    const uint8_t len = (uint8_t) (1u + payload_len); /* LEN 含 TYPE 一字节 */
    out[n++] = len;
    crc = edge_crc16_update(crc, len);

    out[n++] = type;
    crc = edge_crc16_update(crc, type);

    for (uint8_t i = 0; i < payload_len; ++i) {
        out[n++] = payload[i];
        crc = edge_crc16_update(crc, payload[i]);
    }

    out[n++] = (uint8_t) (crc & 0xFFu); /* CRC_LO 在前(小端) */
    out[n++] = (uint8_t) (crc >> 8);    /* CRC_HI */

    return n;
}

/* ============================================================
 * 收帧 FSM
 *
 * 【resync 核心思想】任何错误路径都汇聚到同一个安全起点 WAIT_HDR0,
 * 最坏代价是丢 1 帧,下一帧照常解析 —— FSM 永不卡死。
 * ============================================================ */

/* 回到起点并记一次 resync。错误只计数,不打印:协议层不做 I/O。 */
static void edge_parser_resync(edge_parser_t* p) {
    p->state = EDGE_ST_WAIT_HDR0;
    p->stats.resync++;
}

static void edge_parser_deliver(edge_parser_t* p) {
    const uint16_t received = (uint16_t) (((uint16_t) p->crc_hi << 8) | p->crc_lo);

    if (received == p->crc) {
        p->stats.frames_ok++;
        if (p->on_frame != NULL) {
            p->on_frame(p->type, p->payload, (uint8_t) (p->len - 1u), p->user);
        }
        p->state = EDGE_ST_WAIT_HDR0; /* 正常路径不算 resync */
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
        /* 非 0xAA 一律静默丢弃,留在本态 —— 这是噪声中找帧头的常态,不算错误 */
        if (byte == EDGE_HDR0) {
            p->state = EDGE_ST_WAIT_HDR1;
        }
        break;

    case EDGE_ST_WAIT_HDR1:
        if (byte == EDGE_HDR1) {
            p->crc = EDGE_CRC16_INIT; /* 帧头不进 CRC,从 LEN 开始累加 */
            p->state = EDGE_ST_WAIT_LEN;
        } else if (byte == EDGE_HDR0) {
            /* 自环:连续的 AA AA 55 里,后一个 AA 才是真帧头 */
        } else {
            p->state = EDGE_ST_WAIT_HDR0;
        }
        break;

    case EDGE_ST_WAIT_LEN:
        /* §5.3 Fail Fast:LEN 一到手立刻查范围。放过一个坏 LEN,
         * FSM 会按错误长度吞掉后续若干帧(灾难传播);当场丢弃只丢 1 帧。 */
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
        /* 本层【不校验 TYPE 是否在字典中】,只保证帧的物理结构合法。
         * TYPE 与方向的合法性交由上层业务判断 —— 关注点分离:
         * 协议层稳定,业务层迭代不牵动协议层。 */
        p->type = byte;
        p->crc = edge_crc16_update(p->crc, byte);
        p->received = 0;
        p->state = (p->len == EDGE_LEN_MIN) ? EDGE_ST_WAIT_CRC_LO : EDGE_ST_READ_PAYLOAD;
        break;

    case EDGE_ST_READ_PAYLOAD:
        /* p->len 已在 WAIT_LEN 校验过 ≤ EDGE_LEN_MAX,
         * 故 received 最大到 len-1 = 63 = EDGE_PAYLOAD_MAX,写不越界。 */
        p->payload[p->received] = byte;
        p->received++;
        p->crc = edge_crc16_update(p->crc, byte);
        if (p->received >= (uint8_t) (p->len - 1u)) {
            p->state = EDGE_ST_WAIT_CRC_LO;
        }
        break;

    case EDGE_ST_WAIT_CRC_LO:
        p->crc_lo = byte; /* CRC 字段本身不进 CRC 累加 */
        p->state = EDGE_ST_WAIT_CRC_HI;
        break;

    case EDGE_ST_WAIT_CRC_HI:
        p->crc_hi = byte;
        p->state = EDGE_ST_DELIVER;
        edge_parser_deliver(p); /* 交付后立即回起点,DELIVER 不跨 feed 停留 */
        break;

    case EDGE_ST_DELIVER:
    default:
        /* 防御性:DELIVER 是瞬态,正常不可能带着它进下一次 feed。
         * 真到了这里说明状态被外部破坏,回起点比继续解析安全。 */
        edge_parser_resync(p);
        break;
    }
}

void edge_parser_feed_buf(edge_parser_t* p, const uint8_t* buf, size_t n) {
    if (p == NULL || buf == NULL) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        edge_parser_feed(p, buf[i]);
    }
}
