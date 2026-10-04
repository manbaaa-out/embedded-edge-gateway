#pragma once

#include "gateway/cloud/CommandTranslator.h"
#include "gateway/core/concurrent/TaskExecutor.h"
#include "gateway/io/http/HttpServer.h"
#include "gateway/link/CommandTracker.h"
#include "gateway/pipeline/TelemetryPipeline.h"
#include "gateway/protocol/FrameCodec.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>

namespace gateway {

// 应用状态唯一所有者。I/O 线程只能投递拥有数据的事件，不调用业务处理函数。
class GatewayCore {
public:
    static constexpr std::size_t kMaxCommands = 512;
    static constexpr std::size_t kMaxQueries = 32;
    using SubmitCommand = std::function<bool(uint64_t, DownCmd)>;
    // done 在发布调用执行后释放命令准入额度；不表示 broker ACK 或云端业务完成。
    using Publish = std::function<bool(std::string, std::string, int, std::function<void()>)>;
    using DatabaseProvider = std::function<std::shared_ptr<Database>()>;

    GatewayCore(TelemetryPipeline& pipeline, SubmitCommand submit, Publish publish,
                DatabaseProvider database, HttpRuntimeConfigProvider config);
    ~GatewayCore();
    GatewayCore(const GatewayCore&) = delete;
    GatewayCore& operator=(const GatewayCore&) = delete;

    bool mqttMessage(std::string topic, std::string payload);
    bool telemetry(Frame frame, long received_at);
    bool httpRequest(HttpRequest request, HttpReply reply);
    // Reactor 为每个接收的命令只交付一次终态；预留邮箱空间由命令准入额度保证。
    bool commandFinished(uint64_t id, CommandCompletion result);
    bool commandRejected(uint64_t id, std::string error);
    void stop();

private:
    void handleMqtt(std::string topic, std::string payload);
    void handleTelemetry(const Frame& frame, long received_at);
    void handleHttp(HttpRequest request, HttpReply reply);
    void handleFinished(uint64_t id, const CommandCompletion& result);
    void reject(uint64_t id, const std::string& error);
    void releaseCommand();
    void publishCommand(std::string topic, std::string payload);

    TelemetryPipeline& pipeline_;
    SubmitCommand submit_;
    Publish publish_;
    DatabaseProvider database_;
    HttpRuntimeConfigProvider config_;
    std::shared_ptr<std::atomic<std::size_t>> admitted_ =
        std::make_shared<std::atomic<std::size_t>>(0);
    std::atomic<bool> accepting_{true};
    uint64_t next_id_ = 1;
    std::set<uint64_t> pending_; // 只在业务线程访问，ID 不依赖可回绕的线上 seq。
    std::size_t queries_ = 0;
    // Query 回调必须在 core 邮箱关闭前全部返回，stop 中用屏障建立此顺序。
    TaskExecutor core_{"gateway-core", 1024, kMaxCommands + kMaxQueries + 8};
    TaskExecutor queries_executor_{"gateway-query", kMaxQueries};
    bool stopped_ = false;
};

} // namespace gateway
