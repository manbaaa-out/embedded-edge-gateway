#pragma once

// 配置快照及其热加载接口。
//
// 设计采用“构建新对象后原子换指针”，读线程始终持有一份不可变且内部一致的快照；
// 解析或校验失败时不替换旧快照。ConfigManager 发布成功后只返回资源差异，串口、MQTT
// 和数据库的实际重建由应用层执行；若重建失败，已发布快照不会回滚，配置值可能暂时
// 与外部资源的实际状态不一致，应用层必须记录这种降级。

#include <string>
#include <memory>

namespace gateway {

/** report_n 配置和 HTTP n 参数共用的单次数据点上限。 */
inline constexpr int kMaxReportN = 1000;

/** 一份完整、不可变发布的网关配置值对象。 */
struct Config {
    // 下列字段由使用者读取最新快照即可生效，不绑定长期资源。
    int log_level      = 1;  /**< Logger 最低输出级别；应与 LogLevel 的 0..3 整数值一致。 */
    int idle_timeout   = 5;  /**< HTTP 空闲连接回收阈值，单位秒，必须大于 0。 */
    int report_n       = 10; /**< HTTP 未指定 n 时的默认数据点数，范围 [1, kMaxReportN]。 */

    // 下列字段固化在外部资源中，变更后必须重建相应对象。
    std::string serial_path = "/tmp/ttyV0"; /**< STM32 串口设备路径。 */
    int         serial_baud = 115200;        /**< 串口波特率，单位 bit/s；校验只要求正数。 */
    std::string mqtt_host   = "localhost";  /**< MQTT Broker 主机名或地址。 */
    std::string db_path     = "/tmp/gateway.db"; /**< SQLite 数据库文件路径。 */
    int mqtt_keepalive = 60; /**< MQTT CONNECT keep-alive，单位秒；0 表示禁用协议保活。 */

    // 监听/连接端口在运行期间固定；reload 会恢复启动值并提示需要重启。
    int mqtt_port = 1883; /**< MQTT Broker TCP 端口，范围 [1, 65535]。 */
    int http_port = 8888; /**< 本地 HTTP 服务监听端口，范围 [1, 65535]。 */
};

/**
 * 进程级配置管理器。init() 必须先于 current()/reload() 且只调用一次；init()
 * 和 reload() 必须由调用方串行化。current() 可由任意线程并发读取。reload()
 * 的路径和不可热加载字段都以那次 init() 为基准。
 */
class ConfigManager {
public:
    /**
     * @brief 从文件加载、校验并发布首份配置。
     * @param path 配置文件路径，同时保存为后续 reload() 的固定来源。
     * @throws std::runtime_error 文件不可读、已知值无法解析或任一字段校验失败。
     */
    static void init(const std::string& path);

    /**
     * @brief 原子读取当前配置快照。
     * @return 共享只读快照；init() 前调用会得到空指针。
     */
    static std::shared_ptr<const Config> current();

    /**
     * reload() 的整体结果和新旧快照之间的资源字段差异。这些标志不跟踪
     * 应用层资源是否成功重建；快照发布后重建失败，再加载同样文件时标志会为 false。
     */
    struct ReloadResult {
        bool ok = false;             /**< 新文件已成功解析、校验并发布。 */
        bool serial_changed = false; /**< serial_path 或 serial_baud 发生变化。 */
        bool mqtt_changed = false;   /**< mqtt_host 或 mqtt_keepalive 发生变化。 */
        bool db_changed = false;     /**< db_path 发生变化。 */
    };

    /**
     * @brief 从 init() 保存的路径重新加载配置。
     * @return 结果与资源差异；任何异常或校验失败都返回 ok=false 并保留旧快照。
     *
     * mqtt_port/http_port 会先参与整体校验；校验通过后，即使文件中
     * 数值变化，发布前也会恢复为启动值。
     */
    static ReloadResult reload();

private:
    /**
     * @param path 待读取的配置文件路径。
     * @return 以默认值为基线解析出的配置。
     * @throws std::runtime_error 文件不可读或已知整数格式错误。
     */
    static Config parseFile(const std::string& path);
    /**
     * @param c 待验证的完整配置值。
     * @return 所有范围、端口和非空约束都满足时为 true；失败项写入日志。
     */
    static bool   validate(const Config& c);

    static std::string path_;                   /**< init() 记录的配置文件路径。 */
    static std::shared_ptr<const Config> current_; /**< 原子发布给读线程的当前快照。 */
    static std::shared_ptr<const Config> startup_; /**< 不可热加载字段的启动值锚点。 */
};

} // namespace gateway
