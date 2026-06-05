#pragma once
#include <mosquitto.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <mutex>

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

    void subscribe(const std::string& topic, int qos = 1) {
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
    static void onMessageTrampoline(struct mosquitto*, void* obj,
                                    const struct mosquitto_message* msg) {
        auto* self = static_cast<MqttClient*>(obj);
        if (!self->handler_) return;
        std::string topic(msg->topic ? msg->topic : "");
        std::string payload(static_cast<const char*>(msg->payload), msg->payloadlen);
        self->handler_(topic, payload);
    }

    struct mosquitto* mosq_ = nullptr;
    MessageHandler    handler_;
    std::mutex mtx_;
};

} // namespace gateway
