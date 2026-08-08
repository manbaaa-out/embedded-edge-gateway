#include "gateway/app/GatewayApp.h"

#include "gateway/core/config/Config.h"
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
// 超时扫描周期。必须明显小于 EDGE_ACK_TIMEOUT_MS(500ms),
// 否则一条命令可能在超时大半个周期后才被察觉。
constexpr long kCmdScanIntervalNs = 200L * 1000 * 1000;
}  // namespace

bool GatewayApp::blockReloadSignal() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        LOG_WARN("sigprocmask failed: %s — 热加载不可用", strerror(errno));
        return false;
    }
    return true;
}

// db_ / client_ 用 shared_ptr 而非 unique_ptr:线程池 task 需要捕获它们的快照,
// 见 TelemetryPipeline.h。
GatewayApp::GatewayApp() {
    auto cfg = ConfigManager::current();   // 启动配置快照

    db_ = std::make_shared<Database>(cfg->db_path);
    LOG_INFO("sqlite(rw) opened at %s", cfg->db_path.c_str());

    client_ = std::make_shared<MqttClient>("gateway-main", cfg->mqtt_host, cfg->mqtt_port,
                                           cfg->mqtt_keepalive);

    // HTTP 只读连接与监控线程。http_port 属 C 档不可热改,故取启动配置按值传入。
    // 线程 detach 且按值捕获 roDb 副本,即便成员 roDb_ 被换掉该副本仍持有连接;
    // 进程退出由 systemd 接管。
    //
    // idle_timeout / report_n 属 A 档,若一并在此取值就等于把它们降级成 C 档。
    // 故传下去的是取值器而非取好的值,由 HTTP 线程在每次用时现取 —— io 层因此
    // 不必知道 ConfigManager 的存在,配置从哪来是 app 层的事。
    roDb_        = std::make_shared<Database>(cfg->db_path, true);
    http_thread_ = std::thread([roDb = roDb_, port = cfg->http_port] {
        runHttpServer(*roDb, port, [] {
            auto c = ConfigManager::current();
            return HttpRuntimeConfig{c->idle_timeout, c->report_n};
        });
    });
    http_thread_.detach();
    LOG_INFO("http monitor thread started on :%d", cfg->http_port);

    pool_ = std::make_unique<ThreadPool>(cfg->worker_count);   // worker_count 属 C 档
    LOG_INFO("thread pool started (%d workers)", cfg->worker_count);

    pipeline_.emplace(*pool_);   // 依赖 pool_,须在其之后构造

    link_ = std::make_unique<NodeLink>(cfg->serial_path, cfg->serial_baud);
}

void GatewayApp::onFrame(const Frame& f) {
    if (f.type == EDGE_TYPE_QUERY_RESP || f.type == EDGE_TYPE_ACK) {
        onAckFrame(f);
        return;
    }

    LOG_DEBUG("frame type=0x%02X len=%zu", f.type, f.payload.size());
    // 取收帧时刻而非入库时刻,避免线程池排队延迟污染时间戳
    const long ts = static_cast<long>(time(nullptr));
    pipeline_->submit(decodeTelemetry(f), db_, client_, ts);
}

// 与发命令、超时扫描同在主线程,故 tracker_ 无需加锁。
void GatewayApp::onAckFrame(const Frame& f) {
    if (!edge_payload_len_ok(f.type, static_cast<uint8_t>(f.payload.size()))) {
        LOG_WARN("ACK payload too short: %zu, drop", f.payload.size());
        return;
    }
    const uint8_t seq = f.payload[EDGE_OFF_SEQ];
    const uint8_t rc  = f.payload[EDGE_OFF_RC];

    if (!tracker_.onAck(seq)) {
        LOG_WARN("ACK for unknown seq=%u, ignored (late/dup/spurious)", seq);
        return;
    }
    LOG_INFO("ACK matched seq=%u rc=0x%02X, inflight=%zu", seq, rc, tracker_.inflightCount());

    const std::string seq_str = std::to_string(seq);
    if (rc != EDGE_RC_OK) {
        client_->publish("gateway/ack/" + seq_str, "err," + std::to_string(rc));
        return;
    }

    if (f.type != EDGE_TYPE_QUERY_RESP) {
        client_->publish("gateway/ack/" + seq_str, "ok");
        return;
    }

    // 数据段格式随原命令而定:0x21 回温 ×10 + 湿 ×10 各 2 字节,0x20 回光照原值 2 字节。
    // 两者长度不同,故按长度分派即可,无需回查原命令类型。
    const std::size_t data_len = f.payload.size() - 2;
    if (data_len >= 4) {
        const double t = edge_u16_be_read(&f.payload[2]) / double(EDGE_TEMP_SCALE);
        const double h = edge_u16_be_read(&f.payload[4]) / double(EDGE_HUMI_SCALE);
        client_->publish("gateway/resp/" + seq_str,
                         "ok," + std::to_string(t) + "," + std::to_string(h));
    } else if (data_len >= 2) {
        client_->publish("gateway/resp/" + seq_str,
                         "ok," + std::to_string(edge_u16_be_read(&f.payload[2])));
    } else {
        client_->publish("gateway/resp/" + seq_str, "ok,");
    }
}

// 串口写只发生在本函数与 onCmdTimer 的重发路径,两者同在主线程,单一写者由此成立。
void GatewayApp::onDownlinkWakeup() {
    uint64_t cnt = 0;
    if (::read(evfd_, &cnt, sizeof(cnt)) != sizeof(cnt)) {
        // 读失败不影响后续取队列:队列才是数据来源,eventfd 仅用于唤醒
        LOG_DEBUG("%s", "eventfd read returned short");
    }

    while (auto cmd = cmd_queue_.try_pop()) {
        const uint8_t seq = tracker_.nextSeq();

        std::vector<uint8_t> payload;
        payload.reserve(1 + cmd->arg.size());
        payload.push_back(seq);
        payload.insert(payload.end(), cmd->arg.begin(), cmd->arg.end());

        if (link_->send(cmd->type, payload)) {
            LOG_INFO("downlink sent: type=0x%02X seq=%u", cmd->type, seq);
            tracker_.trackWithSeq(seq, cmd->type, cmd->arg, CommandTracker::Clock::now());
        } else {
            // 发送失败则不登记在途表:等一条未发出的命令的 ACK 没有意义
            LOG_WARN("downlink send failed: type=0x%02X seq=%u, not tracked", cmd->type, seq);
        }
    }
}

void GatewayApp::onCmdTimer() {
    uint64_t exp = 0;
    if (::read(cmd_timerfd_, &exp, sizeof(exp)) != sizeof(exp)) {
        return;   // 必须读掉计数,否则 LT 会反复触发
    }

    const auto actions = tracker_.tick(CommandTracker::Clock::now());

    for (const auto& r : actions.resend) {
        std::vector<uint8_t> payload;
        payload.reserve(1 + r.arg.size());
        payload.push_back(r.seq);   // 复用原 seq,§6.2 要求重发幂等
        payload.insert(payload.end(), r.arg.begin(), r.arg.end());

        if (link_->send(r.type, payload)) {
            LOG_WARN("downlink RETRY seq=%u type=0x%02X (attempt %d/%u)",
                     r.seq, r.type, r.attempt, EDGE_MAX_RETRY);
        } else {
            LOG_WARN("downlink retry send failed seq=%u", r.seq);
        }
    }

    for (const auto& f : actions.failed) {
        LOG_ERROR("downlink FAILED seq=%u type=0x%02X: no ACK after %u retries",
                  f.seq, f.type, EDGE_MAX_RETRY);
        // 把终局结果回到 MQTT,便于运维定位
        client_->publish("gateway/ack/" + std::to_string(f.seq), "timeout");
    }
}

// 仅由主线程的 signalfd 回调调用。
void GatewayApp::reloadConfig() {
    LOG_INFO("%s", "SIGHUP received, reloading config...");
    const auto r = ConfigManager::reload();
    if (!r.ok) {
        // load-then-swap 保证失败时旧配置原封不动,无需回滚
        LOG_WARN("%s", "reload failed, keep running with old config");
        return;
    }
    auto ncfg = ConfigManager::current();

    // A 档:改内存即生效
    Logger::setLevel(static_cast<LogLevel>(ncfg->log_level));

    // B 档:按 diff 重建,仅重建确实变更的资源。
    // 换掉 db_ 的 shared_ptr 即可:在飞 task 持有旧 db 快照,旧对象续命至其完成。
    if (r.db_changed) {
        LOG_INFO("%s", "db_path changed → reopening database");
        db_ = std::make_shared<Database>(ncfg->db_path);
    }
    // MqttClient 不可移动,故整体换 shared_ptr。旧 client 析构时停 loop 并断连;
    // 新 client 必须重新挂 handler 并重新订阅,否则下行链路中断。
    if (r.mqtt_changed) {
        LOG_INFO("%s", "mqtt config changed → reconnecting");
        client_ = std::make_shared<MqttClient>("gateway-main", ncfg->mqtt_host, ncfg->mqtt_port,
                                               ncfg->mqtt_keepalive);
        startMqtt();
    }
    // 串口 fd 在 epoll 中,须连同 Channel 一起替换:先 removeChannel 摘除旧 fd
    // (转入 dying_,批末析构),再重开 port,最后建全新 channel 指向新 fd。
    // 不可复用旧 channel 改写其 fd —— 旧 channel 已在 dying_ 中,
    // 析构时会 close 掉被改写后的那个 fd。
    if (r.serial_changed) {
        LOG_INFO("%s", "serial config changed → reopening port");
        loop_.removeChannel(serial_channel_->fd);
        link_->reopen(ncfg->serial_path, ncfg->serial_baud);
        serial_channel_ = makeSerialChannel();
        loop_.addChannel(serial_channel_);
    }
    LOG_INFO("%s", "reload done");
}

// ---- 事件源装配 ----

std::shared_ptr<channel> GatewayApp::makeSerialChannel() {
    auto ch     = std::make_shared<channel>();
    ch->fd      = link_->fd();
    ch->events  = EPOLLIN | EPOLLET;   // ET 要求回调循环读至 EAGAIN
    ch->on_read = [this] { link_->drainAndParse(); };
    return ch;
}

void GatewayApp::setupSerial() {
    link_->setFrameHandler([this](const Frame& f) { onFrame(f); });
    serial_channel_ = makeSerialChannel();
    loop_.addChannel(serial_channel_);
}

// 下行命令投递:cmd_queue_ 由 mosquitto 线程 push、主线程 try_pop;
// eventfd 负责唤醒阻塞在 epoll_wait 的主线程。mosquitto 线程不直接访问串口。
bool GatewayApp::setupDownlink() {
    evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd_ == -1) {
        LOG_ERROR("eventfd failed: %s", strerror(errno));
        return false;   // 下行属核心功能,建立失败即致命
    }
    auto ch     = std::make_shared<channel>();
    ch->fd      = evfd_;
    ch->events  = EPOLLIN;   // 计数器通知用 LT
    ch->on_read = [this] { onDownlinkWakeup(); };
    loop_.addChannel(ch);
    return true;
}

void GatewayApp::startMqtt() {
    auto cfg = ConfigManager::current();

    // 本 handler 在 mosquitto 线程执行,只做翻译与投递,不访问串口。
    // 初次挂载与重连后复用同一份定义,避免两处拷贝漂移。
    client_->setMessageHandler([this](const std::string& topic, const std::string& payload) {
        auto r = translateCommand(topic, payload);
        if (!r.ok) {
            LOG_WARN("downlink rejected: %s", r.error.c_str());
            // 拒绝原因回到 MQTT,避免命令无声消失。
            // 注意此处由 mosquitto 线程调用 publish —— MqttClient 的锁因此不可省。
            client_->publish("gateway/ack/rejected", r.error);
            return;
        }
        cmd_queue_.push(std::move(r.cmd));
        const uint64_t one = 1;
        if (::write(evfd_, &one, sizeof(one)) != sizeof(one)) {
            LOG_WARN("%s", "eventfd notify failed");
        }
    });

    client_->subscribe("gateway/cmd/#", 1);
    client_->loopStart();
    LOG_INFO("mqtt connected: %s:%d", cfg->mqtt_host.c_str(), cfg->mqtt_port);
}

void GatewayApp::setupCmdTimer() {
    cmd_timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (cmd_timerfd_ == -1) {
        LOG_WARN("cmd timerfd failed: %s — 超时重试不可用", strerror(errno));
        return;   // 降级:采集不受影响,仅失去命令自动重发
    }
    struct itimerspec cts {};
    cts.it_value.tv_nsec    = kCmdScanIntervalNs;
    cts.it_interval.tv_nsec = kCmdScanIntervalNs;
    timerfd_settime(cmd_timerfd_, 0, &cts, nullptr);

    auto ch     = std::make_shared<channel>();
    ch->fd      = cmd_timerfd_;
    ch->events  = EPOLLIN;   // LT
    ch->on_read = [this] { onCmdTimer(); };
    loop_.addChannel(ch);
}

// 信号处理函数受 async-signal-safe 限制,无法直接执行重载逻辑。
// 改为把信号转成可读 fd 接入 epoll,在主循环的安全上下文中处理。
void GatewayApp::setupSignal() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    // 屏蔽动作已由 blockReloadSignal 在创建任何线程之前完成;
    // 此处仅把同一个 mask 交给 signalfd,声明它负责哪些信号。
    sfd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd_ == -1) {
        LOG_WARN("signalfd failed: %s — 热加载不可用", strerror(errno));
        return;   // 降级:采集不受影响,仅失去热加载
    }

    auto ch     = std::make_shared<channel>();
    ch->fd      = sfd_;
    ch->events  = EPOLLIN;   // 信号 fd 用 LT
    ch->on_read = [this] {
        struct signalfd_siginfo si;
        // 必须读空,否则 LT 会反复触发
        while (::read(sfd_, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
            if (si.ssi_signo == SIGHUP) reloadConfig();
        }
    };
    loop_.addChannel(ch);
}

// 四类事件源(串口数据、下行命令、超时重试、SIGHUP)统一挂在主线程的同一个 epoll 上。
int GatewayApp::run() {
    setupSerial();
    if (!setupDownlink()) return 1;   // 失败致命
    startMqtt();
    setupCmdTimer();                  // 失败降级
    setupSignal();                    // 失败降级

    loop_.loop();   // 永久阻塞
    return 0;
}

}  // namespace gateway
