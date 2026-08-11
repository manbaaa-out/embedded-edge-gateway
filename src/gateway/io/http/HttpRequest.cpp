// 四态解析的状态转移。每一态都从 Buffer 里【尽量取】,取不够就原样返回等下次 recv,
// 绝不假设一次读到了完整结构 —— 与串口收帧 FSM 是同一套纪律。

#include "gateway/io/http/HttpRequest.h"
#include <algorithm>

namespace gateway {

ParseResult HttpRequest::parse(Buffer* buf) {
    bool ok = true;
    bool hasMore = true;

    while (hasMore) {
        if (state_ == HttpRequestParseState::ExpectRequestLine) {
            const char* crlf = std::search(buf->peek(), buf->beginWrite(),
                                        kCRLF, kCRLF + 2);
            if (crlf != buf->beginWrite()) {
                ok = parseRequestLine(buf->peek(), crlf);
                if (ok) {
                    // 指针差是 ptrdiff_t 而 retrieve 收 size_t;crlf 由 std::search 在
                    // [peek, beginWrite) 内找到,必 >= peek,故差值非负。
                    buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));
                    state_ = HttpRequestParseState::ExpectHeaders;
                }
                else {
                    hasMore = false;   // 请求行非法
                }
            }
            else {
                hasMore = false;       // 未见 CRLF,半包,等下次可读
            }
        }
        else if (state_ == HttpRequestParseState::ExpectHeaders) {
            const char* crlf = std::search(buf->peek(), buf->beginWrite(),
                                        kCRLF, kCRLF + 2);
            if (crlf != buf->beginWrite()) {
                const char* colon = std::find(buf->peek(), crlf, ':');
                if (colon != crlf) {
                    addHeader(buf->peek(), colon, crlf);
                }
                else {
                    // 空行:header 段结束
                    long len = contentLength();
                    if (len < 0) {
                        ok = false;                                    // 非法 Content-Length
                        hasMore = false;
                    } else if (len > 0) {
                        state_ = HttpRequestParseState::ExpectBody;
                    } else {
                        state_ = HttpRequestParseState::GotAll;
                    }
                }
                buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));   // 两种分支都消费本行
            }
            else {
                hasMore = false;   // 半包
            }
        }
        else if (state_ == HttpRequestParseState::ExpectBody) {
            long len = contentLength();   // 进入本状态的前提即 len > 0
            if (buf->readableBytes() >= static_cast<size_t>(len)) {
                body_.assign(buf->peek(), buf->peek() + len);
                buf->retrieve(static_cast<size_t>(len));
                state_ = HttpRequestParseState::GotAll;
            } else {
                hasMore = false;
            }
        }
        else {  // GotAll
            hasMore = false;
        }
    }

    // 把 ok / state_ 归约为 ParseResult
    if (!ok) {
        return ParseResult::kError;
    }
    if (state_ == HttpRequestParseState::GotAll) {
        return ParseResult::kComplete;
    }
    return ParseResult::kIncomplete;
}

bool HttpRequest::parseRequestLine(const char* begin, const char* end) {
    const char* space1 = std::find(begin, end, ' ');
    if (space1 == end) return false;
    method_.assign(begin, space1);

    const char* start = space1 + 1;
    const char* space2 = std::find(start, end, ' ');
    if (space2 == end) return false;
    path_.assign(start, space2);

    version_.assign(space2 + 1, end);
    return true;
}

void HttpRequest::addHeader(const char* start, const char* colon, const char* end) {
    std::string key(start, colon);
    // header 名大小写不敏感(RFC 9110),统一转小写后入表
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
        return 0;                      // 无 Content-Length,body 长度视为 0
    }
    try {
        return std::stol(it->second);
    } catch (...) {
        return -1;                     // 非法值,由调用方按错误处理
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
