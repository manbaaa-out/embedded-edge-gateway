// HTTP 请求解析测试。

#include "gateway/io/http/Buffer.h"
#include "gateway/io/http/HttpRequest.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace gateway;

namespace {
// 把字符串写入 Buffer,避免每个用例重复计算长度
void feed(Buffer& buf, const char* s) {
    buf.append(s, std::strlen(s));
}
}  // namespace

TEST(HttpRequest, ParsesCompleteRequestWithBody) {
    HttpRequest req;
    Buffer      buf;
    feed(buf,
         "POST /submit HTTP/1.1\r\n"
         "Host: example.com\r\n"
         "Content-Length: 5\r\n"
         "\r\n"
         "hello");

    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.method(), "POST");
    EXPECT_EQ(req.path(), "/submit");
    EXPECT_EQ(req.version(), "HTTP/1.1");
    EXPECT_EQ(req.getHeader("Host"), "example.com");
    EXPECT_EQ(req.body(), "hello");
}

// 半包:TCP 不保证一次 read 取得完整请求,解析器必须能跨多次输入累积状态
TEST(HttpRequest, HandlesPartialThenComplete) {
    HttpRequest req;
    Buffer      buf;

    feed(buf, "GET /index.html HTTP/1.1\r\nHo");          // 在 header 名中间截断
    EXPECT_EQ(req.parse(&buf), ParseResult::kIncomplete);

    feed(buf, "st: a.com\r\n\r\n");
    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.path(), "/index.html");
    EXPECT_EQ(req.getHeader("Host"), "a.com");
}

TEST(HttpRequest, RejectsMalformedContentLength) {
    HttpRequest req;
    Buffer      buf;
    feed(buf,
         "POST /x HTTP/1.1\r\n"
         "Content-Length: abc\r\n"
         "\r\n");

    EXPECT_EQ(req.parse(&buf), ParseResult::kError) << "非法长度应报错而不是崩或挂起";
}

TEST(HttpRequest, ParsesRequestWithoutBody) {
    HttpRequest req;
    Buffer      buf;
    feed(buf, "GET / HTTP/1.1\r\nHost: a.com\r\n\r\n");

    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.method(), "GET");
    EXPECT_EQ(req.body(), "");
}

// RFC 7230:header 字段名大小写不敏感
TEST(HttpRequest, HeaderKeysAreCaseInsensitive) {
    HttpRequest req;
    Buffer      buf;
    feed(buf,
         "POST /x HTTP/1.1\r\n"
         "CONTENT-LENGTH: 3\r\n"
         "\r\n"
         "abc");

    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.body(), "abc") << "归一化失效会导致 body 长度识别不出来";
}

TEST(HttpRequest, ParsesPipelinedRequestsWithSeparateObjects) {
    Buffer buf;
    feed(buf, "GET /first HTTP/1.1\r\n\r\nGET /second HTTP/1.1\r\n\r\n");

    HttpRequest req1;
    ASSERT_EQ(req1.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req1.path(), "/first");

    HttpRequest req2;
    ASSERT_EQ(req2.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req2.path(), "/second");
}

// 长连接:同一个 HttpRequest 对象在 reset 后可复用
TEST(HttpRequest, ResetAllowsReuseOnKeepAlive) {
    Buffer buf;
    feed(buf, "GET /first HTTP/1.1\r\n\r\nGET /second HTTP/1.1\r\n\r\n");

    HttpRequest req;
    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.path(), "/first");

    req.reset();
    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.path(), "/second");
}
