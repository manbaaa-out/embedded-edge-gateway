// ConfigManager 的进程级契约测试：覆盖文本解析、默认值、统一校验、热加载原子替换
// 以及资源重建标志。gtest_discover_tests 为每个用例启动独立进程，因此单例不会跨用例残留。

#include "gateway/core/config/Config.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace gateway;

namespace {

// 由用例标签 tag 生成独立临时路径，防止并行 CTest 进程互相覆盖。
std::string confPath(const std::string& tag) {
    return "/tmp/gateway_config_test_" + tag + ".conf";
}

// 将 content 完整覆盖写入 path；关闭 ofstream 时刷新，随后 ConfigManager 才读取。
void writeConf(const std::string& path, const std::string& content) {
    std::ofstream o(path); // 当前写入流的生命周期限定在一次配置更新。
    o << content;
}

// 返回一份可通过完整校验的基础配置；overrides 追加在末尾，利用解析器的后值覆盖前值
// 机制让用例只声明关心的变更。
std::string baseline(const std::string& overrides = "") {
    return "# 注释行应被跳过\n"
           "log_level = 2\n"
           "serial_path = /tmp/ttyV9\n"
           "serial_baud = 9600\n"
           "mqtt_host = 192.168.1.5\n"
           "mqtt_port = 1884\n"
           "http_port = 8890\n" +
           overrides;
}

// 临时配置 RAII 对象。path 是本用例文件名；构造只分配路径，write 写入内容，析构
// 删除文件，因而异常断言和提前返回也不会污染后续运行。
struct ConfFile {
    std::string path; // ConfigManager::init/reload 实际读取的文件路径。
    explicit ConfFile(const std::string& tag) : path(confPath(tag)) {}
    ~ConfFile() { std::remove(path.c_str()); }
    void write(const std::string& content) const { writeConf(path, content); }
};

} // namespace

// 所有显式字段都应从文本进入 current 快照，且注释行不会干扰解析。
TEST(Config, InitParsesAllFields) {
    ConfFile f("parse"); // 当前用例的完整配置文件。
    f.write(baseline());

    ASSERT_NO_THROW(ConfigManager::init(f.path));
    auto c = ConfigManager::current(); // 初始化后发布的不可变配置快照。
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(c->log_level, 2);
    EXPECT_EQ(c->serial_path, "/tmp/ttyV9");
    EXPECT_EQ(c->serial_baud, 9600);
    EXPECT_EQ(c->mqtt_host, "192.168.1.5");
    EXPECT_EQ(c->mqtt_port, 1884);
    EXPECT_EQ(c->http_port, 8890);
}

// 缺失字段沿用 Config 的结构体默认值。
TEST(Config, MissingKeysKeepDefaults) {
    ConfFile f("defaults"); // 只显式设置日志级别的最小配置。
    f.write("log_level = 3\n");

    ASSERT_NO_THROW(ConfigManager::init(f.path));
    auto c = ConfigManager::current(); // 用于同时检查显式值与结构体默认值。

    EXPECT_EQ(c->log_level, 3);
    EXPECT_EQ(c->idle_timeout, 5) << "未配置的键应取默认值";
    EXPECT_EQ(c->report_n, 10);
    EXPECT_EQ(c->mqtt_keepalive, 60);
}

// 差异标志只报告实际变更，供装配层决定是否重建外部资源。
TEST(Config, ReloadReportsOnlyWhatActuallyChanged) {
    ConfFile f("diff"); // 初始配置与后续版本共用同一路径。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write("log_level = 0\n"
            "serial_path = /tmp/ttyVX\n" // 触发串口重建。
            "serial_baud = 9600\n"       // 保持原值。
            "mqtt_host = 192.168.1.5\n"  // 保持原值。
            "mqtt_port = 1884\n"
            "http_port = 8890\n");

    auto r = ConfigManager::reload(); // 包含成功状态和三类资源差异标志。
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.serial_changed);
    EXPECT_FALSE(r.mqtt_changed) << "host 与 keepalive 都没动,不该重连";
    EXPECT_FALSE(r.db_changed);

    EXPECT_EQ(ConfigManager::current()->log_level, 0) << "A 档改内存即生效";
    EXPECT_EQ(ConfigManager::current()->serial_path, "/tmp/ttyVX");
}

// 同时改变 Broker 地址和数据库路径时只标记相应资源，串口保持不变。
TEST(Config, ReloadDetectsMqttAndDbChanges) {
    ConfFile f("diff2"); // 追加覆盖 MQTT 和数据库字段的配置文件。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("mqtt_host = 10.0.0.1\ndb_path = /tmp/other.db\n"));

    auto r = ConfigManager::reload(); // 本次应同时报告 mqtt_changed 与 db_changed。
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.mqtt_changed);
    EXPECT_TRUE(r.db_changed);
    EXPECT_FALSE(r.serial_changed) << "串口没动就不该重开";
}

// 监听端口只在启动时绑定，热加载忽略这类字段。
TEST(Config, TierCChangesAreIgnored) {
    ConfFile f("tierc"); // 尝试覆盖两个启动期监听端口。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("mqtt_port = 9999\nhttp_port = 7777\n"));

    auto r = ConfigManager::reload(); // reload 本身成功，但保留启动快照中的端口。
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(ConfigManager::current()->mqtt_port, 1884) << "应保持启动值而非 9999";
    EXPECT_EQ(ConfigManager::current()->http_port, 8890) << "监听端口不可热改";
}

// 新文件完成解析和校验前，不替换当前配置。
TEST(Config, ParseFailureKeepsOldConfigIntact) {
    ConfFile f("badvalue"); // 先加载合法值，再原地写入不可解析文本。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const int old_baud = ConfigManager::current()->serial_baud; // 失败后必须保留的已发布值。

    f.write("serial_baud = abc\n"); // 数值转换失败使整次热加载失效。

    auto r = ConfigManager::reload(); // 失败结果不得伴随部分配置提交。
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(ConfigManager::current()->serial_baud, old_baud) << "旧值必须原封不动";
}

// 端口超出 1～65535 时整次热加载失败，包括同文件中的其他合法变更。
TEST(Config, ValidationRejectsOutOfRangePort) {
    ConfFile f("badport"); // 同时包含新串口路径和非法端口。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const auto before = ConfigManager::current()->serial_path; // 检查无部分提交的基线。

    f.write("serial_path = /tmp/ttyVX\n"
            "serial_baud = 9600\n"
            "mqtt_host = x\n"
            "http_port = 70000\n"); // 超出有效 TCP 端口范围。

    auto r = ConfigManager::reload(); // validation 失败结果。
    EXPECT_FALSE(r.ok) << "validate 该拦下越界端口";
    EXPECT_EQ(ConfigManager::current()->serial_path, before) << "失败后旧配置不动";
}

// 连接资源所需路径不能为空白，避免错误推迟到 open 调用才暴露。
TEST(Config, ValidationRejectsEmptyRequiredPaths) {
    ConfFile f("emptypath"); // 用于写入空 serial_path 的配置文件。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("serial_path = \n"));

    auto r = ConfigManager::reload(); // 空 serial_path 的统一校验结果。
    EXPECT_FALSE(r.ok) << "空串口路径应被 validate 拦下";
}

// 启动配置不可用时应立即失败，不能静默使用默认连接参数。
TEST(Config, InitThrowsOnMissingFile) {
    EXPECT_THROW(ConfigManager::init("/tmp/definitely_not_here_9f3a.conf"), std::exception);
}

// 初始化与热加载共享同一套字段校验规则。
TEST(Config, InitRejectsWhatReloadWouldReject) {
    ConfFile f("initvalidate");               // 直接走 init 的非法端口配置。
    f.write(baseline("http_port = 70000\n")); // 启动路径也必须拒绝越界端口。

    EXPECT_THROW(ConfigManager::init(f.path), std::exception)
        << "启动期必须和热加载一样拦下越界端口";
}

// idle_timeout 控制连接生命周期，零或负值会使所有连接立即过期，启动时必须拒绝。
TEST(Config, InitRejectsNonPositiveIdleTimeout) {
    ConfFile f("initidle"); // idle_timeout 下界的启动配置。
    f.write(baseline("idle_timeout = 0\n"));

    EXPECT_THROW(ConfigManager::init(f.path), std::exception);
}

// 配置入口直接拒绝超限查询条数，避免 HTTP 层静默改写用户配置。
TEST(Config, ValidationRejectsReportNAboveMax) {
    ConfFile f("reportnmax"); // 先合法初始化、再写入超限 report_n。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const int before = ConfigManager::current()->report_n; // 超限 reload 后应保持的旧值。

    f.write(baseline("report_n = 5000\n"));

    auto r = ConfigManager::reload(); // report_n 超过共享上限的失败结果。
    EXPECT_FALSE(r.ok) << "超过 kMaxReportN 应被拦下,而不是留给 HTTP 层静默夹紧";
    EXPECT_EQ(ConfigManager::current()->report_n, before) << "失败后旧值不动";
}

// kMaxReportN 本身属于合法闭区间端点，应被原样提交而不是误判越界。
TEST(Config, ValidationAcceptsReportNAtExactlyMax) {
    ConfFile f("reportnedge"); // report_n 合法上界的热加载配置。
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("report_n = " + std::to_string(kMaxReportN) + "\n"));

    auto r = ConfigManager::reload(); // 上限边界的成功热加载结果。
    EXPECT_TRUE(r.ok) << "上限本身是合法值,边界不能少算一个";
    EXPECT_EQ(ConfigManager::current()->report_n, kMaxReportN);
}
