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
// 超时扫描周期。取 200ms 是为了「及时发现」:它必须明显小于 500ms 的应答时限,
// 否则一条命令可能超时了大半个周期才被察觉。
constexpr long kCmdScanIntervalNs = 200L * 1000 * 1000;
}  // namespace

// 见头文件:必须在任何线程被创建之前调用,否则 SIGHUP 会落到某个
// 没屏蔽它的线程上、按默认行为把进程杀掉。
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

// ============================================================
// 构造:RAII 资源。db_ / client_ 用 shared_ptr 而非 unique_ptr,
// 因为线程池 task 要捕获它们的快照(见 TelemetryPipeline 头文件)。
// ============================================================
GatewayApp::GatewayApp() {
    auto cfg = ConfigManager::current();   // 启动配置快照

    db_ = std::make_shared<Database>(cfg->db_path);
    LOG_INFO("sqlite(rw) opened at %s", cfg->db_path.c_str());

    client_ = std::make_shared<MqttClient>("gateway-main", cfg->mqtt_host, cfg->mqtt_port,
                                           cfg->mqtt_keepalive);

    // HTTP 只读连接 + 监控线程。
    //   http_port / db_path(只读侧)用【启动配置】—— http_port 是 C 档不可热改;
    //   只读连接的生命周期与进程同生共死(detach),优雅退出交由 systemd 接管。
    //   线程按值捕获 roDb 副本 → 即便成员 roDb_ 析构,HTTP 线程那份仍续命。
    roDb_        = std::make_shared<Database>(cfg->db_path, true);
    http_thread_ = std::thread([roDb = roDb_] { runHttpServer(*roDb); });
    http_thread_.detach();
    LOG_INFO("http monitor thread started on :%d", cfg->http_port);

    pool_ = std::make_unique<ThreadPool>(cfg->worker_count);   // worker_count 是 C 档
    LOG_INFO("thread pool started (%d workers)", cfg->worker_count);

    pipeline_.emplace(*pool_);   // 必须在 pool_ 建好之后

    link_ = std::make_unique<NodeLink>(cfg->serial_path, cfg->serial_baud);
}

// ============================================================
// 串口来帧:按 TYPE 分流。应答走命令链路,其余走遥测双写。
// ============================================================
void GatewayApp::onFrame(const Frame& f) {
    if (f.type == EDGE_TYPE_QUERY_RESP || f.type == EDGE_TYPE_ACK) {
        onAckFrame(f);
        return;
    }

    LOG_DEBUG("frame type=0x%02X len=%zu", f.type, f.payload.size());
    const long ts = static_cast<long>(time(nullptr));   // 在收帧时刻打,不在 worker 里打
    pipeline_->submit(decodeTelemetry(f), db_, client_, ts);
}

// ============================================================
// 应答帧(0x05 查询应答 / 0x06 ACK):配对在途表 → 销账 → 结果回 MQTT。
// 与发命令、超时扫描同在主线程 → tracker_ 无需加锁。
// ============================================================
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

    // 查询应答的数据段格式随原命令而定:
    //   查温湿度(0x21)→ 温 ×10 + 湿 ×10,各 2 字节
    //   查光照  (0x20)→ 光照原值 2 字节
    // 数据段长度足以区分两者,无需回头查原命令类型。
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

// ============================================================
// eventfd 被戳:取空队列、分配 seq、组帧、发送、登记在途表。
// 串口写【只在这里和超时重发里发生】,两者同在主线程 —— 单一写者。
// ============================================================
void GatewayApp::onDownlinkWakeup() {
    uint64_t cnt = 0;
    if (::read(evfd_, &cnt, sizeof(cnt)) != sizeof(cnt)) {
        // 计数器读空即可,读失败不影响下面取队列(队列才是真正的数据来源)
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
            // 发都没发出去,不登记在途表 —— 没必要等一个根本没发生的命令的 ACK
            LOG_WARN("downlink send failed: type=0x%02X seq=%u, not tracked", cmd->type, seq);
        }
    }
}

// ============================================================
// timerfd:推进在途表。重发用同 seq(§6.2 幂等),重试耗尽判失败。
// ============================================================
void GatewayApp::onCmdTimer() {
    uint64_t exp = 0;
    if (::read(cmd_timerfd_, &exp, sizeof(exp)) != sizeof(exp)) {
        return;   // 必须读掉,否则 LT 反复触发
    }

    const auto actions = tracker_.tick(CommandTracker::Clock::now());

    for (const auto& r : actions.resend) {
        std::vector<uint8_t> payload;
        payload.reserve(1 + r.arg.size());
        payload.push_back(r.seq);   // 同 seq!
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
        // 运维在 MQTT 那头也该看到结局,而不是石沉大海
        client_->publish("gateway/ack/" + std::to_string(f.seq), "timeout");
    }
}

// ============================================================
// SIGHUP 热加载:load-then-swap 后按 diff 仅重建真变了的资源。
// 只在主线程的 signalfd 回调里调用。
// ============================================================
void GatewayApp::reloadConfig() {
    LOG_INFO("%s", "SIGHUP received, reloading config...");
    const auto r = ConfigManager::reload();
    if (!r.ok) {
        // load-then-swap 保证旧配置原封不动,无需任何回滚动作
        LOG_WARN("%s", "reload failed, keep running with old config");
        return;
    }
    auto ncfg = ConfigManager::current();

    // ---- A 档:改内存即生效 ----
    Logger::setLevel(static_cast<LogLevel>(ncfg->log_level));

    // ---- B 档:按 diff 重建,且仅重建真变了的 ----
    // db:reset 旧 shared_ptr。在飞 task 持有旧 db 快照 → 旧对象续命到干完。
    if (r.db_changed) {
        LOG_INFO("%s", "db_path changed → reopening database");
        db_ = std::make_shared<Database>(ncfg->db_path);
    }
    // mqtt:换 shared_ptr 绕过 MqttClient 不可移动。旧 client 析构(停 loop / 断连),
    //       新 client 建好后必须重新挂 handler + 重新订阅,否则下行命令就断了。
    if (r.mqtt_changed) {
        LOG_INFO("%s", "mqtt config changed → reconnecting");
        client_ = std::make_shared<MqttClient>("gateway-main", ncfg->mqtt_host, ncfg->mqtt_port,
                                               ncfg->mqtt_keepalive);
        startMqtt();
    }
    // serial:fd 在 epoll 里,要连 Channel 一起换。
    //   先 removeChannel 摘旧 fd(进 dying_,批末析构),再重开 port,
    //   再建【全新】channel 指向新 fd —— 绝不复用旧 channel 改 fd:
    //   旧 channel 已在 dying_ 里,它析构时会 close 掉被改写的那个 fd。
    if (r.serial_changed) {
        LOG_INFO("%s", "serial config changed → reopening port");
        loop_.removeChannel(serial_channel_->fd);
        link_->reopen(ncfg->serial_path, ncfg->serial_baud);
        serial_channel_ = makeSerialChannel();
        loop_.addChannel(serial_channel_);
    }
    LOG_INFO("%s", "reload done");
}

// ============================================================
// 事件源装配
// ============================================================

std::shared_ptr<channel> GatewayApp::makeSerialChannel() {
    auto ch     = std::make_shared<channel>();
    ch->fd      = link_->fd();
    ch->events  = EPOLLIN | EPOLLET;   // ET:必须循环读到 EAGAIN
    ch->on_read = [this] { link_->drainAndParse(); };
    return ch;
}

void GatewayApp::setupSerial() {
    link_->setFrameHandler([this](const Frame& f) { onFrame(f); });
    serial_channel_ = makeSerialChannel();
    loop_.addChannel(serial_channel_);
}

// 下行命令投递机制:
//   cmd_queue_  mosquitto 线程 push,主线程 try_pop(线程安全队列)
//   eventfd     mosquitto 线程塞完命令后戳一下,唤醒阻塞在 epoll_wait 的主线程
// mosquitto 线程【绝不碰串口】,单一写者由此成立。
bool GatewayApp::setupDownlink() {
    evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd_ == -1) {
        LOG_ERROR("eventfd failed: %s", strerror(errno));
        return false;   // 下行是核心功能,建不起来就致命退出
    }
    auto ch     = std::make_shared<channel>();
    ch->fd      = evfd_;
    ch->events  = EPOLLIN;   // 计数器通知用 LT 即可
    ch->on_read = [this] { onDownlinkWakeup(); };
    loop_.addChannel(ch);
    return true;
}

void GatewayApp::startMqtt() {
    auto cfg = ConfigManager::current();

    // 下行 handler 跑在 mosquitto 线程:只翻译 + 投递,不碰串口。
    // 初次挂载与重连后复用同一份,杜绝两处拷贝漂移。
    client_->setMessageHandler([this](const std::string& topic, const std::string& payload) {
        auto r = translateCommand(topic, payload);
        if (!r.ok) {
            LOG_WARN("downlink rejected: %s", r.error.c_str());
            // 让运维在 MQTT 那头看得到原因,而不是石沉大海
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
        return;   // 降级:采集照常,只是命令不会自动重发
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

// signalfd:信号处理函数不能干重活(async-signal-safe 限制),故把信号变成
// 可读 fd 接进 epoll,在主循环的安全上下文里 reload。
// (channel 抽象的第四次复用:socket → timerfd → signalfd → eventfd)
void GatewayApp::setupSignal() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    // 屏蔽动作已由 main 在建任何线程之前做过(见 blockReloadSignal 的注释)。
    // 这里只把同一个 mask 交给 signalfd —— signalfd 需要知道自己负责哪些信号。
    sfd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd_ == -1) {
        LOG_WARN("signalfd failed: %s — 热加载不可用", strerror(errno));
        return;   // 降级:网关照常采集,只是不能热加载
    }

    auto ch     = std::make_shared<channel>();
    ch->fd      = sfd_;
    ch->events  = EPOLLIN;   // 信号 fd 用 LT 即可
    ch->on_read = [this] {
        struct signalfd_siginfo si;
        // 必须读掉 siginfo,否则 LT 反复触发。读到 EAGAIN 为止。
        while (::read(sfd_, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
            if (si.ssi_signo == SIGHUP) reloadConfig();
        }
    };
    loop_.addChannel(ch);
}

// ============================================================
// 装配四类事件源 + 跑主循环:串口数据、下行命令、超时重试、SIGHUP,
// 统一挂在主线程同一个 epoll 上。
// ============================================================
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
