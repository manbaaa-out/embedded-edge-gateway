// 配置解析与热加载测试。
//
// ConfigManager 是进程级单例,而 gtest_discover_tests 把每个 TEST 注册成独立的
// ctest 条目并各起一个进程,用例之间不共享状态。因此每个用例必须自带完整的 init,
// 不得依赖其他用例的执行顺序或残留状态。

#include "gateway/core/config/Config.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace gateway;

namespace {

// 每个用例使用独立文件名,避免并行执行 ctest 时相互干扰
std::string confPath(const std::string& tag) {
    return "/tmp/gateway_config_test_" + tag + ".conf";
}

void writeConf(const std::string& path, const std::string& content) {
    std::ofstream o(path);
    o << content;
}

// 合法的基线配置,各用例在其上修改一两个键
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

// 用例结束时删除临时配置文件
struct ConfFile {
    std::string path;
    explicit ConfFile(const std::string& tag) : path(confPath(tag)) {}
    ~ConfFile() { std::remove(path.c_str()); }
    void write(const std::string& content) const { writeConf(path, content); }
};

}  // namespace

TEST(Config, InitParsesAllFields) {
    ConfFile f("parse");
    f.write(baseline());

    ASSERT_NO_THROW(ConfigManager::init(f.path));
    auto c = ConfigManager::current();
    ASSERT_NE(c, nullptr);

    EXPECT_EQ(c->log_level, 2);
    EXPECT_EQ(c->serial_path, "/tmp/ttyV9");
    EXPECT_EQ(c->serial_baud, 9600);
    EXPECT_EQ(c->mqtt_host, "192.168.1.5");
    EXPECT_EQ(c->mqtt_port, 1884);
    EXPECT_EQ(c->http_port, 8890);
}

// 未出现的键应保留 Config 结构体的默认值,而非置为 0 或空串
TEST(Config, MissingKeysKeepDefaults) {
    ConfFile f("defaults");
    f.write("log_level = 3\n");

    ASSERT_NO_THROW(ConfigManager::init(f.path));
    auto c = ConfigManager::current();

    EXPECT_EQ(c->log_level, 3);
    EXPECT_EQ(c->idle_timeout, 5) << "未配置的键应取默认值";
    EXPECT_EQ(c->report_n, 10);
    EXPECT_EQ(c->mqtt_keepalive, 60);
}

// 热加载的 diff 必须精确:多报会导致串口与连接被无谓重建,少报则修改不生效
TEST(Config, ReloadReportsOnlyWhatActuallyChanged) {
    ConfFile f("diff");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(
        "log_level = 0\n"
        "serial_path = /tmp/ttyVX\n"   // 变了
        "serial_baud = 9600\n"         // 没变
        "mqtt_host = 192.168.1.5\n"    // 没变
        "mqtt_port = 1884\n"
        "http_port = 8890\n");

    auto r = ConfigManager::reload();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.serial_changed);
    EXPECT_FALSE(r.mqtt_changed) << "host 与 keepalive 都没动,不该重连";
    EXPECT_FALSE(r.db_changed);

    EXPECT_EQ(ConfigManager::current()->log_level, 0) << "A 档改内存即生效";
    EXPECT_EQ(ConfigManager::current()->serial_path, "/tmp/ttyVX");
}

TEST(Config, ReloadDetectsMqttAndDbChanges) {
    ConfFile f("diff2");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("mqtt_host = 10.0.0.1\ndb_path = /tmp/other.db\n"));

    auto r = ConfigManager::reload();
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.mqtt_changed);
    EXPECT_TRUE(r.db_changed);
    EXPECT_FALSE(r.serial_changed) << "串口没动就不该重开";
}

// C 档(两个端口)运行期不可变,热加载必须整体忽略而非部分生效
TEST(Config, TierCChangesAreIgnored) {
    ConfFile f("tierc");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("mqtt_port = 9999\nhttp_port = 7777\n"));

    auto r = ConfigManager::reload();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(ConfigManager::current()->mqtt_port, 1884) << "应保持启动值而非 9999";
    EXPECT_EQ(ConfigManager::current()->http_port, 8890) << "监听端口不可热改";
}

// load-then-swap 的核心保证:解析失败时旧配置原封不动,进程继续运行。
TEST(Config, ParseFailureKeepsOldConfigIntact) {
    ConfFile f("badvalue");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const int old_baud = ConfigManager::current()->serial_baud;

    f.write("serial_baud = abc\n");   // 转换失败抛异常,整体不生效

    auto r = ConfigManager::reload();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(ConfigManager::current()->serial_baud, old_baud) << "旧值必须原封不动";
}

TEST(Config, ValidationRejectsOutOfRangePort) {
    ConfFile f("badport");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const auto before = ConfigManager::current()->serial_path;

    f.write(
        "serial_path = /tmp/ttyVX\n"
        "serial_baud = 9600\n"
        "mqtt_host = x\n"
        "http_port = 70000\n");   // 超 uint16 范围

    auto r = ConfigManager::reload();
    EXPECT_FALSE(r.ok) << "validate 该拦下越界端口";
    EXPECT_EQ(ConfigManager::current()->serial_path, before) << "失败后旧配置不动";
}

TEST(Config, ValidationRejectsEmptyRequiredPaths) {
    ConfFile f("emptypath");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("serial_path = \n"));

    auto r = ConfigManager::reload();
    EXPECT_FALSE(r.ok) << "空串口路径应被 validate 拦下";
}

// 配置文件不存在时 init 必须抛异常,使 main 致命退出,
// 而非带着默认配置连接到错误的 broker。
TEST(Config, InitThrowsOnMissingFile) {
    EXPECT_THROW(ConfigManager::init("/tmp/definitely_not_here_9f3a.conf"), std::exception);
}

// init 与 reload 必须用同一把尺子。
//
// 此前 init 只解析不校验,于是同一份坏配置在两条路径上结果相反:SIGHUP 热加载
// 会拒掉,开机启动却照单全收,然后在更下游以更难懂的方式失败。
TEST(Config, InitRejectsWhatReloadWouldReject) {
    ConfFile f("initvalidate");
    f.write(baseline("http_port = 70000\n"));   // 越界端口:reload 一直都会拒

    EXPECT_THROW(ConfigManager::init(f.path), std::exception)
        << "启动期必须和热加载一样拦下越界端口";
}

TEST(Config, InitRejectsNonPositiveIdleTimeout) {
    ConfFile f("initidle");
    f.write(baseline("idle_timeout = 0\n"));

    EXPECT_THROW(ConfigManager::init(f.path), std::exception);
}

// report_n 的上限此前只存在于 io 层的 clampReportN 里,配置侧只校验 > 0。
// 于是写 5000 能过校验,值被静默夹成 1000 —— 改写用户给的值而不告诉他,
// 比直接拒绝更糟。现在两个入口共用 kMaxReportN,配置侧负责拒绝。
TEST(Config, ValidationRejectsReportNAboveMax) {
    ConfFile f("reportnmax");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const int before = ConfigManager::current()->report_n;

    f.write(baseline("report_n = 5000\n"));

    auto r = ConfigManager::reload();
    EXPECT_FALSE(r.ok) << "超过 kMaxReportN 应被拦下,而不是留给 HTTP 层静默夹紧";
    EXPECT_EQ(ConfigManager::current()->report_n, before) << "失败后旧值不动";
}

TEST(Config, ValidationAcceptsReportNAtExactlyMax) {
    ConfFile f("reportnedge");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("report_n = " + std::to_string(kMaxReportN) + "\n"));

    auto r = ConfigManager::reload();
    EXPECT_TRUE(r.ok) << "上限本身是合法值,边界不能少算一个";
    EXPECT_EQ(ConfigManager::current()->report_n, kMaxReportN);
}
