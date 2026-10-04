// STM32 节点的主机侧模拟器：通过串口周期发送遥测，并执行网关下发的查询和设置命令。
// 模拟器复用共享 edge_proto 编解码，通过概率化丢帧和噪声注入验证网关的重试、
// 重新同步及失败收敛；业务处理独立实现，避免复用网关逻辑掩盖端到端问题。

#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_proto.h"
#include "edge_proto/edge_sr.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

namespace {

// 一次模拟器进程的完整运行配置。概率字段均取闭区间 [0, 1]，固定随机种子使
// e2e_vserial.sh 中的故障序列可以稳定复现。
struct Options {
    std::string device;       // 节点侧串口或 PTY 设备路径。
    int period_s = 2;         // 周期遥测的发送间隔，单位为秒。
    double drop_uplink = 0.0; // 每个温湿度或光照上行帧的独立丢弃概率。
    double drop_ack = 0.0;    // 首次响应和重试响应的独立丢弃概率。
    double garbage = 0.0;     // 在每个合法帧前插入噪声序列的概率。
    bool no_ack = false;      // 为 true 时缓存响应但始终不向网关发送。
    unsigned seed = 42;       // std::mt19937 的初始种子。
};

// 输出命令行语法和故障注入示例；argv0 是用户调用程序时使用的可执行文件名。
void usage(const char* argv0) {
    std::fprintf(stderr,
                 "STM32 传感节点模拟器 —— 上报遥测 + 响应下行命令(SR 有序执行)\n"
                 "\n"
                 "用法: %s <串口设备> [选项]\n"
                 "\n"
                 "选项:\n"
                 "  --period <秒>     上报周期,默认 2\n"
                 "  --drop-ack <p>    以概率 p 丢弃应答,验证网关超时重发(0..1)\n"
                 "  --drop-uplink <p> 以概率 p 丢弃上行遥测帧(0..1)\n"
                 "  --garbage <p>     以概率 p 在帧前注入噪声字节,验证 FSM resync(0..1)\n"
                 "  --no-ack          完全不回应答,验证重试耗尽后判失败\n"
                 "  --seed <n>        故障注入随机种子,默认 42(固定以便复现)\n"
                 "  -h, --help        显示本帮助\n"
                 "\n"
                 "示例:\n"
                 "  %s /tmp/ttyV1                    # 正常节点\n"
                 "  %s /tmp/ttyV1 --drop-ack 0.5     # 一半应答丢失,应能看到重发后成功\n"
                 "  %s /tmp/ttyV1 --no-ack           # 应能看到 3 次重发后 FAILED\n"
                 "  %s /tmp/ttyV1 --garbage 0.3      # 噪声干扰下仍应正常解析\n",
                 argv0, argv0, argv0, argv0, argv0);
}

// 将选项文本 s 严格解析为概率；name 只用于生成可定位的错误信息。
// 非数字、尾随字符或超出 [0, 1] 的值属于命令行错误，直接以状态 2 退出。
double parseProb(const char* s, const char* name) {
    char* end = nullptr;
    const double v = std::strtod(s, &end); // end 指向首个未消费字符，用于拒绝部分解析。
    if (end == s || *end != '\0' || v < 0.0 || v > 1.0) {
        std::fprintf(stderr, "%s 需要 0..1 之间的小数,收到 '%s'\n", name, s);
        std::exit(2);
    }
    return v;
}

// 解析 argc/argv 并写入 opt。缺少设备、请求帮助或遇到未知选项时返回 false，
// 数值选项缺参数或越界时直接退出；调用方据返回值统一打印 usage。
bool parseArgs(int argc, char** argv, Options& opt) {
    if (argc < 2) return false;
    if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) return false;
    opt.device = argv[1]; // 第一个位置参数固定为串口设备。

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i]; // 当前待分派的选项名。
        // 取得当前选项的下一个参数，并同步推进外层循环下标 i。
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 缺少参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--period")
            opt.period_s = std::atoi(need("--period"));
        else if (a == "--drop-ack")
            opt.drop_ack = parseProb(need("--drop-ack"), "--drop-ack");
        else if (a == "--drop-uplink")
            opt.drop_uplink = parseProb(need("--drop-uplink"), "--drop-uplink");
        else if (a == "--garbage")
            opt.garbage = parseProb(need("--garbage"), "--garbage");
        else if (a == "--no-ack")
            opt.no_ack = true;
        else if (a == "--seed")
            opt.seed = static_cast<unsigned>(std::atoi(need("--seed")));
        else {
            std::fprintf(stderr, "未知选项: %s\n", a.c_str());
            return false;
        }
    }
    if (opt.period_s < 1) {
        std::fprintf(stderr, "--period 必须 >= 1\n");
        std::exit(2);
    }
    return true;
}

// 打开 path 指定的串口并配置为 115200、raw、短超时读取。PTY 同样应用 termios；
// raw 模式可防止换行转换和回显修改二进制协议字节。成功返回 fd，失败返回 -1。
int openSerial(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY); // 不让设备成为进程控制终端。
    if (fd < 0) {
        std::fprintf(stderr, "打不开 %s: %s\n", path.c_str(), std::strerror(errno));
        return -1;
    }
    struct termios tio; // 基于设备现有属性修改，保留平台相关的未触及字段。
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 1; // 十分之一秒超时，让事件循环定期检查上报时刻。
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

// 单线程节点状态机。串口接收和周期上报在同一循环推进，避免测试工具自身引入
// 跨线程竞态；命令响应缓存与固件使用同一份有界 C 实现。
class NodeSim {
public:
    // fd 由 main 持有并在 run 返回后关闭；opt 按值保存，以便 set_period 修改运行周期。
    NodeSim(int fd, const Options& opt) : fd_(fd), opt_(opt), rng_(opt.seed) {
        edge_sr_node_init(&sr_);
    }

    // 持续读取下行字节并按 steady_clock 触发上报，直至串口发生不可恢复的读取错误。
    void run() {
        edge_parser_t parser; // 增量解析任意分片到达的下行字节流。
        edge_parser_init(&parser, &NodeSim::onFrameTrampoline, this);

        auto next_report = std::chrono::steady_clock::now(); // 启动后立即发送第一批遥测。

        while (true) {
            uint8_t buf[128]; // 单次串口读取缓冲，不要求与协议帧边界对齐。
            const ssize_t n = ::read(fd_, buf, sizeof buf); // VTIME 限制最长阻塞约 0.1 秒。
            if (n > 0) {
                edge_parser_feed_buf(&parser, buf, static_cast<size_t>(n));
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                std::fprintf(stderr, "读串口失败: %s\n", std::strerror(errno));
                return;
            }

            const auto now = std::chrono::steady_clock::now(); // 单调时钟不受系统时间校准影响。
            edge_sr_node_tick(&sr_, &hooks_, this);
            if (now >= next_report) {
                reportOnce();
                next_report = now + std::chrono::seconds(opt_.period_s);
            }
        }
    }

private:
    // 适配 edge_parser_t 的 C 回调：type/p/len 是完整帧内容，user 恢复当前 NodeSim 实例。
    static void onFrameTrampoline(uint8_t type, const uint8_t* p, uint8_t len, void* user) {
        static_cast<NodeSim*>(user)->onFrame(type, p, len);
    }

    // 以概率 p 返回 true；p<=0 的常用路径不推进随机数状态。
    bool roll(double p) {
        if (p <= 0.0) return false;
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
    }

    // 编码并发送 type/payload/len 描述的协议帧。可先写入固定噪声序列，使网关解析器
    // 必须在同一字节流中重新同步；payload 可在 len 为 0 时取 nullptr。
    void sendFrame(uint8_t type, const uint8_t* payload, uint8_t len) {
        uint8_t frame[EDGE_FRAME_MAX];
        const uint8_t n = edge_frame_encode(type, payload, len, frame);
        if (n != 0) sendRaw(frame, n);
    }

    void sendRaw(const uint8_t* frame, uint8_t len) {
        if (roll(opt_.garbage)) {
            // 0xAA 与帧头首字节相同，覆盖解析器等待第二个帧头字节时的自环路径。
            const uint8_t noise[3] = {0xFF, 0xAA, 0x13};
            // 噪声写失败只会减少故障注入，不应阻止随后发送合法帧。
            if (::write(fd_, noise, sizeof noise) < 0) {
                // 已消费返回值；故障注入失败不改变合法帧的发送流程。
            }
        }
        if (::write(fd_, frame, len) != len) {
            std::fprintf(stderr, "写串口不完整\n");
        }
    }

    // 生成并发送一轮温湿度、光照、状态和心跳。两个测量帧分别执行丢包抽样，
    // 状态和心跳始终发送，以便同时观察遥测缺失与链路存活。
    void reportOnce() {
        tick_++; // 确定性推进，让监控输出形成可辨识且可复现的变化曲线。
        const uint16_t temp_x10 = static_cast<uint16_t>(230 + (tick_ % 50));  // 23.0～27.9 ℃。
        const uint16_t humi_x10 = static_cast<uint16_t>(500 + (tick_ % 200)); // 50.0～69.9 %。
        const uint16_t lux = static_cast<uint16_t>(100 + (tick_ * 7) % 900);  // 100～999 lux。

        if (!roll(opt_.drop_uplink)) {
            uint8_t dht[4]; // 温度和湿度各占一个大端 uint16_t。
            edge_u16_be_write(&dht[0], temp_x10);
            edge_u16_be_write(&dht[2], humi_x10);
            sendFrame(EDGE_TYPE_DHT11, dht, sizeof dht);
        }
        if (!roll(opt_.drop_uplink)) {
            uint8_t bh[2]; // 光照值为单个大端 uint16_t。
            edge_u16_be_write(bh, lux);
            sendFrame(EDGE_TYPE_BH1750, bh, sizeof bh);
        }

        const uint8_t status = EDGE_STATUS_BIT_DHT11 | EDGE_STATUS_BIT_BH1750; // 模拟两传感器在线。
        sendFrame(EDGE_TYPE_STATUS, &status, 1);
        sendFrame(EDGE_TYPE_HEARTBEAT, nullptr, 0);

        std::printf("[上报] 温=%.1f℃ 湿=%.1f%% 光照=%u lux\n", temp_x10 / 10.0, humi_x10 / 10.0,
                    lux);
    }

    void onFrame(uint8_t type, const uint8_t* payload, uint8_t len) {
        edge_sr_node_on_frame(&sr_, type, payload, len, &hooks_, this);
    }

    static uint8_t execute(uint8_t seq, uint8_t type, const uint8_t* args, uint8_t len,
                           uint8_t* result, void* user) {
        auto& self = *static_cast<NodeSim*>(user);
        result[0] = EDGE_RC_BAD_PARAM;
        switch (type) {
        case EDGE_TYPE_QUERY_LIGHT:
            if (len != 0) return 1;
            result[0] = EDGE_RC_OK;
            edge_u16_be_write(result + 1, static_cast<uint16_t>(100 + (self.tick_ * 7) % 900));
            std::printf("[命令] seq=%u 查光照\n", seq);
            return 3;
        case EDGE_TYPE_QUERY_TH:
            if (len != 0) return 1;
            result[0] = EDGE_RC_OK;
            edge_u16_be_write(result + 1, static_cast<uint16_t>(230 + self.tick_ % 50));
            edge_u16_be_write(result + 3, static_cast<uint16_t>(500 + self.tick_ % 200));
            std::printf("[命令] seq=%u 查温湿度\n", seq);
            return 5;
        case EDGE_TYPE_SET_PERIOD:
            if (len == 2 && edge_period_s_valid(edge_u16_be_read(args))) {
                self.opt_.period_s = edge_u16_be_read(args);
                result[0] = EDGE_RC_OK;
                std::printf("[命令] seq=%u 设采样周期 = %u 秒\n", seq,
                            static_cast<unsigned>(self.opt_.period_s));
            }
            return 1;
        default:
            result[0] = EDGE_RC_UNSUPPORTED;
            return 1;
        }
    }

    static void emit(uint8_t type, const uint8_t* p, uint8_t len, void* user) {
        auto& self = *static_cast<NodeSim*>(user);
        if (self.opt_.no_ack || self.roll(self.opt_.drop_ack)) return;
        self.sendFrame(type, p, len);
    }

    static uint32_t nowMs() {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    int fd_;            // 已配置的串口描述符，生命周期由 main 管理。
    Options opt_;       // 当前运行配置；set_period 会更新 period_s。
    std::mt19937 rng_;  // 所有故障注入共用的确定性随机数引擎。
    unsigned tick_ = 0; // 已生成的遥测轮次，也是模拟读数的变化输入。

    edge_sr_node_t sr_{};
    inline static const edge_sr_hooks_t hooks_{
        [](void*) { return nowMs(); }, &NodeSim::execute, &NodeSim::emit};
};

} // namespace

int main(int argc, char** argv) {
    // argc/argv 提供设备和故障注入参数。stdout 强制行缓冲，使输出重定向到文件时，
    // e2e 脚本仍能及时观察启动标志、命令处理和重试日志。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Options opt; // 先填入默认值，再由命令行覆盖。
    if (!parseArgs(argc, argv, opt)) {
        usage(argv[0]);
        return 2;
    }

    const int fd = openSerial(opt.device); // main 保留所有权，并在模拟循环结束后关闭。
    if (fd < 0) return 1;

    std::printf("node-sim 已启动: %s  周期=%d秒\n", opt.device.c_str(), opt.period_s);
    if (opt.no_ack) std::printf("  故障注入: 不回任何应答\n");
    if (opt.drop_ack > 0) std::printf("  故障注入: 丢应答概率 %.0f%%\n", opt.drop_ack * 100);
    if (opt.drop_uplink > 0)
        std::printf("  故障注入: 丢上行帧概率 %.0f%%\n", opt.drop_uplink * 100);
    if (opt.garbage > 0) std::printf("  故障注入: 噪声字节概率 %.0f%%\n", opt.garbage * 100);

    NodeSim sim(fd, opt); // 同步运行；正常情况下由外部信号结束进程。
    sim.run();

    ::close(fd);
    return 0;
}
