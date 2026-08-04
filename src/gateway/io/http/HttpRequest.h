#pragma once
#include <string>
#include <map>
#include "gateway/io/http/Buffer.h"

namespace gateway{

enum class HttpRequestParseState {
ExpectRequestLine,
ExpectHeaders,
ExpectBody,
GotAll
};

enum class ParseResult {
    kError,
    kIncomplete,
    kComplete
};

class HttpRequest {
    public:
    ParseResult parse(Buffer* buf);

    const std::string& method() const { return method_; }
    const std::string& path() const { return path_; }
    const std::string& version() const { return version_; }
    const std::string& body() const { return body_; }
    const std::string& getHeader(const std::string& key) const {
        static const std::string empty;
        std::string lower(key);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c){ return std::tolower(c); });
        auto it = headers_.find(lower);
        return it != headers_.end() ? it->second : empty;
    }
    bool gotAll() const { return state_ == HttpRequestParseState::GotAll; }
    void reset();

    private:
    HttpRequestParseState state_ = HttpRequestParseState::ExpectRequestLine;
    static constexpr char kCRLF[] = "\r\n";

    std::string method_;
    std::string path_;
    std::string version_;
    std::map<std::string, std::string> headers_;
    std::string body_;


    bool parseRequestLine(const char* begin, const char* end);
    void addHeader(const char* start, const char* colon, const char* end);
    long contentLength() const;
};

}



