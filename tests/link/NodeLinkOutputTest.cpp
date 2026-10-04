// NodeLink 输出队列测试：以真实 PTY 验证字节顺序、EPOLLOUT 续写和串口切换。
// 对指定 fd 的 write 增加测试预算，确定性制造短写与 EAGAIN；其他 fd 仍调用真实系统调用。

#include "gateway/link/NodeLink.h"
#include "gateway/io/event/EventLoop.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <memory>
#include <string>
#include <vector>

using namespace gateway;

namespace {
std::atomic<int> limited_fd{-1}; // 日志线程也会调用 write，观察测试开关时不能产生数据竞争。
std::size_t write_budget = 0;  // 本轮允许真实 write 接受的字节总数。
} // namespace

extern "C" ssize_t __real_write(int fd, const void* data, size_t len);

// 链接器 --wrap=write 只影响本测试目标，产品代码不需要暴露专用测试接口。
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t len) {
    if (fd != limited_fd.load(std::memory_order_relaxed)) return __real_write(fd, data, len);
    if (write_budget == 0) {
        errno = EAGAIN;
        return -1;
    }
    const ssize_t n = __real_write(fd, data, std::min(len, write_budget));
    if (n > 0) write_budget -= static_cast<std::size_t>(n);
    return n;
}

namespace {

// Pty 独占非阻塞主端，从端路径交给 NodeLink；主端用于逐字节核对实际输出。
struct Pty {
    int master = -1;
    std::string slave_path;

    Pty() {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (master == -1) return;
        if (grantpt(master) != 0 || unlockpt(master) != 0) return;
        const char* name = ptsname(master);
        if (name) slave_path = name;
    }
    ~Pty() { if (master != -1) close(master); }
    bool ok() const { return master != -1 && !slave_path.empty(); }
};

// 将当前所有可读字节追加到输出；遇到 EAGAIN 返回，不在读取路径等待后续数据。
bool drainMaster(int fd, std::vector<uint8_t>& bytes) {
    uint8_t buf[4096]; // 单次读取的固定缓冲区，保留多帧之间的原始字节顺序。
    while (true) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) bytes.insert(bytes.end(), buf, buf + n);
        else if (n < 0 && errno == EINTR) continue;
        else return n < 0 && errno == EAGAIN;
    }
}

// 小帧写完后允许 PTY 的线路规程完成数据交付，等待只出现在测试观察端。
bool readUntil(int fd, std::vector<uint8_t>& bytes, std::size_t size) {
    while (bytes.size() < size) {
        struct pollfd ready { fd, POLLIN, 0 };
        if (poll(&ready, 1, 1000) <= 0 || !drainMaster(fd, bytes)) return false;
    }
    return true;
}

class NodeLinkOutput : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pty_.ok()) << "无法创建 PTY，不能跳过真实串口语义验证";
        link_ = std::make_unique<NodeLink>(pty_.slave_path, 115200);
    }
    void TearDown() override { limited_fd = -1; }

    void allowBytes(std::size_t size) {
        limited_fd = link_->fd();
        write_budget = size;
    }

    Pty pty_;                        // 先创建主端，最后销毁，保证从端的完整生命周期。
    std::unique_ptr<NodeLink> link_;  // 每个用例独立的输出队列和解析状态。
};

} // namespace

// 入队不代表发送完成；只有整帧写出后回调一次，重复 flush 不得重复完成通知。
TEST_F(NodeLinkOutput, CompletionWaitsForWholeFrame) {
    int completed = 0;
    const std::vector<uint8_t> payload = {0x11};
    const auto expected = buildFrame(EDGE_TYPE_QUERY_TH, payload);
    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_TH, payload, [&](bool sent) {
        EXPECT_TRUE(sent);
        completed++;
    }));
    EXPECT_EQ(completed, 0);
    EXPECT_TRUE(link_->hasPendingOutput());

    link_->flushOutput();
    link_->flushOutput();
    EXPECT_EQ(completed, 1);
    EXPECT_FALSE(link_->hasPendingOutput());
    std::vector<uint8_t> received;
    ASSERT_TRUE(readUntil(pty_.master, received, expected.size()));
    EXPECT_EQ(received, expected);
}

// 第一帧只写出三个字节时，第二帧不得越过它；续写必须从原偏移继续。
TEST_F(NodeLinkOutput, ShortWritePreservesPrefixAndFrameOrder) {
    std::vector<int> completed; // 完成通知的顺序须与整帧 FIFO 顺序一致。
    std::vector<uint8_t> expected;
    for (uint8_t seq = 0; seq < 2; ++seq) {
        const auto frame = buildFrame(EDGE_TYPE_QUERY_LIGHT, {seq});
        expected.insert(expected.end(), frame.begin(), frame.end());
        ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_LIGHT, {seq}, [&, seq](bool sent) {
            EXPECT_TRUE(sent);
            completed.push_back(seq);
        }));
    }
    allowBytes(3);
    link_->flushOutput();
    EXPECT_TRUE(completed.empty());
    EXPECT_TRUE(link_->hasPendingOutput());
    std::vector<uint8_t> received;
    ASSERT_TRUE(readUntil(pty_.master, received, 3));
    ASSERT_EQ(received.size(), 3u);

    limited_fd = -1;
    link_->flushOutput();
    ASSERT_TRUE(readUntil(pty_.master, received, expected.size()));
    EXPECT_EQ(received, expected);
    EXPECT_EQ(completed, (std::vector<int>{0, 1}));
    EXPECT_FALSE(link_->hasPendingOutput());
}

// 尚未写出首字节的重试可被 ACK 撤销，后续帧仍应继续发送。
TEST_F(NodeLinkOutput, CancelledUnstartedFrameIsSkipped) {
    bool valid = true;
    int cancelled = 0;
    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_LIGHT, {0x01}, [&](bool sent) {
        EXPECT_FALSE(sent);
        cancelled++;
    }, [&] { return valid; }));
    allowBytes(0);
    link_->flushOutput();
    EXPECT_EQ(cancelled, 0);

    valid = false;
    limited_fd = -1;
    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_TH, {0x02}));
    link_->flushOutput();
    EXPECT_EQ(cancelled, 1);
    EXPECT_FALSE(link_->hasPendingOutput());
    const auto expected = buildFrame(EDGE_TYPE_QUERY_TH, {0x02});
    std::vector<uint8_t> received;
    ASSERT_TRUE(readUntil(pty_.master, received, expected.size()));
    EXPECT_EQ(received, expected);
}

// 已写出前缀后即使收到 ACK，也必须补齐本帧，避免下一个帧头被当成旧 payload。
TEST_F(NodeLinkOutput, PartiallyWrittenFrameFinishesAfterCancellation) {
    bool valid = true;
    int completed = 0;
    ASSERT_TRUE(link_->send(EDGE_TYPE_SET_PERIOD, {0x01, 0x00, 0x02}, [&](bool sent) {
        EXPECT_TRUE(sent);
        completed++;
    }, [&] { return valid; }));
    allowBytes(4);
    link_->flushOutput();
    valid = false;
    limited_fd = -1;
    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_LIGHT, {0x02}));
    link_->flushOutput();

    auto expected = buildFrame(EDGE_TYPE_SET_PERIOD, {0x01, 0x00, 0x02});
    const auto next = buildFrame(EDGE_TYPE_QUERY_LIGHT, {0x02});
    expected.insert(expected.end(), next.begin(), next.end());
    std::vector<uint8_t> received;
    ASSERT_TRUE(readUntil(pty_.master, received, expected.size()));
    EXPECT_EQ(received, expected);
    EXPECT_EQ(completed, 1);
}

// 不可恢复写错误要结束所有排队帧，避免它们永远占用序号或被重复通知失败。
TEST_F(NodeLinkOutput, WriteErrorFailsPendingFramesOnce) {
    int failed = 0;
    for (uint8_t seq = 0; seq < 2; ++seq) {
        ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_TH, {seq}, [&](bool sent) {
            EXPECT_FALSE(sent);
            failed++;
        }));
    }
    close(pty_.master);
    pty_.master = -1;
    link_->flushOutput();
    link_->flushOutput();
    EXPECT_EQ(failed, 2);
    EXPECT_FALSE(link_->hasPendingOutput());
}

// 候选串口打开失败时，旧帧的短写偏移与完成回调必须继续有效。
TEST_F(NodeLinkOutput, FailedReopenPreservesPendingOutput) {
    int completed = 0;
    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_TH, {0x01}, [&](bool sent) {
        EXPECT_TRUE(sent);
        completed++;
    }));
    allowBytes(3);
    link_->flushOutput();
    EXPECT_THROW(link_->reopen("/dev/definitely_not_a_tty_9f3a", 115200), std::exception);
    EXPECT_EQ(completed, 0);

    limited_fd = -1;
    link_->flushOutput();
    const auto expected = buildFrame(EDGE_TYPE_QUERY_TH, {0x01});
    std::vector<uint8_t> received;
    ASSERT_TRUE(readUntil(pty_.master, received, expected.size()));
    EXPECT_EQ(received, expected);
    EXPECT_EQ(completed, 1);
}

// 串口切换不能把旧帧后缀带入新字节流，旧输出应收到一次失败通知。
TEST_F(NodeLinkOutput, SuccessfulReopenDiscardsOldPartialOutput) {
    Pty next;
    ASSERT_TRUE(next.ok());
    int failed = 0;
    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_TH, {0x01}, [&](bool sent) {
        EXPECT_FALSE(sent);
        failed++;
    }));
    allowBytes(3);
    link_->flushOutput();
    limited_fd = -1;
    link_->reopen(next.slave_path, 115200);
    EXPECT_EQ(failed, 1);
    EXPECT_FALSE(link_->hasPendingOutput());

    ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_LIGHT, {0x02}));
    link_->flushOutput();
    const auto expected = buildFrame(EDGE_TYPE_QUERY_LIGHT, {0x02});
    std::vector<uint8_t> received;
    ASSERT_TRUE(readUntil(next.master, received, expected.size()));
    EXPECT_EQ(received, expected);
}

// 不注入 write：填满真实 PTY 缓冲后，由同一 Reactor 的读写事件逐步排空全部帧。
TEST_F(NodeLinkOutput, RealBackpressureResumesThroughEpoll) {
    constexpr int kFrames = 1024; // 总字节数超过 PTY 容量，确保出现真实 EAGAIN。
    int completed = 0;
    std::vector<uint8_t> expected;
    for (int i = 0; i < kFrames; ++i) {
        std::vector<uint8_t> payload(EDGE_PAYLOAD_MAX, static_cast<uint8_t>(i));
        const auto frame = buildFrame(EDGE_TYPE_QUERY_RESP, payload);
        expected.insert(expected.end(), frame.begin(), frame.end());
        ASSERT_TRUE(link_->send(EDGE_TYPE_QUERY_RESP, payload, [&](bool sent) {
            EXPECT_TRUE(sent);
            completed++;
        }));
    }
    link_->flushOutput();
    ASSERT_TRUE(link_->hasPendingOutput());
    ASSERT_LT(completed, kFrames);

    EventLoop loop; // 同时驱动串口续写、观察端排空和防挂死定时器。
    auto serial = std::make_shared<channel>();
    serial->fd = link_->fd();
    serial->owns_fd = false;
    serial->events = EPOLLOUT | EPOLLET;
    serial->on_write = [&] {
        link_->flushOutput();
        if (!link_->hasPendingOutput()) {
            serial->events &= ~EPOLLOUT;
            loop.modifyChannel(serial.get());
        }
    };
    loop.addChannel(serial);

    std::vector<uint8_t> received;
    auto reader = std::make_shared<channel>();
    reader->fd = pty_.master;
    reader->owns_fd = false;
    reader->events = EPOLLIN | EPOLLET;
    reader->on_read = [&] {
        ASSERT_TRUE(drainMaster(pty_.master, received));
        if (received.size() == expected.size() && !link_->hasPendingOutput()) loop.quit();
    };
    loop.addChannel(reader);

    bool timed_out = false;
    auto watchdog = std::make_shared<channel>();
    watchdog->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    ASSERT_GE(watchdog->fd, 0);
    struct itimerspec timeout {};
    timeout.it_value.tv_sec = 3;
    ASSERT_EQ(timerfd_settime(watchdog->fd, 0, &timeout, nullptr), 0);
    watchdog->events = EPOLLIN;
    watchdog->on_read = [&] { timed_out = true; loop.quit(); };
    loop.addChannel(watchdog);

    loop.loop();
    EXPECT_FALSE(timed_out) << "真实 EPOLLOUT 应在观察端释放容量后继续驱动输出";
    EXPECT_EQ(completed, kFrames);
    EXPECT_EQ(received, expected);
    EXPECT_FALSE(link_->hasPendingOutput());
    EXPECT_EQ(serial->events & EPOLLOUT, 0u);
}
