#include "FrameParser.h"
#include "CRC16.h"
#include <iostream>

using namespace m5;

void FrameParser::feed(uint8_t byte) {
    switch (state_)
    {
    case State::WAIT_HEADER1 :
        switch (byte)
        {
        case HEADER1:
            state_ = State::WAIT_HEADER2;
            break;
        
        default:
            break;
        }

        break;

    case State::WAIT_HEADER2:
        switch (byte)
        {
        case HEADER1:
            break;
        
        case HEADER2:
            crc_.reset(); 
            state_ = State::WAIT_LEN;
            break;
        default:
            state_ = State::WAIT_HEADER1;
            break;
        }
        break;

    case State::WAIT_LEN:
        if (byte < LEN_MIN || byte > LEN_MAX) {
             std::cerr << "[ERROR] LEN out of range: "
                       << static_cast<int>(byte) << ", expected [" 
                       << static_cast<int>(LEN_MIN) << ", " 
                       << static_cast<int>(LEN_MAX) << "]\n";
            state_ = State::WAIT_HEADER1;
        }
        else {
            len_ = byte;
            crc_.update(byte);
            state_ = State::WAIT_TYPE;
        }
        break;

    case State::WAIT_TYPE:
        type_ = byte;
        crc_.update(byte); 
        payload_buffer_.clear();
        bytes_received_ = 0;
        if (len_ == LEN_MIN) {
            state_ = State::WAIT_CRC_LO;
        }
        else {
            payload_buffer_.reserve(len_ - 1);
            state_ = State::READ_PAYLOAD ;
        }
        break;

    case State::READ_PAYLOAD:
        payload_buffer_.push_back(byte);
        bytes_received_ ++;
        crc_.update(byte); 
        if (bytes_received_ >= (len_ - 1)) {
            state_ = State::WAIT_CRC_LO;
        }
        break;
    
    case State::WAIT_CRC_LO:                    // ← 新增完整实现
        crc_lo_ = byte;
        state_ = State::WAIT_CRC_HI;
        break;
    
    case State::WAIT_CRC_HI:
        crc_hi_ = byte;
        state_ = State::DELIVER;
        handleDeliver();
        break;

    case State::DELIVER:
        // 瞬态:正常不应停留
        // 防御性日志
        std::cerr << "[ERROR] Unexpected feed in DELIVER state\n";
        state_ = State::WAIT_HEADER1;
        break;

    default:
        std::cerr << "[TODO] State " << static_cast<int>(state_) 
                  << " not implemented, resetting\n";
        state_ = State::WAIT_HEADER1;
        break;
    }
}

void FrameParser::handleDeliver() {
    uint16_t computed_crc = crc_.value();
    uint16_t received_crc = (static_cast<uint16_t>(crc_hi_) << 8) | crc_lo_;

    if (computed_crc == received_crc) {
        // CRC 正确,交付帧给上层
        if (onFrame_) {
            Frame f;
            f.type = type_;
            f.payload = payload_buffer_;
            onFrame_(f);
        }
    } else {
        std::cerr << "[ERROR] CRC mismatch: computed=0x"
                  << std::hex << computed_crc
                  << ", received=0x" << received_crc
                  << std::dec << "\n";
    }

    // 无论成功失败,都 resync
    state_ = State::WAIT_HEADER1;

}