// HTTP 请求解析测试
//
// 由原 src/net/tests/http_test.cpp 移植而来 —— 那份用 assert + cout 写得不错,
// 但从未被任何 CMakeLists 引用过,等于没有。搬进 CTest 后它才真的在守着代码。

#include "gateway/io/http/Buffer.h"
#include "gateway/io/http/HttpRequest.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace gateway;

namespace {
// 把字符串灌进 Buffer,省掉每个用例都写一遍 strlen
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

// 半包:TCP 不保证一次 read 拿到整条请求,这是 HTTP 解析器的基本功
TEST(HttpRequest, HandlesPartialThenComplete) {
    HttpRequest req;
    Buffer      buf;

    feed(buf, "GET /index.html HTTP/1.1\r\nHo");          // 断在 header 名字中间
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

// 长连接:同一个 HttpRequest 对象 reset 后复用
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
