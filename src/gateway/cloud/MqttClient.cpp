/** @file libmosquitto 生命周期与消息接口封装的实现。 */

#include "gateway/cloud/MqttClient.h"

#include "gateway/core/log/Logger.h"

#include <mosquitto.h>

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace gateway {

std::mutex MqttClient::lib_mtx_;
int MqttClient::lib_refcount_ = 0;

MqttClient::MqttClient(const std::string& id,
                       const std::string& host, int port, int keepalive) {
    libInit();
    mosq_ = mosquitto_new(id.c_str(), true, this);
    if (!mosq_) {
        libCleanup();
        throw std::runtime_error("mosquitto_new failed");
    }
    mosquitto_message_callback_set(mosq_, &MqttClient::onMessageTrampoline);
    mosquitto_connect_callback_set(mosq_, &MqttClient::onConnectTrampoline);

    const int rc = mosquitto_connect(mosq_, host.c_str(), port, keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        const std::string msg = mosquitto_strerror(rc);  // 析构句柄前保存错误文本。
        mosquitto_destroy(mosq_);
        libCleanup();
        throw std::runtime_error("mosquitto_connect failed: " + msg);
    }
}

MqttClient::~MqttClient() noexcept {
    if (mosq_) {
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, false);
        mosquitto_destroy(mosq_);
    }
    libCleanup();
}

void MqttClient::setMessageHandler(MessageHandler h) {
    requireNotStarted("setMessageHandler");
    handler_ = std::move(h);
}

void MqttClient::subscribe(const std::string& topic, int qos) {
    requireNotStarted("subscribe");
    subs_.emplace_back(topic, qos);
}

void MqttClient::publish(const std::string& topic, const std::string& payload,
                         int qos, bool retain) {
    const int rc = mosquitto_publish(  // 仅表示消息是否成功交给客户端库。
        mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()),
        payload.data(), qos, retain);
    if (rc != MOSQ_ERR_SUCCESS)
        LOG_WARN("mqtt publish failed: %s", mosquitto_strerror(rc));
}

bool MqttClient::loopStart() {
    started_ = true;
    const int rc = mosquitto_loop_start(mosq_);  // 后台循环启动结果。
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_loop_start failed: %s — 上行与下行均不可用",
                  mosquitto_strerror(rc));
        return false;
    }
    return true;
}

void MqttClient::requireNotStarted(const char* who) const {
    if (started_)
        throw std::logic_error(std::string(who) + "() called after loopStart()");
}

void MqttClient::onConnectTrampoline(struct mosquitto* mosq, void* obj, int rc) {
    auto* self = static_cast<MqttClient*>(obj);  // 恢复 C 回调对应的 C++ 对象。
    if (rc != 0) {
        LOG_ERROR("mqtt connect refused: %s", mosquitto_connack_string(rc));
        return;
    }
    for (const auto& s : self->subs_) {  // s=(主题过滤器, 请求 QoS)。
        const int r = mosquitto_subscribe(  // 订阅请求进入客户端发送队列的结果。
            mosq, nullptr, s.first.c_str(), s.second);
        if (r != MOSQ_ERR_SUCCESS)
            LOG_ERROR("mqtt re-subscribe '%s' failed: %s",
                      s.first.c_str(), mosquitto_strerror(r));
    }
}

void MqttClient::onMessageTrampoline(struct mosquitto* /* mosq */, void* obj,
                                     const struct mosquitto_message* msg) {
    auto* self = static_cast<MqttClient*>(obj);  // 恢复回调所属对象。
    if (!self->handler_) return;
    const std::string topic(msg->topic ? msg->topic : "");  // 拷贝库持有的主题。
    const std::size_t payload_len =
        (msg->payloadlen > 0) ? static_cast<std::size_t>(msg->payloadlen) : 0u;
    const std::string payload(  // 按显式长度拷贝，保留消息体中的 NUL。
        static_cast<const char*>(msg->payload), payload_len);
    self->handler_(topic, payload);
}

void MqttClient::libInit() {
    std::lock_guard<std::mutex> lock(lib_mtx_);  // 串行化非线程安全的全局初始化。
    if (lib_refcount_++ == 0) mosquitto_lib_init();
}

void MqttClient::libCleanup() noexcept {
    std::lock_guard<std::mutex> lock(lib_mtx_);  // 与构造及其他析构互斥。
    if (--lib_refcount_ == 0) mosquitto_lib_cleanup();
}

}  // namespace gateway
