#pragma once

// 金标准向量文件加载器(测试专用)。
//
// protocol/vectors/*.csv 由网关与 STM32 节点共读,节点侧有一份等价的纯 C 加载器
// (Protocol/test/)。不引第三方 CSV 库:格式自定义且规模可控,引入依赖会给
// MCU 侧的对等实现增加负担。

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gwtest {

// 向量文件里的一行,已按逗号切好
using VectorRow = std::vector<std::string>;

inline std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 读取向量文件:跳过 # 注释行与空行,其余按逗号切分。
// 文件缺失直接抛异常:静默跳过会伪装成测试通过。
inline std::vector<VectorRow> loadVectors(const std::string& relative_path,
                                          std::size_t expected_columns) {
    const std::string path = std::string(GATEWAY_TEST_DATA_DIR) + "/" + relative_path;
    std::ifstream     in(path);
    if (!in) {
        throw std::runtime_error("向量文件打不开: " + path);
    }

    std::vector<VectorRow> rows;
    std::string            line;
    int                    lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        VectorRow          row;
        std::stringstream  ss(t);
        std::string        cell;
        while (std::getline(ss, cell, ',')) {
            row.push_back(trim(cell));
        }
        // 末列可能为空(getline 不产出尾随空段),补齐到期望列数
        while (row.size() < expected_columns) row.push_back("");

        if (row.size() != expected_columns) {
            throw std::runtime_error(path + ":" + std::to_string(lineno) + " 列数为 " +
                                     std::to_string(row.size()) + ",期望 " +
                                     std::to_string(expected_columns));
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        throw std::runtime_error("向量文件里一行数据都没有: " + path);
    }
    return rows;
}

// "AA 55 01" → {0xAA, 0x55, 0x01};"-" 与空串表示空序列
inline std::vector<uint8_t> parseHex(const std::string& s) {
    std::vector<uint8_t> out;
    if (s.empty() || s == "-") return out;

    std::stringstream ss(s);
    std::string       tok;
    while (ss >> tok) {
        out.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
    }
    return out;
}

// 0x1234 / 1234 都接受
inline uint16_t parseU16(const std::string& s) {
    return static_cast<uint16_t>(std::stoul(s, nullptr, 0) & 0xFFFFu);
}

// 断言失败时以十六进制打印,便于比对字节序列
inline std::string toHex(const std::vector<uint8_t>& v) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string        s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ' ';
        s += kDigits[v[i] >> 4];
        s += kDigits[v[i] & 0x0F];
    }
    return s.empty() ? "-" : s;
}

}  // namespace gwtest
