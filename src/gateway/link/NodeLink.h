#pragma once

// STM32 串口链路抽象。
//
// NodeLink 把非阻塞 SerialPort、帧输出队列和增量解析器组合成单一链路对象，
// 上层只处理完整 Frame。它不拥有事件循环，也不加锁；fd 注册、读、写和 reopen 必须
// 由同一 Reactor 线程串行协调。接收采用边沿触发时，drainAndParse() 必须读到 EAGAIN。

#include "gateway/io/serial/SerialPort.h"
#include "gateway/protocol/FrameCodec.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gateway {

/** 拥有串口 fd 和接收解析状态的单线程链路对象。 */
class NodeLink {
public:
    using FrameHandler = FrameParser::OnFrameCallback; /**< CRC 正确帧的同步处理函数。 */
    using SendHandler = std::function<void(bool)>; /**< 整帧写完为 true，取消或写失败为 false。 */
    using SendPredicate = std::function<bool()>;   /**< 首字节写出前检查该帧是否仍需发送。 */

    /**
     * @brief 以非阻塞模式打开并配置 STM32 串口。
     * @param path 串口设备路径。
     * @param baud 人类可读的 bit/s 数值；不支持的值会告警并使用 115200。
     * @throws std::runtime_error 打开或 termios 配置失败。
     * @throws std::bad_alloc SerialPort 对象分配失败。
     */
    NodeLink(const std::string& path, int baud, bool defer_configuration = false);

    /** 热加载时在 Reactor 移除旧注册后应用 termios；准备线程只打开候选 fd。 */
    void activate();

    /** 复制源对象不会被读取；串口 fd 与解析状态均为独占资源。 */
    NodeLink(const NodeLink&)            = delete;
    /** 复制源对象不会被读取；禁止多个对象管理同一串口。 */
    NodeLink& operator=(const NodeLink&) = delete;

    /** @param cb CRC 正确后的新回调；空回调表示只解析和统计。 */
    void setFrameHandler(FrameHandler cb) { parser_.setOnFrame(std::move(cb)); }

    /**
     * @return 当前 SerialPort 拥有的 fd 借用值。
     * @warning reopen() 成功或对象析构后，先前取得的 fd 不再有效。
     */
    int fd() const noexcept { return port_->get(); }

    /**
     * @brief 循环读取当前 fd，直到 EAGAIN、EOF 或不可恢复错误。
     *
     * 每批字节按序交给 parser_；EINTR 会重试。帧回调在本函数栈内同步执行。
     * 临时 Frame 分配或业务回调产生的异常会向调用方传播。
     */
    void drainAndParse();

    /**
     * @brief 编码并排入一帧，由可写事件继续推进输出。
     * @param type 线上 TYPE 原始值。
     * @param payload 完整业务 payload；命令 seq 由上层提前放入。
     * @param on_complete 整帧写完、取消或写失败时同步调用；空回调表示无需通知。
     * @param can_send 首字节写出前的有效性检查；空回调表示始终发送。
     * @return 成功入队为 true；编码失败为 false，此时不调用 on_complete。
     *
     * 本函数不访问串口；调用方须在队列非空时监听 EPOLLOUT，并调用 flushOutput()。
     * 输出按整帧 FIFO 顺序推进，完成表示内核已接受全帧，不表示物理线路已发送完毕。
     * 队列容量由上层限制；内存分配或回调异常向调用方传播。
     */
    bool send(uint8_t type, const std::vector<uint8_t>& payload,
              SendHandler on_complete = {}, SendPredicate can_send = {});

    /**
     * @brief 非阻塞续写，直到输出队列为空、EAGAIN 或不可恢复错误。
     *
     * 短写保留帧和偏移；写完一帧立即通知上层。已写出前缀的帧即使失效也须补齐，
     * 避免下一帧与残留前缀交错。写错误会终止当前队列并逐帧回调 false。
     * 回调只能更新业务状态，不得重入 flushOutput() 或 reopen()。
     */
    void flushOutput();

    /** @return 是否仍有等待可写事件继续发送的帧。 */
    bool hasPendingOutput() const noexcept { return !output_.empty(); }

    /**
     * @brief 先准备候选端口，再更换串口设备或波特率并终止旧输出。
     * @param path 新设备路径。
     * @param baud 新 bit/s 数值；不支持的值退回 115200。
     * @throws std::runtime_error 新串口打开或配置失败；此时旧 port_ 保持不变。
     * @throws std::bad_alloc 新 SerialPort 对象分配失败；旧 port_ 同样保持不变。
     *
     * 调用方须先从事件循环注销旧 fd，随后无论成功或异常都登记 fd() 的实际值。
     * 成功后丢弃旧收发半帧，待发队列逐帧回调 false；接收回调和累计统计保留。
     * 打开或配置失败时，旧串口、收发队列和解析状态均不变。
     * 待发帧的失败回调异常向调用方传播，此时新串口已经生效。
     */
    void reopen(const std::string& path, int baud);

    /** @return parser_ 自构造以来的累计统计引用；生命周期与本对象相同。 */
    const edge_parser_stats_t& parserStats() const noexcept { return parser_.stats(); }

private:
    /** 一条尚未完整交给内核的输出帧，以及与其生命周期绑定的业务回调。 */
    struct PendingWrite {
        std::vector<uint8_t> frame; /**< 完整线上帧；短写期间保持字节内容不变。 */
        std::size_t offset = 0;     /**< 内核已接受的帧前缀长度。 */
        SendHandler on_complete;   /**< 从队列移除后执行，最多调用一次。 */
        SendPredicate can_send;    /**< 尚未写出字节时允许上层取消过期重试。 */
    };

    /** @param sent 队首帧是否已完整写出；先移除队首，再通知业务层。 */
    void completeFront(bool sent);
    /** @brief 清空当前输出队列，并对每条未完成帧通知失败。 */
    void discardOutput();

    speed_t baud_;
    std::unique_ptr<SerialPort> port_; /**< 独占当前串口 fd；替换或析构时关闭。 */
    FrameParser parser_;               /**< 跨 read 保存半帧状态和累计解析统计。 */
    std::deque<PendingWrite> output_;   /**< 保持帧序及短写偏移；仅所属 Reactor 线程访问。 */
};

}  // namespace gateway
