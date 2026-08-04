#include "HttpRequest.h"
#include <algorithm>

namespace gateway {

ParseResult HttpRequest::parse(Buffer* buf) {
    bool ok = true;
    bool hasMore = true;

    while (hasMore) {
        if (state_ == HttpRequestParseState::ExpectRequestLine) {
            // 1. 在 buf 里找 \r\n
            const char* crlf = std::search(buf->peek(), buf->beginWrite(),
                                        kCRLF, kCRLF + 2);
            if (crlf != buf->beginWrite()) {
                // 找到完整一行 [peek(), crlf)
                ok = parseRequestLine(buf->peek(), crlf);   // 解析请求行
                if (ok) {
                    // 指针差是 ptrdiff_t(有符号),而 retrieve 收 size_t。
                    // crlf 由 std::search 在 [peek, beginWrite) 内找到,必 >= peek,差值非负。
                    buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));  // 消费掉这行(含\r\n)
                    state_ = HttpRequestParseState::ExpectHeaders;  // 转移状态
                }
                else {
                    hasMore = false;   // 请求行非法
                }
            }
            else {
                hasMore = false;       // 没找到\r\n,半包,退出等下次
            }
        }
        else if (state_ == HttpRequestParseState::ExpectHeaders) {
            const char* crlf = std::search(buf->peek(), buf->beginWrite(),
                                        kCRLF, kCRLF + 2);
            if (crlf != buf->beginWrite()) {
                const char* colon = std::find(buf->peek(), crlf, ':');
                if (colon != crlf) {
                    // 情况1:普通 header 行,有冒号 → 拆 key:value 存进 map
                    addHeader(buf->peek(), colon, crlf);
                }
                else {
                    long len = contentLength();
                    if (len < 0) {
                        ok = false;                                    // 非法 Content-Length → 报错
                        hasMore = false;
                    } else if (len > 0) {
                        state_ = HttpRequestParseState::ExpectBody;
                    } else {
                        state_ = HttpRequestParseState::GotAll;
                    }
                }
                buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));   // 不管哪种,都消费掉这一行
            }
            else {
                hasMore = false;   // 半包
            }
        }
        else if (state_ == HttpRequestParseState::ExpectBody) {
            long len = contentLength();          // 已经验证过,这里 >0
            if (buf->readableBytes() >= static_cast<size_t>(len)) {
                body_.assign(buf->peek(), buf->peek() + len);
                buf->retrieve(static_cast<size_t>(len));   // 上面 len > 0 已验证
                state_ = HttpRequestParseState::GotAll;
            } else {
                hasMore = false;
            }
        }
        else {  // GotAll
            hasMore = false;
        }
    }

    // 把 ok / state_ 翻译成 ParseResult —— 这步你来想
    if (!ok) {
        return ParseResult::kError;
    }
    if (state_ == HttpRequestParseState::GotAll) {
        return ParseResult::kComplete;
    }
    return ParseResult::kIncomplete;
}

bool HttpRequest::parseRequestLine(const char* begin, const char* end) {
    const char* space1 = std::find(begin, end, ' ');     // 第一个空格
    if (space1 == end) return false;                     // 没找到,非法
    method_.assign(begin, space1);                       // [begin, space1) = method

    const char* start = space1 + 1;                      // 跳过空格
    const char* space2 = std::find(start, end, ' ');   // ← 第二个空格,从哪开始找?
    if (space2 == end) return false;
    path_.assign(start, space2);                      // ← path 是哪一段?

    version_.assign(space2 + 1, end);                    // 剩下的就是 version
    return true;
}

void HttpRequest::addHeader(const char* start, const char* colon, const char* end) {
    std::string key(start, colon);
    // ↓ key 转小写
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    const char* valueStart = colon + 1;
    while (valueStart < end && *valueStart == ' ') {
        valueStart += 1;
    }
    std::string value(valueStart, end);
    headers_[key] = value;
}

long HttpRequest::contentLength() const {
    auto it = headers_.find("content-length");
    if (it == headers_.end()) {
        return 0;                      // 没有这个 header,body 长度 0
    }
    try {
        return std::stol(it->second);  // 字符串转 long
    } catch (...) {
        return -1;                     // 非法值,标记错误
    }
}

void HttpRequest::reset() {
    state_ = HttpRequestParseState::ExpectRequestLine;
    method_.clear();
    path_.clear();
    version_.clear();
    headers_.clear();
    body_.clear();
}

}
