#pragma once

// 协议一致性测试的轻量向量加载器。CSV 格式由仓库控制，不支持引号、嵌套逗号等
// 通用 CSV 语义；失败直接抛异常，防止数据文件缺失被误报为零用例通过。

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gwtest {

// VectorRow 表示已 trim 且按逗号拆分的一行，字段含义由具体向量文件定义。
using VectorRow = std::vector<std::string>;

// 移除 s 首尾的 ASCII 空白；全空白输入返回空串。b/e 分别是首尾有效字符位置。
inline std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 从源码根目录读取 relative_path，并要求每个数据行恰有 expected_columns 列。
// 返回的 rows 跳过空行和 # 开头说明行；文件缺失、列数异常或无数据均抛异常。
inline std::vector<VectorRow> loadVectors(const std::string& relative_path,
                                          std::size_t expected_columns) {
    const std::string path =
        std::string(GATEWAY_TEST_DATA_DIR) + "/" + relative_path; // 绝对输入路径。
    std::ifstream in(path);                                       // 向量只读输入流。
    if (!in) {
        throw std::runtime_error("向量文件打不开: " + path);
    }

    std::vector<VectorRow> rows; // 已通过列数校验的数据行。
    std::string line;            // 当前原始文本行。
    int lineno = 0;              // 一基行号，用于异常定位。
    while (std::getline(in, line)) {
        ++lineno;
        const std::string t = trim(line); // 去除行尾换行和字段外空白后的整行。
        if (t.empty() || t[0] == '#') continue;

        VectorRow row;           // 当前行拆分后的字段集合。
        std::stringstream ss(t); // 仅按逗号顺序读取字段。
        std::string cell;        // 当前尚未 trim 的字段文本。
        while (std::getline(ss, cell, ',')) {
            row.push_back(trim(cell));
        }
        // std::getline 不返回末尾空字段，按固定列数补齐后再校验。
        while (row.size() < expected_columns)
            row.push_back("");

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

// 将空格分隔的十六进制字符串 s 转为字节；空串和 "-" 表示空序列。
inline std::vector<uint8_t> parseHex(const std::string& s) {
    std::vector<uint8_t> out; // 按输入顺序累积的字节。
    if (s.empty() || s == "-") return out;

    std::stringstream ss(s); // 以任意空白分隔 token。
    std::string tok;         // 当前十六进制字节文本。
    while (ss >> tok) {
        out.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
    }
    return out;
}

// 将 s 按 C 风格基数规则解析并限制为 uint16_t；基数 0 接受十进制和 0x 前缀。
inline uint16_t parseU16(const std::string& s) {
    return static_cast<uint16_t>(std::stoul(s, nullptr, 0) & 0xFFFFu);
}

// 将字节向量 v 格式化为两位大写十六进制并以空格分隔；空向量返回 "-"。
inline std::string toHex(const std::vector<uint8_t>& v) {
    static const char* kDigits = "0123456789ABCDEF"; // 半字节到字符的查找表。
    std::string s;                                   // 逐字节追加的结果字符串。
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ' ';
        s += kDigits[v[i] >> 4];
        s += kDigits[v[i] & 0x0F];
    }
    return s.empty() ? "-" : s;
}

} // namespace gwtest
