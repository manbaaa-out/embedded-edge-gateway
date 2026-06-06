#include "Logger.h"
#include "SerialPort.h"
#include "FrameParser.h"
#include "ThreadPool.h"
#include "Database.h"
#include "MqttClient.h"
#include "HttpServer.h"
#include "EventLoop.h"          // [P5] 主线程也 Reactor 化
#include "Config.h"             // [M2] 配置热加载

#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <termios.h>

// ============================================================
// 业务解码层:Frame → Record(原样保留)
// ============================================================
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
        out.emplace_back(Record{"device_status", static_cast<double>(p[0])});
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
static speed_t toBaud(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B115200;
    }
}

int main(int argc, char* argv[]) {
    const char* conf_path = (argc > 1) ? argv[1] : "/etc/gateway.conf";

    // ============================================================
    // [M2] 配置初始化。失败致命:连配置都读不出,无法决定怎么启动。
    //      (对照下面 signalfd 失败=降级:配置是核心前提,热加载是辅助便利)
    // ============================================================
    try {
        gateway::ConfigManager::init(conf_path);
    } catch (const std::exception& e) {
        LOG_ERROR("config init failed: %s", e.what());
        return 1;
    }
    auto cfg = gateway::ConfigManager::current();   // 启动配置快照
    gateway::Logger::setLevel(static_cast<gateway::LogLevel>(cfg->log_level));
    LOG_INFO("gateway starting, config from %s", conf_path);

    try {
        // ========================================================
        // RAII 资源。db / client 用 shared_ptr 持有(不是 unique_ptr):
        //   线程池 task 会捕获它们的 shared_ptr 快照,使旧对象活到 task 干完。
        //   热加载重建时 reset 外层指针,旧对象靠在飞 task 的快照续命 → 无 UAF。
        //   (这是 Config 的 atomic shared_ptr 同款思想:被多方用+可能被替换 → shared_ptr)
        //
        //   声明顺序仍决定析构逆序:pool 最后建 → 最先析构(drain 在飞 task),
        //   此时 db/client 仍被 task 快照持有 → 析构安全,铁律不变。
        // ========================================================
        auto db = std::make_shared<gateway::Database>(cfg->db_path);
        LOG_INFO("sqlite(rw) opened at %s", cfg->db_path.c_str());

        auto client = std::make_shared<gateway::MqttClient>(
            "gateway-main", cfg->mqtt_host, cfg->mqtt_port, cfg->mqtt_keepalive);
        client->setMessageHandler([](const std::string& topic, const std::string& payload){
            LOG_INFO("downlink cmd: %s = %s (not handled yet)", topic.c_str(), payload.c_str());
        });
        client->subscribe("gateway/cmd/#", 1);
        client->loopStart();
        LOG_INFO("mqtt connected: %s:%d", cfg->mqtt_host.c_str(), cfg->mqtt_port);

        // HTTP 只读连接 + 监控线程。
        //   http_port / db_path(只读侧)用【启动配置】—— http_port 是 C 档(不可热改),
        //   只读连接生命周期靠进程同生共死(detach),优雅退出留待 systemd 接管(M15)。
        auto roDb = std::make_shared<gateway::Database>(cfg->db_path, true);
        std::thread http_thread([roDb]{ gateway::runHttpServer(*roDb); });
        http_thread.detach();
        LOG_INFO("http monitor thread started on :%d", cfg->http_port);

        // 线程池(worker_count 是 C 档,用启动值)
        auto pool = std::make_unique<gateway::ThreadPool>(cfg->worker_count);
        LOG_INFO("thread pool started (%d workers)", cfg->worker_count);

        // 串口:nonblock=true(epoll 前提)。用 shared_ptr 持有,热加载可重建。
        auto port = std::make_shared<gateway::SerialPort>(
            cfg->serial_path.c_str(), toBaud(cfg->serial_baud), /*nonblock=*/true);
        LOG_INFO("serial opened: %s @ %d", cfg->serial_path.c_str(), cfg->serial_baud);

        // FrameParser:回调里解码 + 双写。
        //   关键:task 捕获 db/client 的 shared_ptr 【快照】(db, client 按值进 lambda),
        //   而非引用 —— 这样热加载 reset 外层 db/client 时,在飞 task 手里的旧对象不被销毁。
        gateway::FrameParser parser;
        parser.setOnFrame([&pool, &db, &client](const gateway::Frame& f){
            LOG_DEBUG("frame type=0x%02X len=%zu", f.type, f.payload.size());
            long ts = static_cast<long>(time(nullptr));
            for (const auto& r : decodeFrame(f)) {
                std::string dev = r.device_id;
                double      val = r.value;
                auto db_snap     = db;       // 当前 db 的 shared_ptr 快照
                auto client_snap = client;   // 当前 client 的快照
                pool->submit([db_snap, client_snap, dev, val, ts]{
                    db_snap->insert(dev, val, ts);                         // 落库
                    client_snap->publish("gateway/up/" + dev, std::to_string(val)); // 上行
                });
            }
        });

        // ========================================================
        // [P5] 主线程 Reactor 化:串口数据 + SIGHUP 信号,统一事件驱动。
        //      原来"阻塞 read 串口"的裸循环,升级为 epoll 事件循环 ——
        //      否则卡在串口 read 上,signalfd 可读也无人处理。
        // ========================================================
        gateway::EventLoop loop;

        // -------- 串口 Channel --------
        auto serial_channel = std::make_shared<gateway::channel>();
        serial_channel->fd     = port->get();
        serial_channel->events = EPOLLIN | EPOLLET;   // ET:必须循环读到 EAGAIN
        // 引用捕获 port:热加载 reset port 后,这里自动看到新 port。parser 是栈对象,用 .
        serial_channel->on_read = [&port, &parser]() {
            while (1) {
                uint8_t buf[256];
                ssize_t n = read(port->get(), buf, sizeof(buf));
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
                        parser.feed(buf[i]);                       // 批量读、逐字节喂 FSM
                }
            }
        };
        loop.addChannel(serial_channel);

        // -------- signalfd Channel(SIGHUP 热加载)--------
        // 信号处理函数不能干重活(async-signal-safe 限制),故用 signalfd 把信号
        // 变成可读 fd,接进 epoll,在主循环安全上下文里 reload。
        // (这是 Channel 抽象第三次复用:socket → timerfd → signalfd)
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGHUP);
        // 命门:先 block SIGHUP 默认递送。不 block,SIGHUP 默认行为是【终止进程】,
        //       signalfd 根本读不到。block 后信号转由 signalfd 这条路读出。
        if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
            LOG_WARN("sigprocmask failed: %s — 热加载不可用", strerror(errno));
        }
        int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sfd == -1) {
            LOG_WARN("signalfd failed: %s — 热加载不可用", strerror(errno));
        }

        // signalfd 建起来才挂 Channel(降级:建不起来网关照常采集,只是不能热加载)
        if (sfd != -1) {
            auto sig_channel = std::make_shared<gateway::channel>();
            sig_channel->fd     = sfd;
            sig_channel->events = EPOLLIN;   // 信号 fd 用 LT 即可
            sig_channel->on_read =
                [sfd, &loop, &db, &client, &port, &serial_channel]() {
                struct signalfd_siginfo si;
                // 必须读掉 siginfo,否则 LT 反复触发。while 读到 EAGAIN(SFD_NONBLOCK)。
                while (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                    if (si.ssi_signo != SIGHUP) continue;

                    LOG_INFO("%s", "SIGHUP received, reloading config...");
                    auto r = gateway::ConfigManager::reload();
                    if (!r.ok) {
                        // load-then-swap 保证旧配置原封不动,无需任何回滚动作
                        LOG_WARN("%s","reload failed, keep running with old config");
                        continue;
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
                        db = std::make_shared<gateway::Database>(ncfg->db_path);
                    }
                    // mqtt:unique/shared_ptr 替换绕过 MqttClient 不可移动。
                    //       旧 client 析构(stop loop/断连/destroy),新 client 建好。
                    if (r.mqtt_changed) {
                        LOG_INFO("%s","mqtt config changed → reconnecting");
                        client = std::make_shared<gateway::MqttClient>(
                            "gateway-main", ncfg->mqtt_host, ncfg->mqtt_port,
                            ncfg->mqtt_keepalive);
                        client->setMessageHandler([](const std::string& t, const std::string& p){
                            LOG_INFO("downlink cmd: %s = %s (not handled yet)",
                                     t.c_str(), p.c_str());
                        });
                        client->subscribe("gateway/cmd/#", 1);
                        client->loopStart();
                    }
                    // serial:fd 在 epoll 里,要连 Channel 一起换。
                    //   先 removeChannel 摘旧 fd(进 dying_,批末析构 close 旧 fd),
                    //   再重建 port,再建【全新】channel 指向新 fd ——
                    //   绝不复用旧 channel 改 fd:那会让旧 channel 的析构 close 错 fd
                    //   (shared_ptr 别名坑:旧 channel 已在 dying_ 里)。
                    if (r.serial_changed) {
                        LOG_INFO("%s","serial config changed → reopening port");
                        loop.removeChannel(serial_channel->fd);
                        port = std::make_shared<gateway::SerialPort>(
                            ncfg->serial_path.c_str(), toBaud(ncfg->serial_baud),
                            /*nonblock=*/true);
                        auto new_ch = std::make_shared<gateway::channel>();
                        new_ch->fd      = port->get();
                        new_ch->events  = EPOLLIN | EPOLLET;
                        new_ch->on_read = serial_channel->on_read;  // 逻辑不变(引用捕获 port,自动用新 port)
                        loop.addChannel(new_ch);
                        serial_channel = new_ch;   // 外层变量指向新 channel(下次 SIGHUP 用新 fd)
                    }
                    LOG_INFO("%s","reload done");
                }
            };
            loop.addChannel(sig_channel);
        }

        loop.loop();   // 主循环:永久阻塞,串口数据 + SIGHUP 统一事件驱动

    } catch (const std::exception& e) {
        // 串口打不开 / broker 连不上 / 磁盘异常等统一在此致命退出
        LOG_ERROR("gateway fatal: %s", e.what());
        return 1;
    }
    return 0;
}