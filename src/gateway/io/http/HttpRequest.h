#pragma once

/**
 * @file
 * 面向 TCP 字节流的轻量 HTTP/1.x 请求状态机。
 *
 * 解析器按“请求行 → 头部 → Content-Length 正文”推进，可跨多次 recv 保留状态，
 * 也会在完成一条请求后把粘连的后续字节留在 Buffer 中。它服务于内嵌只读监控接口，
 * 不实现 chunked 等完整 HTTP 服务器特性。
 */

#include <string>
#include <map>
#include "gateway/io/http/Buffer.h"

namespace gateway{

/** 当前请求下一步期望解析的协议区段。 */
enum class HttpRequestParseState {
ExpectRequestLine,  ///< 等待一行 method、path 和 version。
ExpectHeaders,      ///< 逐行读取头字段，直至空行。
ExpectBody,         ///< 按 Content-Length 等待固定长度正文。
GotAll              ///< 当前请求已经完整。
};

/** 一次增量 parse 调用的结果。 */
enum class ParseResult {
    kError,       ///< 已确认当前请求不合法，连接应关闭或重置。
    kIncomplete,  ///< 当前缓冲不足，等待更多字节后继续。
    kComplete     ///< 已得到一条完整请求。
};

/** 保存单条 HTTP 请求的增量解析状态和已解析字段。 */
class HttpRequest {
    public:
    /** 允许的最大 Content-Length，单位为字节。 */
    static constexpr long kMaxBodySize = 64 * 1024;

    /**
     * 尽量从缓冲中推进当前请求。
     * @param buf 当前连接的非空读缓冲；已确认属于本请求的前缀会被消费。
     * @return 错误、等待更多数据或请求完整三种状态之一。
     */
    ParseResult parse(Buffer* buf);

    /** @return 请求方法原文。 */
    const std::string& method() const { return method_; }
    /** @return 包含原始查询串的 request-target。 */
    const std::string& path() const { return path_; }
    /** @return 请求行中的 HTTP 版本原文。 */
    const std::string& version() const { return version_; }
    /** @return 按 Content-Length 读取的请求体。 */
    const std::string& body() const { return body_; }
    /**
     * 大小写不敏感地查找头字段。
     * @param key 待查询的字段名。
     * @return 字段值的引用；不存在时返回共享空字符串。
     */
    const std::string& getHeader(const std::string& key) const {
        static const std::string empty;  // 所有未命中查询共享的稳定返回对象。
        std::string lower(key);          // 查询键的小写副本。
        // c 是字段名中的单个无符号字节，避免负 char 传入 tolower。
        std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c){ return std::tolower(c); });
        auto it = headers_.find(lower);  // 规范化头表中的匹配位置。
        return it != headers_.end() ? it->second : empty;
    }
    /** @return 是否已经完成当前请求。 */
    bool gotAll() const { return state_ == HttpRequestParseState::GotAll; }
    /** 清空字段并回到等待下一条请求行的状态。 */
    void reset();

    private:
    HttpRequestParseState state_ = HttpRequestParseState::ExpectRequestLine;  ///< 当前状态。
    static constexpr char kCRLF[] = "\r\n";  ///< 请求行和头字段的行结束符。

    std::string method_;   ///< 请求方法原文。
    std::string path_;     ///< request-target 原文，可能包含查询串。
    std::string version_;  ///< HTTP 版本原文。
    std::map<std::string, std::string> headers_;  ///< 小写字段名到字段值的映射。
    std::string body_;     ///< 已完整读取的固定长度正文。

    /**
     * @param begin 请求行首字节。
     * @param end CRLF 前的一过尾指针。
     * @return 是否找到两个分隔空格并提取三个字段。
     */
    bool parseRequestLine(const char* begin, const char* end);
    /**
     * 规范化并保存一行头字段。
     * @param start 字段名起点。
     * @param colon 名称和值之间的冒号位置。
     * @param end CRLF 前的一过尾指针。
     * 同名字段会覆盖此前保存的值。
     */
    void addHeader(const char* start, const char* colon, const char* end);
    /**
     * @return 正文长度；缺失时为 0，转换失败、负值或超过上限时为 -1。
     * 当前实现使用 std::stol 的前缀解析语义，不检查数字后的剩余字符。
     */
    long contentLength() const;
};

}
