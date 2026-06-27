#include "GatewayApp.h"

#include "Logger.h"
#include "FrameBuilder.h"      // [方案B-3] 组下行帧
#include "HttpServer.h"
#include "Config.h"            // [M2] 配置热加载

#include <sys/signalfd.h>
#include <sys/eventfd.h>        // [方案B-3] 跨线程唤醒主循环
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace gateway {

// ============================================================
// 文件内私有:纯业务解码 + 工具(不依赖类状态,保持自由函数)
// ============================================================
namespace {

// 业务解码层:Frame → Record(原样保留)
struct Record {
    std::string device_id;
    double      value;
};

namespace frame_type {
    constexpr uint8_t kDHT11     = 0x01;
    constexpr uint8_t kBH1750    = 0x02;
    constexpr uint8_t kHeartbeat = 0x03;
    constexpr uint8_t kStatus    = 0x04;
}

std::vector<Record> decodeFrame(const gateway::Frame& f) {
    std::vector<Record> out;
    const auto& p = f.payload;
    switch (f.type) {
    case frame_type::kDHT11: {
        if (p.size() < 5) { LOG_WARN("DHT11 payload too short: %zu", p.size()); return out; }
        double temperature = ((static_cast<uint16_t>(p[0]) << 8) | p[1]) / 10.0;
        double humidity    = ((static_cast<uint16_t>(p[2]) << 8) | p[3]) / 10.0;
        out.emplace_back(Record{"temperature", temperature});
        out.emplace_back(Record{"humidity",    humidity});
        break;
    }
    case frame_type::kBH1750: {
        if (p.size() < 2) { LOG_WARN("BH1750 payload too short: %zu", p.size()); return out; }
        double illuminance = (static_cast<uint16_t>(p[0]) << 8) | p[1];
        out.emplace_back(Record{"illuminance", illuminance});
        break;
    }
    case frame_type::kStatus: {
        if (p.size() < 1) return out;
        uint8_t st = p[0];
        // 0x04 状态帧 payload 是 bitmask:bit0=DHT11 OK, bit1=BH1750 OK
        // 拆成两路健康状态,1.0=在线 0.0=故障,与设备名一一对应
        out.emplace_back(Record{"status_dht11",  (st & 0x01) ? 1.0 : 0.0});
        out.emplace_back(Record{"status_bh1750", (st & 0x02) ? 1.0 : 0.0});
        break;
    }
    case frame_type::kHeartbeat:
        break;   // 心跳不落库
    default:
        LOG_WARN("unknown frame type: 0x%02X", f.type);
        break;
    }
    return out;
}

// 配置里存人类可读的 115200,termios 要 Bxxxx 宏。转换隔离在这一层,
// 不让配置层掺串口实现细节。validate 已挡掉 <=0,default 给安全兜底。
speed_t toBaud(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B115200;
    }
}

}  // namespace

// ============================================================
//   下行命令:从 MQTT(mosquitto 线程)投递给主线程统一发送。
//   单一写者原则:SerialPort 的 fd 只由主线程碰,mosquitto 线程不直接写串口,
//   只把命令塞进线程安全队列 + 戳 eventfd 唤醒主循环。
//   (DownCmd / InflightCmd 已上移到 GatewayApp.h 作为类状态的元素类型)
// ============================================================

// 处理命令应答(0x05 查询应答 / 0x06 ACK):配对在途表 + 销账 + 发 MQTT。
// 在主线程的 parser 回调里调用,与发命令、超时扫描同线程 → inflight_ 无需加锁。
void GatewayApp::handleAck(const Frame& f) {
    // 1. 边界:至少 seq + 结果码
    if (f.payload.size() < 2) {
        LOG_WARN("ACK payload too short: %zu, drop", f.payload.size());
        return;
    }
    uint8_t seq = f.payload[0];
    uint8_t rc  = f.payload[1];

    // 2. 配对
    auto it = inflight_.find(seq);
    if (it == inflight_.end()) {
        LOG_WARN("ACK for unknown seq=%u, ignored (late/dup/spurious)", seq);
        return;
    }

    // 3. 配对成功 → 立刻销账(无论结果码,一问一答已结束)
    uint8_t orig_type = it->second.type;        // ← 新增: 先记原命令类型
    inflight_.erase(it);
    LOG_INFO("ACK matched seq=%u rc=0x%02X, inflight=%zu", seq, rc, inflight_.size());

    // 4. 结果码分流发 MQTT
    if (rc == 0x00) {
        if (f.type == 0x05) {
            if (orig_type == 0x21 && f.payload.size() >= 6) {          // query_th: 温×10 + 湿×10
                double t = ((f.payload[2] << 8) | f.payload[3]) / 10.0;
                double h = ((f.payload[4] << 8) | f.payload[5]) / 10.0;
                client_->publish("gateway/resp/" + std::to_string(seq),
                            "ok," + std::to_string(t) + "," + std::to_string(h));
            } else if (f.payload.size() >= 4) {                        // query_light: 光照原值
                uint16_t val = (f.payload[2] << 8) | f.payload[3];
                client_->publish("gateway/resp/" + std::to_string(seq), "ok," + std::to_string(val));
            } else {
                client_->publish("gateway/resp/" + std::to_string(seq), "ok,");
            }
        } else {
            client_->publish("gateway/ack/" + std::to_string(seq), "ok");
        }
    } else {
        client_->publish("gateway/ack/" + std::to_string(seq), "err," + std::to_string(rc));
    }
}

// ============================================================
// FrameParser data-path:回调里按 type 分流 + 双写。
//   关键:task 捕获 db/client 的 shared_ptr 【快照】(db_, client_ 按值进 lambda),
//   而非引用 —— 这样热加载 reset 外层 db_/client_ 时,在飞 task 手里的旧对象不被销毁。
// ============================================================
FrameParser::OnFrameCallback GatewayApp::makeFrameHandler() {
    return [this](const gateway::Frame& f){
        // [方案B-4] 先按 type 分流:0x05/0x06 是命令应答,走配对;其余是传感器数据,走落库
        if (f.type == 0x05 || f.type == 0x06) {
            handleAck(f);                      // 配对 + 销账 + 发 MQTT
            return;
        }
        // ---- 以下是原来的传感器数据双写逻辑,原样保留 ----
        LOG_DEBUG("frame type=0x%02X len=%zu", f.type, f.payload.size());
        long ts = static_cast<long>(time(nullptr));
        for (const auto& r : decodeFrame(f)) {
            std::string dev = r.device_id;
            double      val = r.value;
            auto db_snap     = db_;
            auto client_snap = client_;
            pool_->submit([db_snap, client_snap, dev, val, ts]{
                db_snap->insert(dev, val, ts);
                client_snap->publish("gateway/up/" + dev, std::to_string(val));
            });
        }
    };
}

// ============================================================
// MQTT 下行 handler(在 mosquitto 线程跑,只翻译+投递,不碰串口)。
// [方案B-3] topic 形如 gateway/cmd/<命令名>,payload 放裸参数。
//   handler 把命令翻译成 DownCmd 塞进队列 + 戳 eventfd,绝不直接写串口。
// 初次挂载与 SIGHUP 重连后复用同一份,杜绝两处拷贝漂移。
// ============================================================
MqttClient::MessageHandler GatewayApp::makeDownlinkHandler() {
    return [this](const std::string& topic, const std::string& payload){
        // 取 topic 最后一段作为命令名(同 pipeline.cpp 的 find_last_of)
        std::string cmd_name = topic;
        size_t pos = topic.find_last_of('/');
        if (pos != std::string::npos) cmd_name = topic.substr(pos + 1);

        DownCmd cmd;
        bool valid = true;
        if (cmd_name == "query_light") {
            cmd.type = 0x20;                          // 查询光照,无参数
        } else if (cmd_name == "query_th") {
            cmd.type = 0x21;                          // 查询温湿度,无参数
        } else if (cmd_name == "set_period") {
            cmd.type = 0x22;                          // 设采样周期(单位:秒)
            // payload 是周期秒数字符串如 "2000"(秒),转成 2 字节大端 uint16(协议 §3.4)
            try {
                int period = std::stoi(payload);
                if (period < 0 || period > 0xFFFF) {
                    LOG_WARN("set_period out of range: %s", payload.c_str());
                    valid = false;
                } else {
                    cmd.arg.push_back(static_cast<uint8_t>((period >> 8) & 0xFF)); // 高字节
                    cmd.arg.push_back(static_cast<uint8_t>(period & 0xFF));        // 低字节
                }
            } catch (...) {
                LOG_WARN("set_period bad payload: %s", payload.c_str());
                valid = false;
            }
        } else {
            valid = false;
            LOG_WARN("unknown downlink cmd: %s", cmd_name.c_str());
        }

        if (valid) {
            cmd_queue_.push(std::move(cmd));        // 塞队列(线程安全)
            uint64_t one = 1;
            if (write(evfd_, &one, sizeof(one)) != sizeof(one)) {  // 戳 eventfd 唤醒主循环
                LOG_WARN("%s", "eventfd notify failed");
            }
        }
    };
}

// ============================================================
// 串口 Channel:ET 模式,on_read 循环读到 EAGAIN,逐字节喂 FSM。
// 访问成员 port_:热加载 reset port_ 后,这里自动看到新 port。parser_ 是成员,用 .
// ============================================================
std::shared_ptr<channel> GatewayApp::makeSerialChannel() {
    auto serial_channel = std::make_shared<gateway::channel>();
    serial_channel->fd     = port_->get();
    serial_channel->events = EPOLLIN | EPOLLET;   // ET:必须循环读到 EAGAIN
    serial_channel->on_read = [this]() {
        while (1) {
            uint8_t buf[256];
            ssize_t n = read(port_->get(), buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)  continue;                 // 被信号打断,重试
                if (errno == EAGAIN) break;                    // 读空,ET 下正常退出
                LOG_ERROR("serial read error: %s", strerror(errno));
                break;
            } else if (n == 0) {
                LOG_WARN("%s", "serial EOF (peer closed?)");      // 对端关闭(socat 那头),非 EAGAIN
                break;
            } else {
                for (ssize_t i = 0; i < n; ++i)
                    parser_.feed(buf[i]);                      // 批量读、逐字节喂 FSM
            }
        }
    };
    return serial_channel;
}

// ============================================================
// eventfd Channel(下行命令投递):
// [方案B-3] mosquitto 线程把命令塞进 cmd_queue_ 后,往 evfd_ 写 1 唤醒主循环;
//   主循环在这个回调里取空队列、组帧、写串口(单一写者,无需给 SerialPort 加锁)。
// ============================================================
std::shared_ptr<channel> GatewayApp::makeEventfdChannel() {
    auto evt_channel = std::make_shared<gateway::channel>();
    evt_channel->fd     = evfd_;
    evt_channel->events = EPOLLIN;   // 计数器通知用 LT 即可(同 signalfd)
    evt_channel->on_read = [this]() {
        uint64_t cnt = 0;
        read(evfd_, &cnt, sizeof(cnt));

        while (auto cmd = cmd_queue_.try_pop()) {
            uint8_t seq = seq_counter_++;                 // 分配 seq
            std::vector<uint8_t> payload;
            payload.push_back(seq);
            for (uint8_t b : cmd->arg) payload.push_back(b);
            auto frame = gateway::buildFrame(cmd->type, payload);
            ssize_t w = port_->write(frame.data(), frame.size());
            if (w != static_cast<ssize_t>(frame.size())) {
                LOG_WARN("downlink send incomplete: type=0x%02X ret=%zd/%zu",
                        cmd->type, w, frame.size());
                // 发都没发成功,不登记在途表(没必要等它的 ACK)
            } else {
                LOG_INFO("downlink sent: type=0x%02X seq=%u (%zu bytes)",
                        cmd->type, seq, frame.size());
                // 登记进在途表,等 ACK / 超时重发
                inflight_[seq] = InflightCmd{cmd->type, cmd->arg,
                                            static_cast<long>(time(nullptr)), 0};
            }
        }
    };
    return evt_channel;
}

// ============================================================
// timerfd Channel(命令超时重试扫描):
//   周期扫描在途命令表 inflight_:超时未满重试次数则重发(同 seq,幂等),
//   超满次数判失败删除。扫描周期 200ms < 超时时限 500ms,保证及时发现。
//   与发命令(eventfd 回调)、收 ACK(parser 回调)同在主线程 → inflight_ 无锁。
// ============================================================
std::shared_ptr<channel> GatewayApp::makeCmdTimerChannel() {
    auto cmd_timer_channel = std::make_shared<gateway::channel>();
    cmd_timer_channel->fd     = cmd_timerfd_;
    cmd_timer_channel->events = EPOLLIN;   // LT
    cmd_timer_channel->on_read = [this]() {
        uint64_t exp = 0;
        read(cmd_timerfd_, &exp, sizeof(exp));   // 必须读掉,否则 LT 反复触发

        const long   TIMEOUT_SEC = 1;   // 超时时限(协议 §6.5 是 500ms;秒级 time() 精度有限,
                                        //   取 1s 保守稳妥。要更精细可换 steady_clock 毫秒)
        const int    MAX_RETRY   = 3;
        long now = static_cast<long>(time(nullptr));

        // 两阶段:先只读收集,再统一处理(避免遍历 inflight_ 时 erase/改导致迭代器失效)
        std::vector<uint8_t> to_resend, to_fail;
        for (auto& kv : inflight_) {
            if (now - kv.second.sent_time > TIMEOUT_SEC) {
                if (kv.second.retry_count < MAX_RETRY) to_resend.push_back(kv.first);
                else                                   to_fail.push_back(kv.first);
            }
        }
        // 重发(同 seq,§6.2 幂等)
        for (uint8_t seq : to_resend) {
            auto& c = inflight_[seq];
            std::vector<uint8_t> payload;
            payload.push_back(seq);                       // 同 seq!
            for (uint8_t b : c.arg) payload.push_back(b);
            auto frame = gateway::buildFrame(c.type, payload);
            ssize_t w = port_->write(frame.data(), frame.size());
            if (w == static_cast<ssize_t>(frame.size())) {
                c.retry_count++;
                c.sent_time = now;                        // 重置计时
                LOG_WARN("downlink RETRY seq=%u type=0x%02X (attempt %d/%d)",
                        seq, c.type, c.retry_count, MAX_RETRY);
            } else {
                LOG_WARN("downlink retry send incomplete seq=%u ret=%zd", seq, w);
            }
        }
        // 重试耗尽,判失败删除
        for (uint8_t seq : to_fail) {
            LOG_ERROR("downlink FAILED seq=%u type=0x%02X: no ACK after %d retries",
                    seq, inflight_[seq].type, MAX_RETRY);
            inflight_.erase(seq);
            // 可选:发 MQTT 通知运维这条命令彻底失败
            // client_->publish("gateway/ack/" + std::to_string(seq), "timeout");
        }
    };
    return cmd_timer_channel;
}

// ============================================================
// SIGHUP 热加载策略:load-then-swap 后按 diff 仅重建真变了的资源。
//   只在主线程的 signalfd 回调里调用(与发命令、收 ACK、超时扫描同线程)。
//   db_ / client_ / port_ / serial_channel_ 均为成员:reset 后,
//   各 channel 回调(捕获 this)与在飞 task(shared_ptr 快照)各自看到正确对象。
// ============================================================
void GatewayApp::reloadConfig() {
    LOG_INFO("%s", "SIGHUP received, reloading config...");
    auto r = gateway::ConfigManager::reload();
    if (!r.ok) {
        // load-then-swap 保证旧配置原封不动,无需任何回滚动作
        LOG_WARN("%s","reload failed, keep running with old config");
        return;
    }
    auto ncfg = gateway::ConfigManager::current();

    // ---- A 档:改内存即生效 ----
    gateway::Logger::setLevel(
        static_cast<gateway::LogLevel>(ncfg->log_level));
    // idle_timeout / report_n 等:用到处读 current() 自动生效

    // ---- B 档:按 diff 重建,且仅重建真变了的 ----
    // db:reset 旧 shared_ptr。在飞 task 持有旧 db 快照 → 旧对象续命到干完。
    if (r.db_changed) {
        LOG_INFO("%s","db_path changed → reopening database");
        db_ = std::make_shared<gateway::Database>(ncfg->db_path);
    }
    // mqtt:unique/shared_ptr 替换绕过 MqttClient 不可移动。
    //       旧 client 析构(stop loop/断连/destroy),新 client 建好。
    if (r.mqtt_changed) {
        LOG_INFO("%s","mqtt config changed → reconnecting");
        client_ = std::make_shared<gateway::MqttClient>(
            "gateway-main", ncfg->mqtt_host, ncfg->mqtt_port,
            ncfg->mqtt_keepalive);
        // 重连后要重新挂下行命令 handler + 重新订阅(否则下行命令断了)
        client_->setMessageHandler(makeDownlinkHandler());
        client_->subscribe("gateway/cmd/#", 1);
        client_->loopStart();
    }
    // serial:fd 在 epoll 里,要连 Channel 一起换。
    //   先 removeChannel 摘旧 fd(进 dying_,批末析构 close 旧 fd),
    //   再重建 port,再建【全新】channel 指向新 fd ——
    //   绝不复用旧 channel 改 fd:那会让旧 channel 的析构 close 错 fd
    //   (shared_ptr 别名坑:旧 channel 已在 dying_ 里)。
    if (r.serial_changed) {
        LOG_INFO("%s","serial config changed → reopening port");
        loop_.removeChannel(serial_channel_->fd);
        port_ = std::make_shared<gateway::SerialPort>(
            ncfg->serial_path.c_str(), toBaud(ncfg->serial_baud),
            /*nonblock=*/true);
        // 复用同一工厂重建(逻辑不变:访问成员 port_,自动用新 port)
        auto new_ch = makeSerialChannel();
        loop_.addChannel(new_ch);
        serial_channel_ = new_ch;   // 成员指向新 channel(下次 SIGHUP 用新 fd)
    }
    LOG_INFO("%s","reload done");
}

// ============================================================
// 构造:RAII 资源。db_ / client_ 用 shared_ptr 持有(不是 unique_ptr):
//   线程池 task 会捕获它们的 shared_ptr 快照,使旧对象活到 task 干完。
//   热加载重建时 reset 成员,旧对象靠在飞 task 的快照续命 → 无 UAF。
//
//   成员声明顺序决定析构逆序:pool_ 在 db_/client_ 之后声明 → 最先析构
//   (drain 在飞 task),此时 db_/client_ 仍被 task 快照持有 → 析构安全,铁律不变。
// ============================================================
GatewayApp::GatewayApp() {
    auto cfg = gateway::ConfigManager::current();   // 启动配置快照

    db_ = std::make_shared<gateway::Database>(cfg->db_path);
    LOG_INFO("sqlite(rw) opened at %s", cfg->db_path.c_str());

    client_ = std::make_shared<gateway::MqttClient>(
        "gateway-main", cfg->mqtt_host, cfg->mqtt_port, cfg->mqtt_keepalive);

    // HTTP 只读连接 + 监控线程。
    //   http_port / db_path(只读侧)用【启动配置】—— http_port 是 C 档(不可热改),
    //   只读连接生命周期靠进程同生共死(detach),优雅退出留待 systemd 接管(M15)。
    //   线程按值捕获 roDb 副本 → 即便成员 roDb_ 析构,HTTP 线程那份仍续命。
    roDb_ = std::make_shared<gateway::Database>(cfg->db_path, true);
    http_thread_ = std::thread([roDb = roDb_]{ gateway::runHttpServer(*roDb); });
    http_thread_.detach();
    LOG_INFO("http monitor thread started on :%d", cfg->http_port);

    // 线程池(worker_count 是 C 档,用启动值)
    pool_ = std::make_unique<gateway::ThreadPool>(cfg->worker_count);
    LOG_INFO("thread pool started (%d workers)", cfg->worker_count);

    // 串口:nonblock=true(epoll 前提)。用 shared_ptr 持有,热加载可重建。
    port_ = std::make_shared<gateway::SerialPort>(
        cfg->serial_path.c_str(), toBaud(cfg->serial_baud), /*nonblock=*/true);
    LOG_INFO("serial opened: %s @ %d", cfg->serial_path.c_str(), cfg->serial_baud);
}

// ---- 串口 Channel --------
void GatewayApp::setupSerial() {
    parser_.setOnFrame(makeFrameHandler());
    serial_channel_ = makeSerialChannel();
    loop_.addChannel(serial_channel_);
}

// ---- eventfd Channel(下行命令投递)--------
// [方案B-3] 下行命令投递机制:
//   - cmd_queue_:mosquitto 线程 push,主线程 try_pop(线程安全队列)
//   - eventfd:mosquitto 线程塞完命令后戳一下,唤醒阻塞在 epoll_wait 的主线程
//   单一写者:串口 write 只发生在主线程的 eventfd Channel 回调里。
bool GatewayApp::setupDownlink() {
    evfd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd_ == -1) {
        // eventfd 建不起来,下行命令链路不可用。这里致命退出(下行是核心功能)。
        LOG_ERROR("eventfd failed: %s", strerror(errno));
        return false;
    }
    loop_.addChannel(makeEventfdChannel());
    return true;
}

// ---- MQTT 下行 handler(在 mosquitto 线程跑,只翻译+投递,不碰串口)--------
void GatewayApp::startMqtt() {
    auto cfg = gateway::ConfigManager::current();
    client_->setMessageHandler(makeDownlinkHandler());
    client_->subscribe("gateway/cmd/#", 1);
    client_->loopStart();
    LOG_INFO("mqtt connected: %s:%d", cfg->mqtt_host.c_str(), cfg->mqtt_port);
}

// ---- timerfd Channel(命令超时重试扫描)--------
void GatewayApp::setupCmdTimer() {
    cmd_timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (cmd_timerfd_ == -1) {
        LOG_WARN("cmd timerfd failed: %s — 超时重试不可用", strerror(errno));
        return;
    }
    struct itimerspec cts;
    cts.it_value.tv_sec = 0;     cts.it_value.tv_nsec = 200 * 1000 * 1000;    // 首次 200ms
    cts.it_interval.tv_sec = 0;  cts.it_interval.tv_nsec = 200 * 1000 * 1000; // 之后每 200ms
    timerfd_settime(cmd_timerfd_, 0, &cts, nullptr);
    loop_.addChannel(makeCmdTimerChannel());
}

// ============================================================
// signalfd Channel(SIGHUP 热加载)
// 信号处理函数不能干重活(async-signal-safe 限制),故用 signalfd 把信号
// 变成可读 fd,接进 epoll,在主循环安全上下文里 reload。
// (这是 Channel 抽象第三次复用:socket → timerfd → signalfd → eventfd)
// ============================================================
void GatewayApp::setupSignal() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    // 命门:先 block SIGHUP 默认递送。不 block,SIGHUP 默认行为是【终止进程】,
    //       signalfd 根本读不到。block 后信号转由 signalfd 这条路读出。
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        LOG_WARN("sigprocmask failed: %s — 热加载不可用", strerror(errno));
    }
    sfd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd_ == -1) {
        LOG_WARN("signalfd failed: %s — 热加载不可用", strerror(errno));
    }

    // signalfd 建起来才挂 Channel(降级:建不起来网关照常采集,只是不能热加载)
    if (sfd_ != -1) {
        auto sig_channel = std::make_shared<gateway::channel>();
        sig_channel->fd     = sfd_;
        sig_channel->events = EPOLLIN;   // 信号 fd 用 LT 即可
        sig_channel->on_read = [this]() {
            struct signalfd_siginfo si;
            // 必须读掉 siginfo,否则 LT 反复触发。while 读到 EAGAIN(SFD_NONBLOCK)。
            while (read(sfd_, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                if (si.ssi_signo != SIGHUP) continue;
                reloadConfig();
            }
        };
        loop_.addChannel(sig_channel);
    }
}

// ============================================================
// [P5] 装配四类事件源 + 跑主循环:串口数据 + SIGHUP 信号 + 下行命令,统一事件驱动。
// ============================================================
int GatewayApp::run() {
    setupSerial();                       // 串口 Channel
    if (!setupDownlink()) return 1;      // eventfd + 下行 Channel(失败致命)
    startMqtt();                         // 挂下行 handler + 订阅 + loopStart
    setupCmdTimer();                     // 超时重试 timerfd(失败降级)
    setupSignal();                       // SIGHUP signalfd(失败降级)

    loop_.loop();   // 主循环:永久阻塞,串口数据 + SIGHUP + 下行命令统一事件驱动
    return 0;
}

}  // namespace gateway
