// test_crc16.cpp
#include "CRC16.h"
#include <iostream>
#include <iomanip>
#include <vector>

void runTest(const std::vector<uint8_t>& data, uint16_t expected) {
    m5::CRC16 calc;
    for (auto b : data) {
        calc.update(b);
    }
    uint16_t got = calc.value();
    
    std::cout << "Input: ";
    for (auto b : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(b) << " ";
    }
    std::cout << "| Expected: 0x" << std::setw(4) << expected
              << " | Got: 0x" << std::setw(4) << got
              << " | " << (got == expected ? "PASS" : "FAIL") << "\n";
}

int main() {
    runTest({0x01, 0x03, 0x00, 0x00, 0x00, 0x02}, 0x0BC4);
    runTest({0x01, 0x06, 0x00, 0x01, 0x00, 0x03}, 0x0B98);
    runTest({0x02, 0x01, 0x00, 0x00, 0x00, 0x02}, 0xF8BD);
    runTest({0x04}, 0x83BE);
    runTest({0x01, 0x03}, 0x2140);             // 心跳帧 CRC (LEN=1, TYPE=0x03)
    runTest({0x03, 0x02, 0x01, 0x90}, 0x5CA0); // 光照帧 CRC (LEN=3, TYPE=0x02, payload=01 90)
    return 0;
}