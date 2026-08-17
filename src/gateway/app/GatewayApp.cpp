/**
 * @file GatewayApp.cpp
 * @brief 实现网关资源装配、事件源注册和跨线程事件汇聚。
 *
 * 主 Reactor 是串口和 CommandTracker 的唯一访问线程。MQTT 网络线程不直接写串口，
 * 而是把翻译后的命令写入 cmd_queue_，再用 eventfd 唤醒 Reactor。命令发送、重试和
 * ACK 销账因此严格串行。启动阶段无法创建必需资源时抛出异常；SIGHUP 重载期间则按
 * 数据库、MQTT、串口三个资源分别捕获错误，让未受影响的路径继续运行。
 */

#include "gateway/app/GatewayApp.h"

#include "gateway/core/config/Config.h"
#include "gateway/core/format/Number.h"
#include "gateway/core/log/Logger.h"
#include "gateway/io/http/HttpServer.h"
#include "gateway/pipeline/TelemetryDecoder.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

namespace gateway {

namespace {
/** 命令超时扫描间隔，单位为纳秒；小于单次 ACK 等待时间以控制检测延迟。 */
constexpr long kCmdScanIntervalNs = 200L * 1000 * 1000;

/** 自定义 ACK 和查询结果的 MQTT QoS；1 表示 broker 至少接收一次。 */
constexpr int kMqttCommandResultQos = 1;
}  // namespace

/**
 * @brief 填充由网关主循环管理的 Unix 信号集合。
 * @param mask 输出参数；函数会先清空，再加入热加载和两种退出信号。
 *
 * blockManagedSignals() 与 setupSignal() 共用该函数，确保屏蔽集合和 signalfd
 * 监听集合始终一致。
 */
static void fillManagedSignals(sigset_t& mask) {
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
}

bool GatewayApp::blockManagedSignals() {
    // mask 保存调用线程及其后继线程需要屏蔽的信号集合。
    sigset_t mask;
    fillManagedSignals(mask);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        LOG_WARN("sigprocmask failed: %s — 热加载与优雅停机不可用", strerror(errno));
        return false;
    }
    return true;
}

GatewayApp::GatewayApp() {
    // cfg 是构造期间使用的一致配置快照，避免同一批资源读取到不同版本。
    auto cfg = ConfigManager::current();

    // db 是写连接的临时所有者；创建 schema 后立即移交给 TelemetryPipeline。
    auto db = std::make_shared<Database>(cfg->db_path);
    LOG_INFO("sqlite(rw) opened at %s", cfg->db_path.c_str());

    // client_ 此时仅完成同步连接准备，消息回调和网络循环在 run() 中安装并启动。
    client_ = std::make_shared<MqttClient>("gateway-main", cfg->mqtt_host, cfg->mqtt_port,
                                           cfg->mqtt_keepalive);

    // pipeline_ 接管 db，并启动独占该写连接的后台线程。
    pipeline_.emplace(std::move(db));

    // link_ 以非阻塞模式打开 STM32 串口，后续只在主线程访问。
    link_ = std::make_unique<NodeLink>(cfg->serial_path, cfg->serial_baud);

    // roDb_ 是 HTTP 当前只读连接；HTTP 请求通过原子 shared_ptr 快照取得所有权。
    roDb_        = std::make_shared<Database>(cfg->db_path, true);

    // 裸 std::thread 最后启动，避免构造函数随后抛异常时析构 joinable 线程。
    // port 固定为启动端口；database 提供器则让每个请求读取当前生效的只读连接。
    http_thread_ = std::thread([this, port = cfg->http_port] {
        runHttpServer(
            [this] {
                return std::atomic_load_explicit(&roDb_, std::memory_order_acquire);
            },
            port,
            [] {
                // c 是每次 HTTP 定时检查或查询时读取的最新运行配置快照。
                auto c = ConfigManager::current();
                return HttpRuntimeConfig{c->idle_timeout, c->report_n};
            },
            [this] { return http_stop_.load(std::memory_order_relaxed); });
    });
    LOG_INFO("http monitor thread started on :%d", cfg->http_port);
}

GatewayApp::~GatewayApp() {
    // http_stop_ 是 HTTP 线程轮询的退出请求，不负责停止其他后台组件。
    http_stop_.store(true, std::memory_order_relaxed);
    if (http_thread_.joinable()) {
        // 回调捕获 this，必须在任何成员销毁前结束 HTTP 线程。
        http_thread_.join();
    }
}

void GatewayApp::onFrame(const Frame& f) {
    // f 是 FrameParser 已完成帧头、长度和 CRC 校验后的上行帧。
    if (f.type == EDGE_TYPE_QUERY_RESP || f.type == EDGE_TYPE_ACK) {
        onAckFrame(f);
        return;
    }

    LOG_DEBUG("frame type=0x%02X len=%zu", f.type, f.payload.size());
    // ts 是本批遥测的 Unix 秒时间戳，取收帧时刻而非后台实际写库时刻。
    const long ts       = static_cast<long>(time(nullptr));

    // readings 是一帧拆分出的标准化指标集合；未知或非法业务帧会得到空集合。
    const auto readings = decodeTelemetry(f);

    // r 表示单个设备指标；MQTT topic 由固定前缀和逻辑设备名组成。
    for (const auto& r : readings) {
        client_->publish("gateway/up/" + r.device, formatValue(r.value));
    }

    // 写队列满时丢弃本批数据，避免阻塞串口 Reactor。
    if (!pipeline_->submit(readings, ts)) {
        LOG_WARN("telemetry queue full, dropped %zu readings", readings.size());
    }
}

void GatewayApp::onAckFrame(const Frame& f) {
    // edge_payload_len_ok 在读取 seq/rc 前验证该响应至少包含协议规定的公共前缀。
    if (!edge_payload_len_ok(f.type, static_cast<uint8_t>(f.payload.size()))) {
        LOG_WARN("ACK payload too short: %zu, drop", f.payload.size());
        return;
    }
    const uint8_t seq = f.payload[EDGE_OFF_SEQ];  // 对应原下行命令的 8 位序列号。
    const uint8_t rc  = f.payload[EDGE_OFF_RC];   // STM32 返回的协议结果码。

    if (!tracker_.onAck(seq)) {
        LOG_WARN("ACK for unknown seq=%u, ignored (late/dup/spurious)", seq);
        return;
    }
    LOG_INFO("ACK matched seq=%u rc=0x%02X, inflight=%zu", seq, rc, tracker_.inflightCount());

    // seq_str 用作 MQTT 结果 topic 的末级，使云端能按序列号匹配结果。
    const std::string seq_str = std::to_string(seq);
    if (rc != EDGE_RC_OK) {
        client_->publish("gateway/ack/" + seq_str, "err," + std::to_string(rc),
                         kMqttCommandResultQos);
        return;
    }

    if (f.type != EDGE_TYPE_QUERY_RESP) {
        client_->publish("gateway/ack/" + seq_str, "ok", kMqttCommandResultQos);
        return;
    }

    // data_len 不含公共的 seq 和 rc。tracker 未保存查询种类，因此按数据长度分派。
    const std::size_t data_len = f.payload.size() - 2;
    if (data_len >= 4) {
        // t/h 分别是按协议缩放系数还原后的温度和湿度浮点值。
        const double t = edge_u16_be_read(&f.payload[2]) / double(EDGE_TEMP_SCALE);
        const double h = edge_u16_be_read(&f.payload[4]) / double(EDGE_HUMI_SCALE);
        client_->publish("gateway/resp/" + seq_str,
                         "ok," + formatValue(t) + "," + formatValue(h),
                         kMqttCommandResultQos);
    } else if (data_len >= 2) {
        client_->publish("gateway/resp/" + seq_str,
                         "ok," + std::to_string(edge_u16_be_read(&f.payload[2])),
                         kMqttCommandResultQos);
    } else {
        client_->publish("gateway/resp/" + seq_str, "ok,", kMqttCommandResultQos);
    }
}

void GatewayApp::onDownlinkWakeup() {
    // cnt 接收自上次读取以来累计的 eventfd 通知次数；次数不等同于命令条数。
    uint64_t cnt = 0;
    if (::read(evfd_, &cnt, sizeof(cnt)) != sizeof(cnt)) {
        // eventfd 只负责唤醒；命令队列才是待处理数据源。
        LOG_DEBUG("%s", "eventfd read returned short");
    }

    // 发送与重试都在主线程执行，串口始终只有一个写入者。
    // 当前在途数未设上限；新增非幂等命令前必须同时限制并发并扩大去重窗口。
    // cmd 是 try_pop 返回的可选命令所有权；循环持续到队列为空。
    while (auto cmd = cmd_queue_.try_pop()) {
        // seq 是本次发送分配的 8 位线端标识，ACK 和重试都复用它。
        const uint8_t seq = tracker_.nextSeq();

        // payload 是线端负载：[seq][命令参数...]，帧头、类型和 CRC 由 NodeLink 添加。
        std::vector<uint8_t> payload;
        payload.reserve(1 + cmd->arg.size());
        payload.push_back(seq);
        payload.insert(payload.end(), cmd->arg.begin(), cmd->arg.end());

        if (link_->send(cmd->type, payload)) {
            LOG_INFO("downlink sent: type=0x%02X seq=%u", cmd->type, seq);
            tracker_.trackWithSeq(seq, cmd->type, cmd->arg, CommandTracker::Clock::now());
        } else {
            // 未写入串口的命令不进入等待 ACK 的在途表。
            LOG_WARN("downlink send failed: type=0x%02X seq=%u, not tracked", cmd->type, seq);
        }
    }
}

void GatewayApp::onCmdTimer() {
    // exp 接收 timerfd 合并后的到期次数；一次 tick 即可按当前时刻处理全部命令。
    uint64_t exp = 0;
    if (::read(cmd_timerfd_, &exp, sizeof(exp)) != sizeof(exp)) {
        return;
    }

    // actions 是本轮状态推进产生的两类终局：需要重发与已经耗尽重试次数。
    const auto actions = tracker_.tick(CommandTracker::Clock::now());

    // r 描述一条待重发命令，包括原序列号、类型、参数和当前尝试次数。
    for (const auto& r : actions.resend) {
        // payload 重新构造线端负载，但保留 r.seq 以支持节点侧幂等去重。
        std::vector<uint8_t> payload;
        payload.reserve(1 + r.arg.size());
        payload.push_back(r.seq);
        payload.insert(payload.end(), r.arg.begin(), r.arg.end());

        if (link_->send(r.type, payload)) {
            LOG_WARN("downlink RETRY seq=%u type=0x%02X (attempt %d/%u)",
                     r.seq, r.type, r.attempt, EDGE_MAX_RETRY);
        } else {
            LOG_WARN("downlink retry send failed seq=%u", r.seq);
        }
    }

    // f 描述不再重试的命令；其 timeout 是网关自定义的业务终局响应。
    for (const auto& f : actions.failed) {
        LOG_ERROR("downlink FAILED seq=%u type=0x%02X: no ACK after %u retries",
                  f.seq, f.type, EDGE_MAX_RETRY);
        client_->publish("gateway/ack/" + std::to_string(f.seq), "timeout",
                         kMqttCommandResultQos);
    }
}

void GatewayApp::reloadConfig() {
    LOG_INFO("%s", "SIGHUP received, reloading config...");

    // r 同时表示解析是否成功以及相对上一份配置需要重建的资源集合。
    const auto r = ConfigManager::reload();
    if (!r.ok) {
        LOG_WARN("%s", "reload failed, keep running with old config");
        return;
    }
    // ncfg 是 ConfigManager 已发布的新快照，供本次各资源重建读取一致参数。
    // 发布发生在资源应用之前，因此某分支失败时“配置值”和“实际资源”可能暂时不同。
    auto ncfg = ConfigManager::current();

    // 日志级别只涉及原子内存状态，无需重建外部资源即可立即生效。
    Logger::setLevel(static_cast<LogLevel>(ncfg->log_level));

    // all_applied 汇总外部资源是否全部生效，只用于最终日志，不参与回滚。
    bool all_applied = true;

    if (r.db_changed) {
        LOG_INFO("%s", "db_path changed → reopening database");
        try {
            // 先把新库的读写连接全部构造成功，再向写线程提交切换，避免只切换一侧。
            auto write_db = std::make_shared<Database>(ncfg->db_path);
            auto read_db  = std::make_shared<Database>(ncfg->db_path, true);
            const std::string new_path = ncfg->db_path;

            // 切库任务与遥测批次共用队列。写线程先刷完旧库数据并替换写连接，再通过
            // 回调原子发布 read_db；正在查询的 HTTP 请求继续持有旧连接，后续请求取新连接。
            if (!pipeline_->swapDatabase(
                    std::move(write_db), [this, read_db = std::move(read_db), new_path] {
                        std::atomic_store_explicit(&roDb_, read_db, std::memory_order_release);
                        LOG_INFO("http reader switched to new database: %s", new_path.c_str());
                    })) {
                all_applied = false;
                LOG_ERROR("%s", "db swap not delivered (write queue full), read and write remain "
                                "on the old database");
            }
        } catch (const std::exception& e) {
            // 任一候选连接创建失败都不会提交切换，HTTP 与写管线继续使用旧库。
            all_applied = false;
            LOG_ERROR("db reopen failed, keep reading and writing the old database: %s", e.what());
        }
    }
    if (r.mqtt_changed) {
        LOG_INFO("%s", "mqtt config changed → reconnecting");
        try {
            // fresh 是候选 MQTT 连接；同步构造成功后才替换当前 client_。
            auto fresh = std::make_shared<MqttClient>("gateway-main", ncfg->mqtt_host,
                                                      ncfg->mqtt_port, ncfg->mqtt_keepalive);
            client_ = std::move(fresh);
            // 每个 MqttClient 都有独立订阅表；换到新 broker 后必须重新登记并启动循环。
            startMqtt();
        } catch (const std::exception& e) {
            // e 来自候选客户端构造或启动前配置；失败项只影响 MQTT 路径。
            all_applied = false;
            LOG_ERROR("mqtt rebuild failed, uplink may be degraded: %s", e.what());
        }
    }
    if (r.serial_changed) {
        // Channel 绑定具体 fd，换串口时必须从 epoll 移除后重新创建。
        LOG_INFO("%s", "serial config changed → reopening port");
        loop_.removeChannel(serial_channel_->fd);
        try {
            link_->reopen(ncfg->serial_path, ncfg->serial_baud);
        } catch (const std::exception& e) {
            // e 描述新串口打开/配置错误；reopen 的临时候选失败时旧 port_ 保持有效。
            all_applied = false;
            LOG_ERROR("serial reopen failed, staying on the previous port: %s", e.what());
            LOG_WARN("effective serial path is still the old one, not '%s'",
                     ncfg->serial_path.c_str());
        }
        // reopen 失败时 NodeLink 仍持有旧 fd，因此同一路径也能恢复旧注册。
        serial_channel_ = makeSerialChannel();
        loop_.addChannel(serial_channel_);
    }

    LOG_INFO("reload done%s", all_applied ? "" : " (with degraded items, see ERROR above)");
}

std::shared_ptr<channel> GatewayApp::makeSerialChannel() {
    // ch 保存 epoll 关注事件和回调；shared_ptr 让 EventLoop 决定注册期生命周期。
    auto ch     = std::make_shared<channel>();
    ch->fd      = link_->fd();
    ch->events  = EPOLLIN | EPOLLET;
    ch->on_read = [this] { link_->drainAndParse(); };
    // SerialPort 拥有该 fd；Channel 只负责监听，不能重复关闭。
    ch->owns_fd = false;
    return ch;
}

void GatewayApp::setupSerial() {
    // f 是 NodeLink 解析完成后短暂传入的帧引用，处理在同一 Reactor 回调中完成。
    link_->setFrameHandler([this](const Frame& f) { onFrame(f); });
    serial_channel_ = makeSerialChannel();
    loop_.addChannel(serial_channel_);
}

bool GatewayApp::setupDownlink() {
    // evfd_ 使用非阻塞计数器语义；初值 0 表示启动时没有待处理通知。
    evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd_ == -1) {
        LOG_ERROR("eventfd failed: %s", strerror(errno));
        return false;
    }
    // ch 拥有 evfd_，并把可读事件映射为队列消费回调。
    auto ch     = std::make_shared<channel>();
    ch->fd      = evfd_;
    ch->events  = EPOLLIN;
    ch->on_read = [this] { onDownlinkWakeup(); };
    loop_.addChannel(ch);
    return true;
}

void GatewayApp::startMqtt() {
    // cfg 仅用于记录当前连接端点；客户端本身已在构造时保存连接参数。
    auto cfg = ConfigManager::current();

    // 回执绑定到接收命令的连接，避免重载时从网络线程并发读取 client_。
    // MqttClient 析构会先停止并等待网络线程，保证 owner 在回调期间有效。
    MqttClient* owner = client_.get();

    // MQTT 回调只翻译和入队；eventfd 唤醒主线程完成实际串口发送。
    // topic 是 broker 提供的完整主题，payload 是未经解释的消息字节串。
    client_->setMessageHandler([this, owner](const std::string& topic,
                                             const std::string& payload) {
        // r 包含翻译后的 DownCmd，或供云端诊断的拒绝原因。
        auto r = translateCommand(topic, payload);
        if (!r.ok) {
            LOG_WARN("downlink rejected: %s", r.error.c_str());
            owner->publish("gateway/ack/rejected", r.error, kMqttCommandResultQos);
            return;
        }
        cmd_queue_.push(std::move(r.cmd));

        // one 是写入 eventfd 的单次增量；计数可合并，主线程会一次排空队列。
        const uint64_t one = 1;
        if (::write(evfd_, &one, sizeof(one)) != sizeof(one)) {
            LOG_WARN("%s", "eventfd notify failed");
        }
    });

    // QoS 1 用于云端命令。该调用只登记逻辑订阅；MqttClient 会在首次连接及以后
    // 每次 CONNACK 时发送 SUBSCRIBE，所以 clean session 重连和新 broker 都走同一路径。
    client_->subscribe("gateway/cmd/#", 1);
    if (!client_->loopStart()) {
        LOG_ERROR("%s", "mqtt loop start failed, uplink and downlink are both dead");
        return;
    }
    LOG_INFO("mqtt connected: %s:%d", cfg->mqtt_host.c_str(), cfg->mqtt_port);
}

void GatewayApp::setupCmdTimer() {
    // cmd_timerfd_ 使用单调时钟，避免系统时间校准改变命令超时判断。
    cmd_timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (cmd_timerfd_ == -1) {
        LOG_WARN("cmd timerfd failed: %s — 超时重试不可用", strerror(errno));
        return;
    }
    // cts 同时定义首次触发和后续周期；秒字段保持 0，纳秒字段取扫描间隔。
    struct itimerspec cts {};
    cts.it_value.tv_nsec    = kCmdScanIntervalNs;
    cts.it_interval.tv_nsec = kCmdScanIntervalNs;
    timerfd_settime(cmd_timerfd_, 0, &cts, nullptr);

    // ch 拥有 timerfd，并在每次可读时推进全部在途命令状态。
    auto ch     = std::make_shared<channel>();
    ch->fd      = cmd_timerfd_;
    ch->events  = EPOLLIN;
    ch->on_read = [this] { onCmdTimer(); };
    loop_.addChannel(ch);
}

void GatewayApp::setupSignal() {
    // mask 与进程启动时屏蔽的集合一致，作为 signalfd 的输入过滤器。
    sigset_t mask;
    fillManagedSignals(mask);
    sfd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd_ == -1) {
        LOG_WARN("signalfd failed: %s — 热加载与优雅停机不可用", strerror(errno));
        return;
    }

    // ch 拥有 sfd_，并把异步信号转换为普通 Reactor 可读回调。
    auto ch     = std::make_shared<channel>();
    ch->fd      = sfd_;
    ch->events  = EPOLLIN;
    ch->on_read = [this] {
        // si 保存单个已排队信号的编号和内核元数据；当前逻辑只读取 ssi_signo。
        struct signalfd_siginfo si;
        // LT 模式下必须排空所有已排队信号。
        while (::read(sfd_, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
            switch (si.ssi_signo) {
            case SIGHUP:
                reloadConfig();
                break;
            case SIGTERM:
            case SIGINT:
                LOG_INFO("signal %u received, shutting down", si.ssi_signo);
                loop_.quit();
                break;
            default:
                LOG_WARN("unexpected signal %u on signalfd", si.ssi_signo);
                break;
            }
        }
    };
    loop_.addChannel(ch);
}

int GatewayApp::run() {
    // eventfd 必须先于 MQTT 网络线程建立，确保首条命令回调已有可用唤醒目标。
    setupSerial();
    if (!setupDownlink()) return 1;
    startMqtt();

    // 定时重试和信号控制属于可降级能力，创建失败时各函数记录错误后继续采集。
    setupCmdTimer();
    setupSignal();

    loop_.loop();

    LOG_INFO("%s", "main loop exited, shutting down");
    return 0;
}

}  // namespace gateway
