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
// 超时扫描周期。必须明显小于 EDGE_ACK_TIMEOUT_MS(500ms),
// 否则一条命令可能在超时大半个周期后才被察觉。
constexpr long kCmdScanIntervalNs = 200L * 1000 * 1000;
}  // namespace

// 本进程自己接管的三个信号。屏蔽与 signalfd 两处必须用同一份清单,
// 少一个就意味着那个信号仍走默认动作 —— 而三者的默认动作都是终止进程。
static void fillManagedSignals(sigset_t& mask) {
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);    // 热加载
    sigaddset(&mask, SIGTERM);   // systemctl stop / systemd 停服务
    sigaddset(&mask, SIGINT);    // 终端里 Ctrl-C
}

bool GatewayApp::blockManagedSignals() {
    sigset_t mask;
    fillManagedSignals(mask);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        LOG_WARN("sigprocmask failed: %s — 热加载与优雅停机不可用", strerror(errno));
        return false;
    }
    return true;
}

// client_ 用 shared_ptr 而非 unique_ptr:热加载换 broker 时整体替换指针,
// 而 mosquitto 线程上的 handler 需要一个确定活着的对象,见 startMqtt。
GatewayApp::GatewayApp() {
    auto cfg = ConfigManager::current();   // 启动配置快照

    // 写连接建完即交给 pipeline_,app 层不再持有 —— 从这行往下,能调用它的
    // 只有 pipeline_ 内部的写线程。建库必须最先做:roDb_ 以只读方式打开,
    // 要求库文件与表已存在。
    auto db = std::make_shared<Database>(cfg->db_path);
    LOG_INFO("sqlite(rw) opened at %s", cfg->db_path.c_str());

    client_ = std::make_shared<MqttClient>("gateway-main", cfg->mqtt_host, cfg->mqtt_port,
                                           cfg->mqtt_keepalive);

    // 这一步会起遥测写线程。与下面的 http_thread_ 不同,它可以放在会抛异常的
    // link_ 之前:pipeline_ 是 RAII 封装的线程,构造中途抛异常时已构造完的成员
    // 仍会析构,~TelemetryPipeline 会 join 掉它。裸的 std::thread 成员没有这个
    // 性质 —— 析构时撞上 joinable 就直接 terminate,这正是 http_thread_ 必须
    // 放在最后的原因。
    pipeline_.emplace(std::move(db));

    link_ = std::make_unique<NodeLink>(cfg->serial_path, cfg->serial_baud);

    // HTTP 只读连接与监控线程,放在构造函数最后一步。
    //
    // 顺序是有讲究的:这条线程要被 join,而 join 只能在 ~GatewayApp 里做,
    // 但构造中途抛异常时 ~GatewayApp【不会】被调用 —— 只有已构造完的成员会析构,
    // 于是 ~thread 撞上一条 joinable 的线程,直接 std::terminate。
    // 串口打不开(serial_path 写错、设备没插)恰恰是本构造函数最常见的失败,
    // 所以把起线程放在所有可能抛的动作之后:线程存在时,构造已必然成功。
    //
    // roDb_ 仍在写连接之后创建 —— 只读连接要求库文件已存在。
    // 线程按值捕获 roDb 副本,即便成员 roDb_ 被换掉,该副本仍持有连接。
    //
    // idle_timeout / report_n 属 A 档,若一并在此取值就等于把它们降级成 C 档。
    // 故传下去的是取值器而非取好的值,由 HTTP 线程在每次用时现取 —— io 层因此
    // 不必知道 ConfigManager 的存在,配置从哪来是 app 层的事。
    // should_stop 同理:io 层只知道"有人会告诉我该走了",不知道那是 SIGTERM。
    roDb_        = std::make_shared<Database>(cfg->db_path, true);
    http_thread_ = std::thread([this, roDb = roDb_, port = cfg->http_port] {
        runHttpServer(
            *roDb, port,
            [] {
                auto c = ConfigManager::current();
                return HttpRuntimeConfig{c->idle_timeout, c->report_n};
            },
            [this] { return http_stop_.load(std::memory_order_relaxed); });
    });
    LOG_INFO("http monitor thread started on :%d", cfg->http_port);
}

// 先停 HTTP 线程,再让成员按声明逆序析构(见类头)。
// 捕获 this 的那个 should_stop 闭包活在 HTTP 线程里,所以 join 必须早于
// 本对象的任何成员被销毁 —— 析构函数体正是这个时机。
GatewayApp::~GatewayApp() {
    http_stop_.store(true, std::memory_order_relaxed);
    if (http_thread_.joinable()) {
        http_thread_.join();   // 最多等一个扫描周期(约 1 秒)
    }
}

void GatewayApp::onFrame(const Frame& f) {
    if (f.type == EDGE_TYPE_QUERY_RESP || f.type == EDGE_TYPE_ACK) {
        onAckFrame(f);
        return;
    }

    LOG_DEBUG("frame type=0x%02X len=%zu", f.type, f.payload.size());
    // 取收帧时刻而非入库时刻,避免写队列的排队延迟污染时间戳
    const long ts       = static_cast<long>(time(nullptr));
    const auto readings = decodeTelemetry(f);

    // 上云在本线程直接发。mosquitto_publish 只是把报文塞进库的发送队列就返回,
    // 真正的 socket 写在 mosquitto 自己的网络线程上,所以这里是 µs 级且不阻塞;
    // publish 无锁、允许跨线程并发调用(见 MqttClient.h)。
    // 与 onAckFrame / onCmdTimer 的上报走同一条路径,不另开线程 —— 在库自带的
    // 发送队列前面再套一个队列,只会多一跳延迟。
    for (const auto& r : readings) {
        client_->publish("gateway/up/" + r.device, formatValue(r.value));
    }

    // 落库交给写线程。返回 false 只可能是磁盘写不动导致队列积满,
    // 此时丢弃新数据而不是阻塞 Reactor,见 TelemetryPipeline::submit。
    if (!pipeline_->submit(readings, ts)) {
        LOG_WARN("telemetry queue full, dropped %zu readings", readings.size());
    }
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
                         "ok," + formatValue(t) + "," + formatValue(h));
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
    // 换库不是在这里改指针,而是往写队列里投一条换库指令:写线程会在自己的
    // 时间线上按顺序执行它 —— 先把此前排队的记录落进旧库,再切到新库。
    // 于是「换库」与「落库」天然互斥,既不需要锁,也不会有记录写错库。
    // 下面三个分支都在重建外部资源,每一个都可能失败(路径写错、设备没插、broker
    // 地址打错)。此前它们都没有 try-catch,异常会一路穿出 loop() 与 run(),被 main
    // 接住后 return 1,systemd 依 Restart=on-failure 重启进程 —— 而重启后读到的是
    // 同一份坏配置,启动路径上会以同样的方式再抛一次,于是变成崩溃循环,直到有人
    // 手工改回配置文件。热加载的全部意义就是不重启,却因为一次手滑的改动而让进程
    // 永久起不来,这个代价方向完全反了。
    //
    // 所以每个分支各自接住异常:失败只丢掉这一项功能,已经跑着的其余部分照常。
    // 这与 app 层既有的错误分级一致 —— 致命的在启动期抛,运行期的一律降级。
    bool all_applied = true;

    if (r.db_changed) {
        LOG_INFO("%s", "db_path changed → reopening database");
        try {
            pipeline_->swapDatabase(std::make_shared<Database>(ncfg->db_path));
        } catch (const std::exception& e) {
            all_applied = false;
            LOG_ERROR("db reopen failed, keep writing to the old database: %s", e.what());
        }
    }
    // MqttClient 不可移动,故整体换 shared_ptr。旧 client 析构时断连并 join 掉
    // 网络线程(见 ~MqttClient),这一步会短暂阻塞主循环 —— 换 broker 是人工低频
    // 操作,用一次可预期的停顿换「回调确定不再触发」这个前提,值。
    // 新 client 必须重新挂 handler 并重新订阅,否则下行链路中断。
    if (r.mqtt_changed) {
        LOG_INFO("%s", "mqtt config changed → reconnecting");
        // 这里没有强异常保证可依赖:旧 client_ 一旦被赋值覆盖就已析构。所以先建新的,
        // 建成功了再换 —— 与 NodeLink::reopen 现在的形状一致。
        try {
            auto fresh = std::make_shared<MqttClient>("gateway-main", ncfg->mqtt_host,
                                                      ncfg->mqtt_port, ncfg->mqtt_keepalive);
            client_ = std::move(fresh);
            startMqtt();
        } catch (const std::exception& e) {
            all_applied = false;
            LOG_ERROR("mqtt rebuild failed, uplink may be degraded: %s", e.what());
        }
    }
    // 串口 fd 在 epoll 中,须连同 Channel 一起替换:先 removeChannel 摘除旧 fd
    // (转入 dying_,批末析构),再重开 port,最后建全新 channel 指向新 fd。
    // 不可复用旧 channel 改写其 fd —— 旧 channel 已在 dying_ 中,
    // 析构时会 close 掉被改写后的那个 fd。
    if (r.serial_changed) {
        LOG_INFO("%s", "serial config changed → reopening port");
        loop_.removeChannel(serial_channel_->fd);
        try {
            link_->reopen(ncfg->serial_path, ncfg->serial_baud);
        } catch (const std::exception& e) {
            all_applied = false;
            // reopen 是强异常保证:抛出时 link_ 仍持有旧 port,旧 fd 依然有效。
            // 所以下面那两行无论成败都能跑 —— 失败时它把【旧 fd】重新挂回 epoll,
            // 采集继续;成功时挂的是新 fd。epoll 两条路都保持一致,不会残留空洞。
            LOG_ERROR("serial reopen failed, staying on the previous port: %s", e.what());
            // 此刻 ConfigManager::current()->serial_path 已是新值,而实际在用的是旧口。
            // 这个不一致必须说出来,否则读日志的人会以为新路径已经生效。
            LOG_WARN("effective serial path is still the old one, not '%s'",
                     ncfg->serial_path.c_str());
        }
        serial_channel_ = makeSerialChannel();   // 取 link_ 当前【实际】的 fd
        loop_.addChannel(serial_channel_);
    }

    LOG_INFO("reload done%s", all_applied ? "" : " (with degraded items, see ERROR above)");
}

// ---- 事件源装配 ----

std::shared_ptr<channel> GatewayApp::makeSerialChannel() {
    auto ch     = std::make_shared<channel>();
    ch->fd      = link_->fd();
    ch->events  = EPOLLIN | EPOLLET;   // ET 要求回调循环读至 EAGAIN
    ch->on_read = [this] { link_->drainAndParse(); };
    // 串口 fd 归 SerialPort 所有,channel 只借来挂 epoll。不放弃所有权就是双重关闭,
    // 且热加载换串口时关掉的会是新 fd —— 理由见 channel::owns_fd 的声明。
    ch->owns_fd = false;
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

    // handler 捕获它所属的那个 client 的裸指针,而不是回头读成员 client_。
    //
    // 读成员是错的:handler 跑在 mosquitto 线程上,而 reloadConfig 会在主线程给
    // client_ 赋新值。shared_ptr 的引用计数是原子的,指针本身的读写不是,这就是
    // 一处货真价实的数据竞争 —— 且换 client 时要先跑完阻塞的 mosquitto_connect,
    // 窗口并不窄。
    //
    // 捕获裸指针则既躲开竞争,语义也更正:拒收原因本就该从「收到这条命令的那条
    // 连接」发回去。指针的有效性由 ~MqttClient 保证 —— 它先 disconnect 再
    // loop_stop(false) 等网络线程 join,handler 不可能跑在对象析构之后。
    MqttClient* owner = client_.get();

    // 本 handler 在 mosquitto 线程执行,只做翻译与投递,不访问串口。
    // 初次挂载与重连后复用同一份定义,避免两处拷贝漂移。
    client_->setMessageHandler([this, owner](const std::string& topic,
                                             const std::string& payload) {
        auto r = translateCommand(topic, payload);
        if (!r.ok) {
            LOG_WARN("downlink rejected: %s", r.error.c_str());
            // 拒绝原因回到 MQTT,避免命令无声消失。
            // publish 可跨线程并发调用,不需要外部串行化,见 MqttClient::publish。
            owner->publish("gateway/ack/rejected", r.error);
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
    fillManagedSignals(mask);
    // 屏蔽动作已由 blockManagedSignals 在创建任何线程之前完成;
    // 此处仅把同一份 mask 交给 signalfd,声明它负责哪些信号。
    sfd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd_ == -1) {
        LOG_WARN("signalfd failed: %s — 热加载与优雅停机不可用", strerror(errno));
        return;   // 降级:采集不受影响
    }

    auto ch     = std::make_shared<channel>();
    ch->fd      = sfd_;
    ch->events  = EPOLLIN;   // 信号 fd 用 LT
    ch->on_read = [this] {
        struct signalfd_siginfo si;
        // 必须读空,否则 LT 会反复触发
        while (::read(sfd_, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
            switch (si.ssi_signo) {
            case SIGHUP:
                reloadConfig();
                break;
            case SIGTERM:
            case SIGINT:
                // quit() 只置标志,本轮回调跑完、批末回收之后 loop() 才返回。
                // 之所以能在这里安全地调,是因为本回调就跑在 loop 自己的线程上。
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

// 四类事件源(串口数据、下行命令、超时重试、信号)统一挂在主线程的同一个 epoll 上。
int GatewayApp::run() {
    setupSerial();
    if (!setupDownlink()) return 1;   // 失败致命
    startMqtt();
    setupCmdTimer();                  // 失败降级
    setupSignal();                    // 失败降级

    loop_.loop();   // 阻塞至 SIGTERM / SIGINT

    // 走到这里说明是被信号叫停的。返回之后依次发生:
    //   ~GatewayApp   停 HTTP 线程并 join
    //   成员逆序析构   pipeline_ 关队列、写线程把剩余记录落完再 join(见
    //                 ~TelemetryPipeline)→ link_ 关串口 → client_ 停 mosquitto
    //                 网络线程 → roDb_ 关连接
    //   main 返回      静态对象析构 → ~AsyncLogger 把双缓冲里剩下的日志刷出去
    // 最后这一步正是这次要的:停机原因不会再随缓冲一起消失。
    LOG_INFO("%s", "main loop exited, shutting down");
    return 0;
}

}  // namespace gateway
