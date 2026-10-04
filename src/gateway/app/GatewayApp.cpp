/**
 * @file GatewayApp.cpp
 * @brief 实现网关资源装配、四类事件源注册和跨线程事件汇聚。
 *
 * 实现按生命周期、事件注册、事件入口、协议分派和资源重载排列。主 Reactor 是
 * 串口和 CommandTracker 的唯一访问线程。MQTT、HTTP、遥测通过有界邮箱交给
 * GatewayCore；业务完成后通过命令队列和 eventfd 回到串口 Reactor。
 *
 * 启动阶段无法创建必需资源时向进程入口传播异常；SIGHUP 重载按数据库、MQTT、
 * 串口分别处理资源重建失败，让其他路径继续运行。退出时关闭入口并等待 HTTP，
 * 排空业务与查询，再排空控制任务并等待 MQTT，最后排空遥测写队列。
 */

#include "gateway/app/GatewayApp.h"

#include "gateway/core/config/Config.h"
#include "gateway/core/log/Logger.h"
#include "gateway/io/http/HttpServer.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace gateway {

namespace {
/** 命令超时扫描间隔，单位为纳秒；小于单次 ACK 等待时间以控制检测延迟。 */
constexpr long kCmdScanIntervalNs = 200L * 1000 * 1000;

/** 每次下行唤醒最多接收的命令数，避免持续生产让单个回调长期占用主线程。 */
constexpr std::size_t kDownlinkBatchSize = 32;

/** 自定义 ACK、拒绝回执和查询结果的 MQTT QoS；1 表示至少一次传递语义。 */
constexpr int kMqttCommandResultQos = 1;

/**
 * @brief 填充由网关主循环管理的 Unix 信号集合。
 * @param mask 输出参数；函数会先清空，再加入热加载和两种退出信号。
 *
 * blockManagedSignals() 与 registerSignalEvent() 共用该函数，确保屏蔽集合和
 * signalfd 监听集合始终一致。
 */
void fillManagedSignals(sigset_t& mask) {
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
}
}  // namespace

// 生命周期：先准备业务资源，再注册事件源，最后启动对外服务。

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
    // config 是构造期间使用的一致配置快照，避免同一批资源读取到不同版本。
    const auto config = ConfigManager::current();

    // write_db 是写连接的临时所有者；创建 schema 后立即移交给 TelemetryPipeline。
    auto write_db = std::make_shared<Database>(config->db_path);
    LOG_INFO("sqlite(rw) opened at %s", config->db_path.c_str());
    // pipeline_ 接管写连接，并启动独占该连接的后台线程。
    pipeline_.emplace(std::move(write_db));

    // link_ 以非阻塞模式打开 STM32 串口，后续只在主线程访问。
    link_ = std::make_unique<NodeLink>(config->serial_path, config->serial_baud);
    // frame 是 NodeLink 解析完成后短暂传入的帧引用，分派在同一 Reactor 回调中完成。
    link_->setFrameHandler([this](const Frame& frame) { dispatchFrame(frame); });
    // read_db_ 是 HTTP 初始只读连接；启动后每个请求通过原子 shared_ptr 快照取得所有权。
    read_db_ = std::make_shared<Database>(config->db_path, true);
}

GatewayApp::~GatewayApp() {
    // 先停止入口：网络回调仍可发生，但不再访问业务邮箱。HTTP 回调全部返回后 join。
    stopping_.store(true);
    cmd_queue_.shutdown();
    replacements_.shutdown();
    http_stop_.store(true);
    if (http_thread_.joinable()) http_thread_.join();
    // Reactor 已退出，不再发送新命令；把已接受的排队/在途请求交付明确终态。
    if (core_) {
        while (auto command = cmd_queue_.try_pop()) core_->commandRejected(command->id, "gateway_shutdown");
        for (const auto& [seq, id] : requests_)
            core_->commandFinished(id, CommandCompletion{seq, 0, {}, "gateway_shutdown"});
        requests_.clear();
        core_->stop(); // 排空业务，再等查询完成，再排空其回调。
    }
    control_.stop(); // 已生成的上行消息交给库；所有资源准备/替换任务结束。
    mqtt_client_.reset(); // join MQTT 网络循环，保证回调不再引用本对象。
    // pipeline_ 随后排空遥测写队列，read_db_ 仍有效。
}

int GatewayApp::run() {
    // 串口注册只建立监听；eventfd 必须先于 MQTT 网络线程存在，供首条命令通知使用。
    registerSerialEvent();
    if (!registerDownlinkEvent()) return 1;
    // SR 的有限重试依赖定时器，创建失败不能继续接受下行命令。
    registerCommandTimerEvent();
    registerSignalEvent();

    core_ = std::make_unique<GatewayCore>(*pipeline_,
        [this](uint64_t id, DownCmd command) { return submitCommand(id, std::move(command)); },
        [this](std::string topic, std::string payload, int qos, std::function<void()> done) {
            return publish(std::move(topic), std::move(payload), qos, std::move(done));
        },
        [this] { return std::atomic_load_explicit(&read_db_, std::memory_order_acquire); },
        [] { const auto cfg = ConfigManager::current();
             return HttpRuntimeConfig{cfg->idle_timeout, cfg->report_n}; });
    // config 为本次后台服务启动提供一致参数；此时事件源已注册但尚未开始派发。
    const auto config = ConfigManager::current();
    // mqtt_client_ 接管已配置并启动的客户端；网络线程可以入队，由稍后的主循环消费。
    auto initialized = std::make_shared<std::promise<void>>();
    auto ready = initialized->get_future();
    if (!control_.post([this, config, initialized] {
        try {
            mqtt_client_ = createMqttClient(*config);
            initialized->set_value();
        } catch (...) { initialized->set_exception(std::current_exception()); }
    })) throw std::runtime_error("MQTT initialization queue closed");
    ready.get(); // 仅启动阶段等待；进入 Reactor 后不等待后台工作。
    // HTTP 线程在完整应用对象上启动，后续异常能够由 GatewayApp 析构完成 join。
    startHttpMonitor(config->http_port);

    // 主线程进入阻塞派发，直到信号入口调用 quit() 或循环抛出异常。
    loop_.loop();
    LOG_INFO("%s", "main loop exited, shutting down");
    return http_failed_.load() ? 1 : 0;
}

// 事件注册：每个 fd 只绑定一个同层级入口，不在注册代码中展开业务逻辑。

void GatewayApp::registerSerialEvent() {
    // ch 保存当前串口 fd、epoll 关注事件和入口回调；注册后由 EventLoop 持有。
    auto ch = std::make_shared<channel>();
    ch->fd = link_->fd();
    ch->owns_fd = false;  // NodeLink 内的 SerialPort 保留关闭权。
    ch->events = EPOLLIN | EPOLLET;
    if (link_->hasPendingOutput()) ch->events |= EPOLLOUT;
    ch->on_read = [this] { onSerialEvent(); };
    ch->on_write = [this] { onSerialWriteEvent(); };
    loop_.addChannel(ch);
    serial_channel_ = std::move(ch);
}

bool GatewayApp::registerDownlinkEvent() {
    // ch 从 fd 创建起就负责关闭资源，后续回调安装或注册抛异常时也能自动清理。
    auto ch = std::make_shared<channel>();
    // eventfd 使用非阻塞计数器语义；初值 0 表示启动时没有待处理通知。
    ch->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ch->fd == -1) {
        LOG_ERROR("eventfd failed: %s", strerror(errno));
        return false;
    }
    ch->events = EPOLLIN;
    ch->on_read = [this] { onDownlinkEvent(); };
    loop_.addChannel(ch);
    // downlink_fd_ 只保存已注册描述符的借用值，供网络线程通知和主线程读取。
    downlink_fd_ = ch->fd;
    return true;
}

void GatewayApp::registerCommandTimerEvent() {
    // ch 拥有本次创建的 timerfd；注册前任何失败返回都会由其析构关闭描述符。
    auto ch = std::make_shared<channel>();
    // CLOCK_MONOTONIC 避免系统时间校准改变命令超时判断。
    ch->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ch->fd == -1) {
        throw std::runtime_error(std::string("SR timerfd_create failed: ") + strerror(errno));
    }

    // interval 同时定义首次触发和后续周期；秒字段保持 0，纳秒字段取扫描间隔。
    struct itimerspec interval {};
    interval.it_value.tv_nsec = kCmdScanIntervalNs;
    interval.it_interval.tv_nsec = kCmdScanIntervalNs;
    if (timerfd_settime(ch->fd, 0, &interval, nullptr) == -1) {
        throw std::runtime_error(std::string("SR timerfd_settime failed: ") + strerror(errno));
    }

    ch->events = EPOLLIN;
    ch->on_read = [this] { onCommandTimerEvent(); };
    loop_.addChannel(ch);
    command_timer_fd_ = ch->fd;
}

void GatewayApp::registerSignalEvent() {
    // mask 与进程启动时屏蔽的集合一致，作为 signalfd 的输入过滤器。
    sigset_t mask;
    fillManagedSignals(mask);

    // ch 拥有 signalfd，并把异步信号转换为主 Reactor 中的普通可读事件。
    auto ch = std::make_shared<channel>();
    ch->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (ch->fd == -1) {
        LOG_WARN("signalfd failed: %s — 热加载与优雅停机不可用", strerror(errno));
        return;
    }
    ch->events = EPOLLIN;
    ch->on_read = [this] { onSignalEvent(); };
    loop_.addChannel(ch);
    signal_fd_ = ch->fd;
}

// 四类事件入口：全部在同一 Reactor 线程执行，串口发送与 ACK/重试共享时间线。

void GatewayApp::onSerialEvent() {
    // ET 模式下排空到 EAGAIN；NodeLink 同步调用 dispatchFrame()。
    link_->drainAndParse();
}

void GatewayApp::onSerialWriteEvent() {
    // 每帧写完后 NodeLink 立即执行完成回调；短写由队首偏移保留到下次可写事件。
    link_->flushOutput();
    updateSerialWriteInterest();
}

void GatewayApp::onDownlinkEvent() {
    // count 接收自上次读取以来累计的 eventfd 通知次数；次数不等同于命令条数。
    uint64_t count = 0;
    ssize_t n = 0; // EINTR 不代表通知已被消费，必须重试同一次读取。
    do {
        n = ::read(downlink_fd_, &count, sizeof(count));
    } while (n < 0 && errno == EINTR);
    if (n != sizeof(count) && !(n < 0 && errno == EAGAIN)) {
        LOG_DEBUG("%s", "eventfd read returned short");
    }

    if (http_failed_.load()) { loop_.quit(); return; }
    while (auto fresh = replacements_.try_pop()) {
        loop_.removeChannel(link_->fd());
        serial_channel_.reset();
        try { (*fresh)->activate(); }
        catch (const std::exception& error) {
            LOG_ERROR("serial configure failed, keeping old link: %s", error.what());
            registerSerialEvent();
            continue;
        }
        // 先结束旧会话，但直到新链路就绪才投递 OPEN；旧队列析构不执行回调。
        auto actions = tracker_.start(tracker_.session(), CommandTracker::Clock::now());
        link_ = std::move(*fresh);
        link_->setFrameHandler([this](const Frame& frame) { dispatchFrame(frame); });
        registerSerialEvent();
        applyTrackerActions(actions);
        LOG_INFO("%s", "serial resource applied on reactor");
    }
    if (!cmd_queue_.empty() && tracker_.state() == CommandTracker::State::Closed) {
        const auto epoch = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        applyTrackerActions(tracker_.start(epoch > 0 ? static_cast<uint64_t>(epoch) : 1,
                                           CommandTracker::Clock::now()));
    }
    std::size_t processed = 0;
    for (; processed < kDownlinkBatchSize && !cmd_queue_.empty(); ++processed) {
        if (tracker_.state() == CommandTracker::State::Failed) {
            auto command = cmd_queue_.try_pop();
            if (!command) break;
            core_->commandRejected(command->id, "link_failed");
            continue;
        }
        if (!tracker_.canSubmit()) break;
        auto command = cmd_queue_.try_pop();
        if (!command) break;
        const auto send = tracker_.submit(command->command.type, std::move(command->command.arg),
                                          CommandTracker::Clock::now());
        if (send) {
            requests_.emplace(send->seq, command->id);
            queueTransmission(*send);
        } else core_->commandRejected(command->id, "link_failed");
    }
    updateSerialWriteInterest();
    if (processed == kDownlinkBatchSize && !cmd_queue_.empty()) notifyDownlink();
}

void GatewayApp::onCommandTimerEvent() {
    uint64_t expirations = 0;
    if (::read(command_timer_fd_, &expirations, sizeof(expirations)) != sizeof(expirations)) return;
    applyTrackerActions(tracker_.tick(CommandTracker::Clock::now()));
    if (!cmd_queue_.empty()) notifyDownlink();
}

void GatewayApp::onSignalEvent() {
    // info 保存单个已排队信号的编号和内核元数据；当前逻辑只读取 ssi_signo。
    struct signalfd_siginfo info;
    // 在同一回调中排空当前所有待处理信号，统一通过主线程执行控制动作。
    while (::read(signal_fd_, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
        switch (info.ssi_signo) {
        case SIGHUP:
            requestReload();
            break;
        case SIGTERM:
        case SIGINT:
            LOG_INFO("signal %u received, shutting down", info.ssi_signo);
            loop_.quit();
            break;
        default:
            LOG_WARN("unexpected signal %u on signalfd", info.ssi_signo);
            break;
        }
    }
}

// Reactor 内的协议分派：推进 SR 状态，将应用输入投递给 GatewayCore。

void GatewayApp::dispatchFrame(const Frame& frame) {
    if (frame.type == EDGE_TYPE_SR_RECEIVED || frame.type == EDGE_TYPE_SR_RESULT ||
        frame.type == EDGE_TYPE_SR_OPEN_ACK) {
        applyTrackerActions(tracker_.onFrame(frame.type, frame.payload, CommandTracker::Clock::now()));
        if (!cmd_queue_.empty()) notifyDownlink();
    } else if (!core_->telemetry(frame, static_cast<long>(time(nullptr)))) {
        LOG_WARN("%s", "business queue full, dropped telemetry frame");
    }
}

void GatewayApp::applyTrackerActions(const TrackerActions& actions) {
    // SR 协议动作立即推进；完成数据拥有独立副本，业务格式转换在 GatewayCore。
    for (const auto& done : actions.finished) {
        const auto request = requests_.find(done.seq);
        if (request == requests_.end()) continue;
        if (!core_->commandFinished(request->second, done))
            LOG_ERROR("%s", "command completion rejected during shutdown");
        requests_.erase(request);
    }
    for (const auto& send : actions.send) queueTransmission(send);
    updateSerialWriteInterest();
}

void GatewayApp::queueTransmission(const CommandSend& send) {
    auto complete = [this, send](bool sent) {
        if (sent && send.type == EDGE_TYPE_SR_COMMAND)
            LOG_INFO("SR sent session=%llu seq=%u attempt=%u",
                     static_cast<unsigned long long>(send.session), send.seq, send.attempt);
        applyTrackerActions(tracker_.onWritten(send, sent, CommandTracker::Clock::now()));
        if (!cmd_queue_.empty()) notifyDownlink();
    };
    auto valid = [this, send] { return tracker_.canSend(send, CommandTracker::Clock::now()); };
    if (!link_->send(send.type, send.payload, complete, std::move(valid))) complete(false);
}

void GatewayApp::updateSerialWriteInterest() {
    if (!serial_channel_) return; // 串口重开期间的旧输出完成回调仍可推进状态机。
    uint32_t events = EPOLLIN | EPOLLET; // 读侧保留 ET；只有待发帧存在时才关注可写。
    if (link_->hasPendingOutput()) events |= EPOLLOUT;
    if (serial_channel_->events == events) return;
    serial_channel_->events = events;
    loop_.modifyChannel(serial_channel_.get());
}

void GatewayApp::notifyDownlink() {
    const uint64_t one = 1; // 单次唤醒增量；计数合并不改变命令队列的 FIFO 顺序。
    ssize_t n = 0;
    do {
        n = ::write(downlink_fd_, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    if (n != sizeof(one) && !(n < 0 && errno == EAGAIN)) {
        LOG_WARN("%s", "eventfd notify failed");
    }
}

bool GatewayApp::submitCommand(uint64_t id, DownCmd command) {
    if (stopping_.load() || !cmd_queue_.try_push(Downlink{id, std::move(command)})) return false;
    notifyDownlink();
    return true;
}

bool GatewayApp::publish(std::string topic, std::string payload, int qos, std::function<void()> done) {
    // 客户端发布/替换由同一线程串行化；命令终态独享准入额度对应的预留位置。
    const bool completion = static_cast<bool>(done);
    const bool accepted = control_.post(
        [this, topic = std::move(topic), payload = std::move(payload), qos, done = std::move(done)] {
            if (mqtt_client_) mqtt_client_->publish(topic, payload, qos);
            if (done) done();
        }, completion);
    if (!accepted) LOG_WARN("%s", "MQTT publish queue full, message not accepted");
    return accepted;
}

void GatewayApp::requestReload() {
    if (reload_pending_.exchange(true)) return;
    if (!control_.post([this] {
        if (!stopping_.load()) reloadConfig();
        reload_pending_.store(false);
    }, true)) reload_pending_.store(false);
}

void GatewayApp::reloadConfig() {
    LOG_INFO("%s", "SIGHUP received, reloading config...");
    // changes 同时表示解析是否成功，以及相对上一份配置需要重建的资源集合。
    const auto changes = ConfigManager::reload();
    if (!changes.ok) {
        LOG_WARN("%s", "reload failed, keep running with old config");
        return;
    }

    // config 是 ConfigManager 已发布的新快照，供本次各资源重建读取一致参数。
    // 发布发生在资源应用之前，因此某分支失败时配置值和实际资源可能暂时不同。
    const auto config = ConfigManager::current();
    // 日志级别只涉及原子内存状态，无需重建外部资源即可立即生效。
    Logger::setLevel(static_cast<LogLevel>(config->log_level));

    // all_applied 汇总同步重建和切库任务入队的结果，只用于最终日志，不参与回滚。
    bool all_applied = true;
    // 各分支独立执行，前一资源失败不会跳过后续资源的应用。
    if (changes.db_swap_required && !reloadDatabase(*config)) all_applied = false;
    if (changes.mqtt_rebuild_required && !reloadMqtt(*config)) all_applied = false;
    if (changes.serial_rebuild_required && !reloadSerial(*config)) all_applied = false;

    LOG_INFO("reload done%s", all_applied ? "" : " (with degraded items, see ERROR above)");
}

// 资源装配与重载：候选资源准备成功后再交付，回调始终绑定其实际所有者。

std::unique_ptr<MqttClient> GatewayApp::createMqttClient(const Config& config) {
    // client 是候选连接的临时所有者；配置和网络线程启动全部成功后才移交给调用方。
    auto client = std::make_unique<MqttClient>(
        "gateway-main", config.mqtt_host, config.mqtt_port, config.mqtt_keepalive);
    // 网络回调只进行边界检查和入队；命令翻译在 GatewayCore。
    MqttClient* owner = client.get();
    client->setMessageHandler([this, owner](const std::string& topic, const std::string& payload) {
        if (stopping_.load()) return;
        if (!core_->mqttMessage(topic, payload)) {
            // 未接受的请求由入口明确拒绝；这是过载处理，不进入设备业务状态机。
            owner->publish("gateway/ack/rejected", "gateway_busy", kMqttCommandResultQos);
        }
    });

    // QoS 1 用于云端命令。这里只登记逻辑订阅；客户端在首次连接及以后每次
    // CONNACK 时发送 SUBSCRIBE，使 clean session 重连和热加载共用同一订阅路径。
    client->subscribe("gateway/cmd/#", 1);
    if (!client->loopStart()) {
        // 候选对象由局部 unique_ptr 清理；启动阶段向入口报错，重载阶段由调用方降级。
        throw std::runtime_error("mqtt loop start failed");
    }
    LOG_INFO("mqtt network loop started: %s:%d", config.mqtt_host.c_str(), config.mqtt_port);
    return client;
}

void GatewayApp::startHttpMonitor(int port) {
    http_thread_ = std::thread([this, port] {
        pthread_setname_np(pthread_self(), "gateway-http");
        const bool ok = runHttpServer(port,
            [] { const auto config = ConfigManager::current();
                 return HttpRuntimeConfig{config->idle_timeout, config->report_n}; },
            [this](HttpRequest request, HttpReply reply) {
                return !stopping_.load() && core_->httpRequest(std::move(request), std::move(reply));
            },
            [this] { return http_stop_.load(); });
        if (!ok) { http_failed_.store(true); notifyDownlink(); }
    });
    LOG_INFO("http reactor started on :%d", port);
}

bool GatewayApp::reloadDatabase(const Config& config) {
    LOG_INFO("%s", "db_path changed → reopening database");
    try {
        // write_db/read_db 是候选读写连接；两者全部创建成功后才向写线程提交切换。
        auto write_db = std::make_shared<Database>(config.db_path);
        auto read_db = std::make_shared<Database>(config.db_path, true);
        // new_path 按值捕获到异步回调，用于记录实际完成切换的目标路径。
        const std::string new_path = config.db_path;

        // 切库与遥测共用有序队列。写线程刷完旧库并切换后，才发布新的 HTTP 读连接；
        // 正在执行的 HTTP 请求继续持有旧连接，后续请求原子取得新连接。
        if (!pipeline_->swapDatabase(
                std::move(write_db), [this, read_db = std::move(read_db), new_path] {
                    std::atomic_store_explicit(&read_db_, read_db, std::memory_order_release);
                    LOG_INFO("http reader switched to new database: %s", new_path.c_str());
                })) {
            LOG_ERROR("%s", "db swap not delivered (write queue full), read and write remain "
                            "on the old database");
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        // error 描述候选连接创建或切换任务提交失败，尚未提交时读写两侧继续使用旧库。
        LOG_ERROR("db reopen failed, keep reading and writing the old database: %s", error.what());
        return false;
    }
}

bool GatewayApp::reloadMqtt(const Config& config) {
    LOG_INFO("%s", "mqtt config changed → reconnecting");
    try {
        // 创建路径与启动一致；连接、回调、订阅和网络线程全部就绪后才替换旧客户端。
        mqtt_client_ = createMqttClient(config);
        return true;
    } catch (const std::exception& error) {
        // error 来自候选客户端创建或启动；赋值尚未发生，当前客户端对象仍保留。
        LOG_ERROR("mqtt rebuild failed, keeping the previous client: %s", error.what());
        return false;
    }
}

bool GatewayApp::reloadSerial(const Config& config) {
    try {
        // open 在控制线程准备；共享 tty 的 termios 配置、fd 注册和会话切换归 Reactor。
        auto fresh = std::make_shared<NodeLink>(config.serial_path, config.serial_baud, true);
        if (!replacements_.try_push(std::move(fresh))) {
            LOG_ERROR("%s", "serial replacement queue full");
            return false;
        }
        notifyDownlink();
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR("serial reopen failed, staying on previous port: %s", error.what());
        return false;
    }
}

} // namespace gateway
