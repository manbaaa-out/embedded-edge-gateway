// HttpRequest 流式状态机测试：Buffer 模拟 TCP 接收缓冲，覆盖完整请求、半包、流水线
// 与 keep-alive 复用；本文件不启动 socket 或 EventLoop。

#include "gateway/io/http/Buffer.h"
#include "gateway/io/http/HttpRequest.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace gateway;

namespace {
// 将以 NUL 结尾的测试文本 s 按原始字节追加到 buf；strlen 只用于这些无内嵌 NUL 的样例。
void feed(Buffer& buf, const char* s) {
    buf.append(s, std::strlen(s));
}
} // namespace

// 完整 POST 请求应一次解析完成，并分别保存请求行、首部和指定长度的 body。
TEST(HttpRequest, ParsesCompleteRequestWithBody) {
    HttpRequest req; // 被测解析状态及最终请求字段。
    Buffer buf;      // 模拟单次 read 已取得的完整字节流。
    feed(buf, "POST /submit HTTP/1.1\r\n"
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

// 首次输入停在首部字段中间，后续字节应延续同一解析状态。
TEST(HttpRequest, HandlesPartialThenComplete) {
    HttpRequest req; // 两次 parse 调用之间保留中间状态。
    Buffer buf;      // 第二次 feed 追加到同一接收缓冲。

    feed(buf, "GET /index.html HTTP/1.1\r\nHo"); // 故意在 Host 字段名中间截断。
    EXPECT_EQ(req.parse(&buf), ParseResult::kIncomplete);

    feed(buf, "st: a.com\r\n\r\n");
    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.path(), "/index.html");
    EXPECT_EQ(req.getHeader("Host"), "a.com");
}

// 非十进制 Content-Length 无法确定消息边界，必须进入错误状态而非无限等待。
TEST(HttpRequest, RejectsMalformedContentLength) {
    HttpRequest req; // 接收错误状态的解析器。
    Buffer buf;      // 包含非法长度首部的完整请求。
    feed(buf, "POST /x HTTP/1.1\r\n"
              "Content-Length: abc\r\n"
              "\r\n");

    EXPECT_EQ(req.parse(&buf), ParseResult::kError) << "非法长度应报错而不是崩或挂起";
}

// 未声明 Content-Length 的 GET 在空行处完成，body 保持为空。
TEST(HttpRequest, ParsesRequestWithoutBody) {
    HttpRequest req; // 被测无正文请求。
    Buffer buf;      // 请求行、Host 和首部终止符。
    feed(buf, "GET / HTTP/1.1\r\nHost: a.com\r\n\r\n");

    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.method(), "GET");
    EXPECT_EQ(req.body(), "");
}

// 首部字段名匹配不区分大小写。
TEST(HttpRequest, HeaderKeysAreCaseInsensitive) {
    HttpRequest req; // 应归一化首部字段名的解析器。
    Buffer buf;      // 使用全大写 CONTENT-LENGTH 的请求。
    feed(buf, "POST /x HTTP/1.1\r\n"
              "CONTENT-LENGTH: 3\r\n"
              "\r\n"
              "abc");

    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.body(), "abc") << "归一化失效会导致 body 长度识别不出来";
}

// 两个连续请求由两个独立 HttpRequest 消费，共享 Buffer 只移除已完成的第一个请求。
TEST(HttpRequest, ParsesPipelinedRequestsWithSeparateObjects) {
    Buffer buf; // 同时容纳两个完整 HTTP 请求的流水线字节流。
    feed(buf, "GET /first HTTP/1.1\r\n\r\nGET /second HTTP/1.1\r\n\r\n");

    HttpRequest req1; // 消费第一段请求。
    ASSERT_EQ(req1.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req1.path(), "/first");

    HttpRequest req2; // 从剩余缓冲继续消费第二段请求。
    ASSERT_EQ(req2.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req2.path(), "/second");
}

// reset 清除上一请求状态，但保留缓冲区中的流水线后续请求。
TEST(HttpRequest, ResetAllowsReuseOnKeepAlive) {
    Buffer buf; // 同一连接中已接收的两个请求。
    feed(buf, "GET /first HTTP/1.1\r\n\r\nGET /second HTTP/1.1\r\n\r\n");

    HttpRequest req; // reset 后复用同一状态机对象。
    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.path(), "/first");

    req.reset();
    ASSERT_EQ(req.parse(&buf), ParseResult::kComplete);
    EXPECT_EQ(req.path(), "/second");
}
