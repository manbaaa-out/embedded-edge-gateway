#include "gateway/app/GatewayCore.h"
#include "gateway/core/concurrent/TaskExecutor.h"

#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace gateway;
using namespace std::chrono_literals;

namespace {
struct TemporaryDatabase {
    std::string path;
    TemporaryDatabase() {
        char name[] = "/tmp/gateway-core-test-XXXXXX";
        const int fd = mkstemp(name);
        if (fd < 0) throw std::runtime_error("mkstemp failed");
        close(fd);
        path = name;
    }
    ~TemporaryDatabase() {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
};
class CoreTest : public testing::Test {
protected:
    TemporaryDatabase file;
    std::shared_ptr<Database> writer = std::make_shared<Database>(file.path);
    std::shared_ptr<Database> reader = std::make_shared<Database>(file.path, true);
    TelemetryPipeline pipeline{writer};
    std::unique_ptr<GatewayCore> make(GatewayCore::SubmitCommand submit,
                                    GatewayCore::Publish publish) {
        return std::make_unique<GatewayCore>(pipeline, std::move(submit), std::move(publish),
            [this] { return reader; }, [] { return HttpRuntimeConfig{5, 10}; });
    }
};

TEST_F(CoreTest, CommandTranslationAndResultMappingHaveOneBusinessOwner) {
    std::promise<std::pair<uint64_t, std::thread::id>> submitted;
    auto input = submitted.get_future();
    std::promise<std::tuple<std::string, std::string, std::thread::id>> published;
    auto output = published.get_future();
    std::atomic<int> publications{0};
    auto core = make([&](uint64_t id, DownCmd command) {
        EXPECT_EQ(command.type, EDGE_TYPE_QUERY_TH);
        submitted.set_value({id, std::this_thread::get_id()});
        return true;
    }, [&](std::string topic, std::string body, int qos, std::function<void()> done) {
        EXPECT_EQ(qos, 1);
        ++publications;
        published.set_value({std::move(topic), std::move(body), std::this_thread::get_id()});
        if (done) done();
        return true;
    });
    ASSERT_TRUE(core->mqttMessage("gateway/cmd/query_th", ""));
    ASSERT_EQ(input.wait_for(3s), std::future_status::ready);
    const auto [id, owner] = input.get();
    EXPECT_NE(owner, std::this_thread::get_id());
    CommandCompletion result{7, EDGE_TYPE_QUERY_TH, {0, 0, 231, 1, 245}, {}};
    ASSERT_TRUE(core->commandFinished(id, result));
    ASSERT_TRUE(core->commandFinished(id, result)); // 迟到/重复事件不得重复完成业务。
    ASSERT_EQ(output.wait_for(3s), std::future_status::ready);
    const auto [topic, body, result_owner] = output.get();
    EXPECT_EQ(topic, "gateway/resp/7");
    EXPECT_EQ(body, "ok,23.1,50.1");
    EXPECT_EQ(result_owner, owner);
    core->stop();
    EXPECT_EQ(publications.load(), 1);
}

TEST_F(CoreTest, AdmissionCreditIncludesPendingTerminalPublication) {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<uint64_t> ids;
    std::function<void()> release;
    auto core = make([&](uint64_t id, DownCmd) {
        std::lock_guard<std::mutex> lock(mutex);
        ids.push_back(id);
        ready.notify_all();
        return true;
    }, [&](std::string, std::string, int, std::function<void()> done) {
        std::lock_guard<std::mutex> lock(mutex);
        release = std::move(done); // 模拟控制线程暂未执行 publish。
        ready.notify_all();
        return true;
    });
    for (std::size_t i = 0; i < GatewayCore::kMaxCommands; ++i)
        ASSERT_TRUE(core->mqttMessage("gateway/cmd/query_light", ""));
    EXPECT_FALSE(core->mqttMessage("gateway/cmd/query_light", ""));
    uint64_t id = 0;
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 3s, [&] { return ids.size() == GatewayCore::kMaxCommands; }));
        id = ids.front();
    }
    ASSERT_TRUE(core->commandFinished(id, {0, EDGE_TYPE_QUERY_LIGHT, {0, 0, 42}, {}}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 3s, [&] { return static_cast<bool>(release); }));
    }
    EXPECT_FALSE(core->mqttMessage("gateway/cmd/query_light", ""));
    release();
    EXPECT_TRUE(core->mqttMessage("gateway/cmd/query_light", ""));
    core->stop();
    EXPECT_FALSE(core->mqttMessage("gateway/cmd/query_light", ""));
}

TEST_F(CoreTest, QueryCompletionReturnsToBusinessThreadAndShutdownDrainsIt) {
    { Database seed(file.path); seed.insertBatch({{"temperature", 23.1, 1234}}); }
    std::promise<std::thread::id> submitted;
    auto owner = submitted.get_future();
    auto core = make([&](uint64_t, DownCmd) {
        submitted.set_value(std::this_thread::get_id()); return true;
    }, [](std::string, std::string, int, std::function<void()> done) {
        if (done) done();
        return true;
    });
    ASSERT_TRUE(core->mqttMessage("gateway/cmd/query_light", ""));
    ASSERT_EQ(owner.wait_for(3s), std::future_status::ready);
    const auto business_thread = owner.get();
    std::atomic<int> completed{0};
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(core->httpRequest({"GET", "/api/data?dev=tempera%74ure&n=1"},
            [&](HttpResponse response) {
                EXPECT_EQ(std::this_thread::get_id(), business_thread);
                EXPECT_EQ(response.status, 200u);
                EXPECT_EQ(response.body, "[{\"device_id\":\"temperature\",\"value\":23.1,\"ts\":1234}]");
                ++completed;
            }));
    }
    core->stop();
    EXPECT_EQ(completed.load(), 20);
    EXPECT_FALSE(core->httpRequest({"GET", "/"}, [](HttpResponse) {}));
}

TEST_F(CoreTest, InvalidCommandsAndRoutesAreHandledWithoutDeviceSubmission) {
    std::atomic<int> sends{0}, rejections{0}, replies{0};
    auto core = make([&](uint64_t, DownCmd) { ++sends; return true; },
        [&](std::string topic, std::string, int, std::function<void()> done) {
            EXPECT_EQ(topic, "gateway/ack/rejected");
            ++rejections;
            if (done) done();
            return true;
        });
    ASSERT_TRUE(core->mqttMessage("gateway/cmd/set_period", "0"));
    ASSERT_TRUE(core->mqttMessage("gateway/cmd/unknown", ""));
    ASSERT_TRUE(core->httpRequest({"POST", "/"}, [&](HttpResponse r) { EXPECT_EQ(r.status, 405u); ++replies; }));
    ASSERT_TRUE(core->httpRequest({"GET", "/api/data?dev=%zz"}, [&](HttpResponse r) { EXPECT_EQ(r.status, 400u); ++replies; }));
    ASSERT_TRUE(core->httpRequest({"GET", "/api/data"}, [&](HttpResponse r) { EXPECT_EQ(r.status, 400u); ++replies; }));
    core->stop();
    EXPECT_EQ(sends.load(), 0);
    EXPECT_EQ(rejections.load(), 2);
    EXPECT_EQ(replies.load(), 3);
}

TEST(TaskExecutor, CompletionReserveSurvivesOrdinaryQueueSaturationAndDrainsInOrder) {
    std::promise<void> entered, resume;
    auto entered_future = entered.get_future();
    auto gate = resume.get_future().share();
    std::vector<int> order;
    TaskExecutor executor("test-executor", 2, 1);
    EXPECT_TRUE(executor.post([&] { entered.set_value(); gate.wait(); order.push_back(0); }));
    const bool running = entered_future.wait_for(3s) == std::future_status::ready;
    EXPECT_TRUE(running);
    if (!running) { resume.set_value(); return; }
    EXPECT_TRUE(executor.post([&] { order.push_back(1); }));
    EXPECT_TRUE(executor.post([&] { order.push_back(2); }));
    EXPECT_FALSE(executor.post([] {}));
    EXPECT_TRUE(executor.post([&] { order.push_back(3); }, true));
    EXPECT_FALSE(executor.post([] {}, true));
    resume.set_value();
    executor.stop();
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
    EXPECT_FALSE(executor.post([] {}, true));
}
} // namespace
