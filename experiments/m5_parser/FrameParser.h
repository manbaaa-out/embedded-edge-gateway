#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include "CRC16.h"

namespace m5 {

struct Frame {
    uint8_t type;
    std::vector<uint8_t> payload;
};

enum class State {
    WAIT_HEADER1,
    WAIT_HEADER2,
    WAIT_LEN,
    WAIT_TYPE,
    READ_PAYLOAD ,
    WAIT_CRC_LO,
    WAIT_CRC_HI,
    DELIVER
};

constexpr uint8_t HEADER1 = 0xAA;
constexpr uint8_t HEADER2 = 0x55;
constexpr uint8_t LEN_MAX = 64;
constexpr uint8_t LEN_MIN = 1;

class FrameParser {
    public:
    using OnFrameCallback = std::function<void(const Frame&)>;
    FrameParser() = default;

    void feed(uint8_t byte);

    void setOnFrame(OnFrameCallback cb) {onFrame_ = std::move(cb);}

    State currentState() const noexcept {return state_;}

    private:
    State state_ = State::WAIT_HEADER1;
    OnFrameCallback onFrame_;

    uint8_t len_ = 0;
    uint8_t type_ = 0;
    std::vector<uint8_t> payload_buffer_;
    uint8_t bytes_received_ = 0;

    CRC16 crc_;
    uint8_t crc_lo_ = 0;
    uint8_t crc_hi_ = 0;

    void handleDeliver();  
};

}