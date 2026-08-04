// ============================================================
// node-sim —— STM32 传感节点模拟器
//
// 由 experiments/m5_parser/fake_stm32.cpp 提升而来。旧版只会单向发数据、
// 不认下行命令,README 里得写一句"虚拟串口下下行命令会重试 3 次后判失败,
// 要看完整 ACK 闭环需真实固件"。也就是说,占网关一半代码量的命令链路,
// 在没有硬件时【完全没法验证】。
//
// 本版补上了这半边:解析下行 0x20/0x21/0x22,按节点固件 App/cmd_service.c
// 的同一套规则回 0x05/0x06,含 §6.2 同 seq 幂等。于是 socat 一对虚拟串口
// 就能跑通「下发 → 执行 → 应答 → 销账」的全流程。
//
// 更要紧的是它能【主动制造故障】:丢应答、丢上行帧、注入噪声字节 ——
// 这些在真硬件上要靠拔线和运气,这里是命令行参数。
//
// 组帧解帧直接用与网关、固件共享的 edge_proto,不再像旧版那样自抄一份
// buildFrame + CRC(那第三份抄本用的还是早已改名的 m5::CRC16)。
// ============================================================

#include "edge_proto/edge_frame.h"
#include "edge_proto/edge_proto.h"

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

// ---------------- 运行参数 ----------------
struct Options {
    std::string device;
    int         period_s   = 2;      // 上报周期(秒)
    double      drop_uplink = 0.0;   // 丢弃上行帧的概率(验证网关侧容错)
    double      drop_ack    = 0.0;   // 丢弃应答的概率(验证网关侧超时重发)
    double      garbage     = 0.0;   // 每帧前注入噪声字节的概率(验证 FSM resync)
    bool        no_ack      = false; // 完全不回应答(验证重试耗尽判死)
    unsigned    seed        = 42;    // 固定种子:故障注入要可复现
};

void usage(const char* argv0) {
    std::fprintf(stderr,
        "STM32 传感节点模拟器 —— 上报遥测 + 响应下行命令(含同 seq 幂等)\n"
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

double parseProb(const char* s, const char* name) {
    char*        end = nullptr;
    const double v   = std::strtod(s, &end);
    if (end == s || *end != '\0' || v < 0.0 || v > 1.0) {
        std::fprintf(stderr, "%s 需要 0..1 之间的小数,收到 '%s'\n", name, s);
        std::exit(2);
    }
    return v;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    if (argc < 2) return false;
    if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) return false;
    opt.device = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto              need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 缺少参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--period")           opt.period_s    = std::atoi(need("--period"));
        else if (a == "--drop-ack")    opt.drop_ack    = parseProb(need("--drop-ack"), "--drop-ack");
        else if (a == "--drop-uplink") opt.drop_uplink = parseProb(need("--drop-uplink"), "--drop-uplink");
        else if (a == "--garbage")     opt.garbage     = parseProb(need("--garbage"), "--garbage");
        else if (a == "--no-ack")      opt.no_ack      = true;
        else if (a == "--seed")        opt.seed        = static_cast<unsigned>(std::atoi(need("--seed")));
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

// ---------------- 串口 ----------------
// 与网关侧 SerialPort 同样的 raw 模式配置。虚拟串口(PTY)同样是 termios 设备,
// 不设 raw 的话,\n 会被转成 \r\n,二进制帧当场损坏 —— 这是联调时的经典坑。
int openSerial(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        std::fprintf(stderr, "打不开 %s: %s\n", path.c_str(), std::strerror(errno));
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 1;   // 0.1s 超时,让读循环能定期回来看看该不该上报
        tcsetattr(fd, TCSANOW, &tio);
    }
    return fd;
}

// ---------------- 模拟节点 ----------------
class NodeSim {
public:
    NodeSim(int fd, const Options& opt)
        : fd_(fd), opt_(opt), rng_(opt.seed) {}

    void run() {
        edge_parser_t parser;
        edge_parser_init(&parser, &NodeSim::onFrameTrampoline, this);

        auto next_report = std::chrono::steady_clock::now();

        while (true) {
            // 读串口:非阻塞式轮询(VTIME=1 → 最多等 0.1s)
            uint8_t buf[128];
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n > 0) {
                edge_parser_feed_buf(&parser, buf, static_cast<size_t>(n));
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                std::fprintf(stderr, "读串口失败: %s\n", std::strerror(errno));
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_report) {
                reportOnce();
                next_report = now + std::chrono::seconds(opt_.period_s);
            }
        }
    }

private:
    static void onFrameTrampoline(uint8_t type, const uint8_t* p, uint8_t len, void* user) {
        static_cast<NodeSim*>(user)->onFrame(type, p, len);
    }

    bool roll(double p) {
        if (p <= 0.0) return false;
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p;
    }

    // 发一帧。garbage 注入发生在帧【之前】—— 模拟线路噪声,
    // 网关的 FSM 应当把这些字节静默丢弃后照常解出后面的帧。
    void sendFrame(uint8_t type, const uint8_t* payload, uint8_t len) {
        if (roll(opt_.garbage)) {
            const uint8_t noise[3] = {0xFF, 0xAA, 0x13};   // 含一个 0xAA 以考验帧头自环
            ::write(fd_, noise, sizeof noise);
        }
        uint8_t       frame[EDGE_FRAME_MAX];
        const uint8_t n = edge_frame_encode(type, payload, len, frame);
        if (n == 0) return;
        if (::write(fd_, frame, n) != n) {
            std::fprintf(stderr, "写串口不完整\n");
        }
    }

    // ---- 上行:周期上报遥测(对应固件 App/report_task)----
    void reportOnce() {
        // 造点会动的假数据,便于在监控页上看出曲线
        tick_++;
        const uint16_t temp_x10 = static_cast<uint16_t>(230 + (tick_ % 50));   // 23.0 ~ 27.9 ℃
        const uint16_t humi_x10 = static_cast<uint16_t>(500 + (tick_ % 200));  // 50.0 ~ 69.9 %
        const uint16_t lux      = static_cast<uint16_t>(100 + (tick_ * 7) % 900);

        if (!roll(opt_.drop_uplink)) {
            uint8_t dht[5];
            edge_u16_be_write(&dht[0], temp_x10);
            edge_u16_be_write(&dht[2], humi_x10);
            dht[4] = static_cast<uint8_t>((dht[0] + dht[1] + dht[2] + dht[3]) & 0xFF);
            sendFrame(EDGE_TYPE_DHT11, dht, sizeof dht);
        }
        if (!roll(opt_.drop_uplink)) {
            uint8_t bh[2];
            edge_u16_be_write(bh, lux);
            sendFrame(EDGE_TYPE_BH1750, bh, sizeof bh);
        }

        const uint8_t status = EDGE_STATUS_BIT_DHT11 | EDGE_STATUS_BIT_BH1750;
        sendFrame(EDGE_TYPE_STATUS, &status, 1);
        sendFrame(EDGE_TYPE_HEARTBEAT, nullptr, 0);

        std::printf("[上报] 温=%.1f℃ 湿=%.1f%% 光照=%u lux\n",
                    temp_x10 / 10.0, humi_x10 / 10.0, lux);
    }

    // ---- 下行:命令分发(对应固件 App/cmd_service.c 的 on_frame)----
    void onFrame(uint8_t type, const uint8_t* payload, uint8_t len) {
        if (len < 1) return;   // 无 payload 的帧无从应答
        const uint8_t seq = payload[EDGE_OFF_SEQ];

        // §6.2 幂等:与上次处理过的 seq 相同 → 判为重发,
        // 不重复执行命令本体,只补发上次的应答。
        // 这条正是网关"重发复用同 seq"的另一半,没有它整个契约只对了一半。
        if (have_last_ && seq == last_seq_) {
            std::printf("[命令] seq=%u 重发 → 补发上次应答(不重复执行)\n", seq);
            resendLastResponse();
            return;
        }

        switch (type) {
        case EDGE_TYPE_QUERY_LIGHT: {
            const uint16_t lux = static_cast<uint16_t>(100 + (tick_ * 7) % 900);
            uint8_t        p[4] = {seq, EDGE_RC_OK, 0, 0};
            edge_u16_be_write(&p[2], lux);
            std::printf("[命令] seq=%u 查光照 → %u lux\n", seq, lux);
            respond(EDGE_TYPE_QUERY_RESP, p, 4, seq);
            break;
        }
        case EDGE_TYPE_QUERY_TH: {
            uint8_t p[6] = {seq, EDGE_RC_OK, 0, 0, 0, 0};
            edge_u16_be_write(&p[2], static_cast<uint16_t>(230 + (tick_ % 50)));
            edge_u16_be_write(&p[4], static_cast<uint16_t>(500 + (tick_ % 200)));
            std::printf("[命令] seq=%u 查温湿度\n", seq);
            respond(EDGE_TYPE_QUERY_RESP, p, 6, seq);
            break;
        }
        case EDGE_TYPE_SET_PERIOD: {
            uint8_t rc = EDGE_RC_OK;
            if (!edge_payload_len_ok(type, len)) {
                rc = EDGE_RC_BAD_PARAM;               // 缺周期字节
            } else {
                const uint16_t period_s = edge_u16_be_read(&payload[1]);
                if (!edge_period_s_valid(period_s)) {
                    rc = EDGE_RC_BAD_PARAM;           // §6.3:周期 0 非法
                } else {
                    opt_.period_s = period_s;         // 单位是【秒】(v1.2 澄清)
                    std::printf("[命令] seq=%u 设采样周期 = %u 秒\n", seq, period_s);
                }
            }
            if (rc != EDGE_RC_OK) {
                std::printf("[命令] seq=%u 设周期被拒 rc=0x%02X\n", seq, rc);
            }
            const uint8_t p[2] = {seq, rc};
            respond(EDGE_TYPE_ACK, p, 2, seq);
            break;
        }
        default: {
            // §6.3:未知下行命令,或收到本该是上行的 TYPE(说明线接反了 / 有人乱发)
            const uint8_t p[2] = {seq, EDGE_RC_UNSUPPORTED};
            std::printf("[命令] seq=%u 不支持的 TYPE 0x%02X%s\n", seq, type,
                        EDGE_IS_UPLINK(type) ? " (这是个上行 TYPE)" : "");
            respond(EDGE_TYPE_ACK, p, 2, seq);
            break;
        }
        }
    }

    // 应答统一走这里:存档以便同 seq 重发时补发,并在此处实施故障注入
    void respond(uint8_t type, const uint8_t* p, uint8_t len, uint8_t seq) {
        last_type_ = type;
        last_len_  = len;
        std::memcpy(last_payload_, p, len);
        last_seq_  = seq;
        have_last_ = true;

        if (opt_.no_ack) {
            std::printf("       └─ 应答被丢弃(--no-ack)\n");
            return;
        }
        if (roll(opt_.drop_ack)) {
            std::printf("       └─ 应答被丢弃(--drop-ack),网关应重发\n");
            return;
        }
        sendFrame(type, p, len);
    }

    void resendLastResponse() {
        if (!have_last_ || opt_.no_ack) return;
        if (roll(opt_.drop_ack)) {
            std::printf("       └─ 补发的应答又被丢弃\n");
            return;
        }
        sendFrame(last_type_, last_payload_, last_len_);
    }

    int          fd_;
    Options      opt_;
    std::mt19937 rng_;
    unsigned     tick_ = 0;

    // §6.2 幂等所需的"最近一条命令"存档
    bool    have_last_ = false;
    uint8_t last_seq_  = 0;
    uint8_t last_type_ = 0;
    uint8_t last_len_  = 0;
    uint8_t last_payload_[EDGE_PAYLOAD_MAX] = {};
};

}  // namespace

int main(int argc, char** argv) {
    // 行缓冲。stdout 不是终端时(重定向到文件、管道给 tee、被测试脚本抓)
    // 默认是【全缓冲】—— 4KB 攒满才落盘。本程序输出稀疏,于是日志会长时间
    // 卡在缓冲区里,看上去像"什么都没发生"。联调工具的输出必须即时可见。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        usage(argv[0]);
        return 2;
    }

    const int fd = openSerial(opt.device);
    if (fd < 0) return 1;

    std::printf("node-sim 已启动: %s  周期=%d秒\n", opt.device.c_str(), opt.period_s);
    if (opt.no_ack)          std::printf("  故障注入: 不回任何应答\n");
    if (opt.drop_ack > 0)    std::printf("  故障注入: 丢应答概率 %.0f%%\n", opt.drop_ack * 100);
    if (opt.drop_uplink > 0) std::printf("  故障注入: 丢上行帧概率 %.0f%%\n", opt.drop_uplink * 100);
    if (opt.garbage > 0)     std::printf("  故障注入: 噪声字节概率 %.0f%%\n", opt.garbage * 100);

    NodeSim sim(fd, opt);
    sim.run();

    ::close(fd);
    return 0;
}
