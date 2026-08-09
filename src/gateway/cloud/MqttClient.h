#pragma once
#include <mosquitto.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace gateway {

class MqttClient {
public:
    using MessageHandler = std::function<void(const std::string& topic,
                                              const std::string& payload)>;

    MqttClient(const std::string& id,
               const std::string& host, int port, int keepalive = 60) {
        mosquitto_lib_init();
        mosq_ = mosquitto_new(id.c_str(), true, this);
        if (!mosq_) {
            mosquitto_lib_cleanup();
            throw std::runtime_error("mosquitto_new failed");
        }
        mosquitto_message_callback_set(mosq_, &MqttClient::onMessageTrampoline);
        // 每次连接建立(含 loop_start 线程的自动重连)都要重挂订阅,见 onConnectTrampoline
        mosquitto_connect_callback_set(mosq_, &MqttClient::onConnectTrampoline);

        int rc = mosquitto_connect(mosq_, host.c_str(), port, keepalive);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::string msg = mosquitto_strerror(rc);
            mosquitto_destroy(mosq_);
            mosquitto_lib_cleanup();
            throw std::runtime_error("mosquitto_connect failed: " + msg);
        }
    }

    ~MqttClient() noexcept {
        if (mosq_) {
            mosquitto_loop_stop(mosq_, true);
            mosquitto_disconnect(mosq_);
            mosquitto_destroy(mosq_);
        }
        mosquitto_lib_cleanup();
    }

    MqttClient(const MqttClient&)            = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    void setMessageHandler(MessageHandler h) { handler_ = std::move(h); }

    // 订阅并【记住】它。
    //
    // 记住这一步不是多余的:客户端以 clean_session = true 连接,broker 不保存会话,
    // 所以每次连接都必须重新提交订阅。loop_start 起的网络线程在断线后会自动重连 ——
    // 连接确实恢复了,发布也照常成功,唯独订阅没了,下行命令从此静默收不到,
    // 直到进程重启。这个故障没有任何报错,只表现为「命令发出去没反应」。
    // 因此订阅列表由本类持有,并在每次 CONNACK 后由 onConnectTrampoline 重放。
    void subscribe(const std::string& topic, int qos = 1) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            subs_.emplace_back(topic, qos);
        }
        int rc = mosquitto_subscribe(mosq_, nullptr, topic.c_str(), qos);
        if (rc != MOSQ_ERR_SUCCESS)
            throw std::runtime_error(std::string("subscribe failed: ")
                                     + mosquitto_strerror(rc));
    }

    void publish(const std::string& topic, const std::string& payload,
                 int qos = 0, bool retain = false) {
        std::lock_guard<std::mutex> lock(mtx_);
        int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(),
                                   static_cast<int>(payload.size()),
                                   payload.data(), qos, retain);
        if (rc != MOSQ_ERR_SUCCESS)
            fprintf(stderr, "[mqtt] publish failed: %s\n", mosquitto_strerror(rc));
    }

    void loopStart()   { mosquitto_loop_start(mosq_); }
    void loopForever() { mosquitto_loop_forever(mosq_, -1, 1); }

private:
    // 每次连接建立后由 mosquitto 网络线程回调:首连一次,之后每次自动重连各一次。
    // rc != 0 表示 broker 拒绝了连接(认证失败等),此时不必重放订阅。
    static void onConnectTrampoline(struct mosquitto* mosq, void* obj, int rc) {
        auto* self = static_cast<MqttClient*>(obj);
        if (rc != 0) {
            fprintf(stderr, "[mqtt] connect refused: %s\n", mosquitto_connack_string(rc));
            return;
        }
        std::vector<std::pair<std::string, int>> subs;
        {
            std::lock_guard<std::mutex> lock(self->mtx_);
            subs = self->subs_;   // 拷贝出来再提交,避免持锁调用 libmosquitto
        }
        for (const auto& s : subs) {
            int r = mosquitto_subscribe(mosq, nullptr, s.first.c_str(), s.second);
            if (r != MOSQ_ERR_SUCCESS)
                fprintf(stderr, "[mqtt] re-subscribe '%s' failed: %s\n",
                        s.first.c_str(), mosquitto_strerror(r));
        }
    }

    static void onMessageTrampoline(struct mosquitto*, void* obj,
                                    const struct mosquitto_message* msg) {
        auto* self = static_cast<MqttClient*>(obj);
        if (!self->handler_) return;
        std::string topic(msg->topic ? msg->topic : "");
        // libmosquitto 的 payloadlen 是 int。虽不预期为负,但类型允许,
        // 而负数转 size_t 会得到巨大值并导致 std::string 构造越界,故在此拦一道。
        const size_t payload_len =
            (msg->payloadlen > 0) ? static_cast<size_t>(msg->payloadlen) : 0u;
        std::string payload(static_cast<const char*>(msg->payload), payload_len);
        self->handler_(topic, payload);
    }

    struct mosquitto* mosq_ = nullptr;
    MessageHandler    handler_;

    // mtx_ 保护 publish 与 subs_。三条线程会调 publish(主线程回 ACK、worker 发上行、
    // mosquitto 线程回拒收原因),subs_ 则由主线程写、mosquitto 线程在重连时读。
    std::mutex mtx_;
    std::vector<std::pair<std::string, int>> subs_;   // 已订阅的 topic + qos,供重连重放
};

} // namespace gateway
