// 配置解析与热加载测试
//
// 由原 src/config/test/config_test.cpp 移植而来(那份从未接进任何构建)。
//
// 【为什么每个用例都自己 init】ConfigManager 是进程级单例,但 gtest_discover_tests
// 会把每个 TEST 注册成独立的 ctest 条目、各起一个进程 —— 用例之间不共享状态。
// 所以每个用例必须自带完整的 init,不能依赖前一个用例留下的残留。
// 原来那份手写测试是「一个 main 顺序跑五段」,搬过来时若照抄顺序依赖,
// 在 ctest 下会直接段错误(current() 返回空)。

#include "gateway/core/config/Config.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace gateway;

namespace {

// 每个用例用独立文件名,避免并行跑 ctest 时互相踩
std::string confPath(const std::string& tag) {
    return "/tmp/gateway_config_test_" + tag + ".conf";
}

void writeConf(const std::string& path, const std::string& content) {
    std::ofstream o(path);
    o << content;
}

// 一份合法的基线配置,各用例在其上改一两个键
std::string baseline(const std::string& overrides = "") {
    return "# 注释行应被跳过\n"
           "log_level = 2\n"
           "serial_path = /tmp/ttyV9\n"
           "serial_baud = 9600\n"
           "mqtt_host = 192.168.1.5\n"
           "mqtt_port = 1884\n"
           "worker_count = 8\n" +
           overrides;
}

// RAII:用例结束删掉临时配置文件
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
    EXPECT_EQ(c->worker_count, 8);
}

// 未写的键应保留 Config 结构体里的默认值,而不是变成 0 / 空串
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

// 热加载的 diff 必须精确:报多了会把没必要的串口/连接白白重开,报少了则改了不生效
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
        "worker_count = 8\n");

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

// C 档(端口 / worker_count)运行期不可变:热加载必须忽略,而不是半生效
TEST(Config, TierCChangesAreIgnored) {
    ConfFile f("tierc");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));

    f.write(baseline("mqtt_port = 9999\nhttp_port = 7777\nworker_count = 2\n"));

    auto r = ConfigManager::reload();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(ConfigManager::current()->mqtt_port, 1884) << "应保持启动值而非 9999";
    EXPECT_EQ(ConfigManager::current()->worker_count, 8) << "线程池大小不可热改";
}

// load-then-swap 的核心保证:解析失败时旧配置原封不动,进程照常跑。
// 这是「配置写错了不该把在跑的网关搞挂」的底线。
TEST(Config, ParseFailureKeepsOldConfigIntact) {
    ConfFile f("badvalue");
    f.write(baseline());
    ASSERT_NO_THROW(ConfigManager::init(f.path));
    const int old_baud = ConfigManager::current()->serial_baud;

    f.write("serial_baud = abc\n");   // 转换失败 → 抛异常 → 整体不生效

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

// 配置文件根本不存在时,init 必须抛 —— 让 main 能致命退出,
// 而不是揣着一份默认配置连到错误的 broker 上。
TEST(Config, InitThrowsOnMissingFile) {
    EXPECT_THROW(ConfigManager::init("/tmp/definitely_not_here_9f3a.conf"), std::exception);
}
