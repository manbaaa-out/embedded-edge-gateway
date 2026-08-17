/** @file HttpRequest 各解析状态的推进与字段提取实现。 */

#include "gateway/io/http/HttpRequest.h"
#include <algorithm>

namespace gateway {

ParseResult HttpRequest::parse(Buffer* buf) {
    bool ok = true;       // 当前请求是否仍可继续解析。
    bool hasMore = true;  // 本次调用是否还可能从现有缓冲推进状态。

    while (hasMore) {
        if (state_ == HttpRequestParseState::ExpectRequestLine) {
            const char* crlf = std::search(  // 当前请求行的结束位置或缓冲末尾。
                buf->peek(), buf->beginWrite(), kCRLF, kCRLF + 2);
            if (crlf != buf->beginWrite()) {
                ok = parseRequestLine(buf->peek(), crlf);
                if (ok) {
                    buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));
                    state_ = HttpRequestParseState::ExpectHeaders;
                }
                else {
                    hasMore = false;
                }
            }
            else {
                hasMore = false;
            }
        }
        else if (state_ == HttpRequestParseState::ExpectHeaders) {
            const char* crlf = std::search(  // 当前头字段行的结束位置或缓冲末尾。
                buf->peek(), buf->beginWrite(), kCRLF, kCRLF + 2);
            if (crlf != buf->beginWrite()) {
                if (crlf == buf->peek()) {
                    long len = contentLength();  // 空行后确定是否还需读取正文。
                    if (len < 0) {
                        ok = false;
                        hasMore = false;
                    } else if (len > 0) {
                        state_ = HttpRequestParseState::ExpectBody;
                    } else {
                        state_ = HttpRequestParseState::GotAll;
                    }
                }
                else {
                    const char* colon = std::find(  // 字段名与字段值的分隔符。
                        buf->peek(), crlf, ':');
                    if (colon == crlf) {
                        ok      = false;
                        hasMore = false;
                    } else {
                        addHeader(buf->peek(), colon, crlf);
                    }
                }
                buf->retrieve(static_cast<size_t>(crlf + 2 - buf->peek()));
            }
            else {
                hasMore = false;
            }
        }
        else if (state_ == HttpRequestParseState::ExpectBody) {
            long len = contentLength();  // 进入本状态前已验证为正且未超限。
            if (buf->readableBytes() >= static_cast<size_t>(len)) {
                body_.assign(buf->peek(), buf->peek() + len);
                buf->retrieve(static_cast<size_t>(len));
                state_ = HttpRequestParseState::GotAll;
            } else {
                hasMore = false;
            }
        }
        else {
            hasMore = false;
        }
    }

    if (!ok) {
        return ParseResult::kError;
    }
    if (state_ == HttpRequestParseState::GotAll) {
        return ParseResult::kComplete;
    }
    return ParseResult::kIncomplete;
}

bool HttpRequest::parseRequestLine(const char* begin, const char* end) {
    const char* space1 = std::find(begin, end, ' ');  // method 后的分隔空格。
    if (space1 == end) return false;
    method_.assign(begin, space1);

    const char* start = space1 + 1;  // request-target 起点。
    const char* space2 = std::find(start, end, ' ');  // version 前的分隔空格。
    if (space2 == end) return false;
    path_.assign(start, space2);

    version_.assign(space2 + 1, end);
    return true;
}

void HttpRequest::addHeader(const char* start, const char* colon, const char* end) {
    std::string key(start, colon);  // 待规范化的字段名。
    // 字段名统一转小写，支持大小写不敏感的查找。
    // c 是字段名中的单个无符号字节，避免负 char 传入 tolower。
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    const char* valueStart = colon + 1;  // 跳过冒号及其后的可选空格。
    while (valueStart < end && *valueStart == ' ') {
        valueStart += 1;
    }
    std::string value(valueStart, end);  // 不含 CRLF 的字段值。
    headers_[key] = value;
}

long HttpRequest::contentLength() const {
    auto it = headers_.find("content-length");  // 规范化头表中的长度字段。
    if (it == headers_.end()) {
        return 0;
    }
    long v = 0;  // 解析后的正文长度，单位为字节。
    try {
        v = std::stol(it->second);
    } catch (...) {
        return -1;
    }
    if (v < 0 || v > kMaxBodySize) return -1;
    return v;
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
