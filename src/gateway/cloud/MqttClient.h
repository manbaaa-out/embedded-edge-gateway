#pragma once

/**
 * @file
 * libmosquitto 的生命周期与消息接口封装。
 *
 * 对象分为配置期和运行期：消息回调与订阅必须在 loopStart() 前登记，网络线程
 * 启动后只读这些配置。publish() 依赖 libmosquitto 自身的线程安全能力，可从业务
 * 线程调用；进程级初始化则由本类统一引用计数。
 */

#include <mosquitto.h>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gateway/core/log/Logger.h"

namespace gateway {

/** 一个 MQTT 连接及其后台网络线程的独占 RAII 对象。 */
class MqttClient {
public:
    /**
     * 收到应用消息时的回调。
     * @param topic broker 投递的完整主题。
     * @param payload 保留原始长度的消息体，可包含 NUL 字节。
     */
    using MessageHandler = std::function<void(const std::string& topic,
                                              const std::string& payload)>;

    /**
     * 创建 clean-session 客户端并同步建立 TCP/MQTT 连接，尚不启动网络线程。
     *
     * @param id MQTT Client ID；同一 broker 上应保持唯一。
     * @param host broker 主机名或地址。
     * @param port broker TCP 端口。
     * @param keepalive MQTT 空闲探活间隔，单位为秒，与业务遥测周期无关。
     * @throws std::runtime_error 客户端创建或首次连接失败。
     */
    MqttClient(const std::string& id,
               const std::string& host, int port, int keepalive = 60) {
        libInit();
        mosq_ = mosquitto_new(id.c_str(), true, this);
        if (!mosq_) {
            libCleanup();
            throw std::runtime_error("mosquitto_new failed");
        }
        mosquitto_message_callback_set(mosq_, &MqttClient::onMessageTrampoline);
        mosquitto_connect_callback_set(mosq_, &MqttClient::onConnectTrampoline);

        int rc = mosquitto_connect(mosq_, host.c_str(), port, keepalive);  // 首连结果码。
        if (rc != MOSQ_ERR_SUCCESS) {
            std::string msg = mosquitto_strerror(rc);  // 析构句柄前保存错误文本。
            mosquitto_destroy(mosq_);
            libCleanup();
            throw std::runtime_error("mosquitto_connect failed: " + msg);
        }
    }

    /** 先请求断开，再等待网络线程退出，确保销毁句柄前不再进入回调。 */
    ~MqttClient() noexcept {
        if (mosq_) {
            mosquitto_disconnect(mosq_);
            mosquitto_loop_stop(mosq_, false);
            mosquitto_destroy(mosq_);
        }
        libCleanup();
    }

    MqttClient(const MqttClient& /* other */) = delete;  ///< 不从其他实例复制连接句柄。
    MqttClient& operator=(const MqttClient& /* other */) = delete;  ///< 不接管副本来源。

    /**
     * 登记运行期消息回调。
     * @param h 网络线程收到消息后调用的处理器；可为空。
     * @throws std::logic_error 网络线程已经启动。
     */
    void setMessageHandler(MessageHandler h) {
        requireNotStarted("setMessageHandler");
        handler_ = std::move(h);
    }

    /**
     * 在配置期登记订阅，不立即发送 SUBSCRIBE。
     *
     * 首连和自动重连成功后由连接回调统一重放列表。当前客户端使用 clean session，
     * 因此同一 broker 的每次重连都需重放；热加载到新 broker 时，新客户端也会沿
     * 相同流程提交自己的列表。
     *
     * @param topic 订阅主题或通配过滤器。
     * @param qos 请求的订阅 QoS，默认 1。
     * @throws std::logic_error 网络线程已经启动。
     */
    void subscribe(const std::string& topic, int qos = 1) {
        requireNotStarted("subscribe");
        subs_.emplace_back(topic, qos);
    }

    /**
     * 将消息交给 libmosquitto 的发送队列。
     *
     * libmosquitto 负责运行期句柄同步，本层不再叠加互斥锁。该接口不等待 QoS
     * 握手，也不向调用方返回投递状态；立即失败只记录日志。
     *
     * @param topic 发布主题。
     * @param payload 原始消息体。
     * @param qos 发布 QoS，默认 0。
     * @param retain 是否要求 broker 保留最后一条消息。
     */
    void publish(const std::string& topic, const std::string& payload,
                 int qos = 0, bool retain = false) {
        int rc = mosquitto_publish(  // 仅表示消息是否成功交给客户端库。
            mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()),
            payload.data(), qos, retain);
        if (rc != MOSQ_ERR_SUCCESS)
            LOG_WARN("mqtt publish failed: %s", mosquitto_strerror(rc));
    }

    /**
     * 启动网络线程并永久结束配置阶段。
     * @return true 表示线程已启动；false 表示上下行均不可用。
     */
    bool loopStart() {
        started_ = true;
        const int rc = mosquitto_loop_start(mosq_);  // 后台循环启动结果。
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("mosquitto_loop_start failed: %s — 上行与下行均不可用",
                      mosquitto_strerror(rc));
            return false;
        }
        return true;
    }

private:
    /**
     * 强制配置期调用约束。
     * @param who 用于异常文本的成员函数名。
     * @throws std::logic_error 对象已进入运行期。
     */
    void requireNotStarted(const char* who) const {
        if (started_)
            throw std::logic_error(std::string(who) + "() called after loopStart()");
    }

    /**
     * CONNACK 回调：连接成功后重放全部订阅，拒绝连接时仅记录错误。
     * @param mosq 触发回调的 libmosquitto 句柄。
     * @param obj 创建句柄时登记的 MqttClient 指针。
     * @param rc broker 返回的连接结果码，0 表示成功。
     */
    static void onConnectTrampoline(struct mosquitto* mosq, void* obj, int rc) {
        auto* self = static_cast<MqttClient*>(obj);  // 恢复 C 回调对应的 C++ 对象。
        if (rc != 0) {
            LOG_ERROR("mqtt connect refused: %s", mosquitto_connack_string(rc));
            return;
        }
        for (const auto& s : self->subs_) {  // s=(主题过滤器, 请求 QoS)。
            int r = mosquitto_subscribe(  // 订阅请求进入客户端发送队列的结果。
                mosq, nullptr, s.first.c_str(), s.second);
            if (r != MOSQ_ERR_SUCCESS)
                LOG_ERROR("mqtt re-subscribe '%s' failed: %s",
                          s.first.c_str(), mosquitto_strerror(r));
        }
    }

    /**
     * 应用消息回调：复制库持有的数据后转交业务处理器。
     * @param mosq 触发回调的句柄；本实现无需使用，因为 obj 已定位所属对象。
     * @param obj 创建句柄时登记的 MqttClient 指针。
     * @param msg 仅在本次回调期间有效的消息对象。
     */
    static void onMessageTrampoline(struct mosquitto* /* mosq */, void* obj,
                                    const struct mosquitto_message* msg) {
        auto* self = static_cast<MqttClient*>(obj);  // 恢复回调所属对象。
        if (!self->handler_) return;
        std::string topic(msg->topic ? msg->topic : "");  // 拷贝库持有的主题。
        const size_t payload_len =
            (msg->payloadlen > 0) ? static_cast<size_t>(msg->payloadlen) : 0u;  // 防止负值扩展。
        std::string payload(  // 按显式长度拷贝，保留消息体中的 NUL。
            static_cast<const char*>(msg->payload), payload_len);
        self->handler_(topic, payload);
    }

    /** 增加进程级使用计数；首个实例调用 libmosquitto 全局初始化。 */
    static void libInit() {
        std::lock_guard<std::mutex> lock(lib_mtx_);  // 串行化非线程安全的全局初始化。
        if (lib_refcount_++ == 0) mosquitto_lib_init();
    }
    /** 释放一次使用权；最后一个实例执行全局清理。 */
    static void libCleanup() noexcept {
        std::lock_guard<std::mutex> lock(lib_mtx_);  // 与构造及其他析构互斥。
        if (--lib_refcount_ == 0) mosquitto_lib_cleanup();
    }

    inline static std::mutex lib_mtx_;           ///< 保护进程级引用计数与初始化/清理。
    inline static int        lib_refcount_ = 0;  ///< 当前存活的 MqttClient 数量。

    struct mosquitto* mosq_ = nullptr;  ///< 本对象独占的 libmosquitto 客户端句柄。
    bool started_ = false;              ///< 是否已经结束配置期并尝试启动网络线程。

    MessageHandler handler_;  ///< 配置期写入，网络线程启动后只读。
    std::vector<std::pair<std::string, int>> subs_;  ///< 待在每次连接后重放的订阅。
};

}  // namespace gateway
