// 最小闭环:MQTT 收数据 → 线程池 → 写 SQLite
//   on_message(网络线程) → 拷贝 → pool.submit → 工作线程 db.insert
#include "Database.h"
#include "ThreadPool.h"     // 你现成的 gateway::ThreadPool
#include <mosquitto.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>
#include <unistd.h>

// 全局指针,回调里要用(回调签名固定,只能靠 obj 或全局传)
static gateway::Database*   g_db   = nullptr;
static gateway::ThreadPool* g_pool = nullptr;

// 解析 payload 为 double(传感器读数)。失败返回 false。
static bool parseValue(const std::string& s, double& out) {
    try { out = std::stod(s); return true; }
    catch (...) { return false; }
}

void on_message(struct mosquitto*, void*, const struct mosquitto_message* msg) {
    // 1) 当场拷贝出 topic + payload —— msg 回调返回后失效(M10 学的坑)
    std::string topic(msg->topic ? msg->topic : "");
    std::string payload(static_cast<const char*>(msg->payload), msg->payloadlen);

    // 2) 提交给线程池,网络线程立即返回继续收(I/O 线程不阻塞铁律)
    g_pool->submit([topic, payload]{      // 按值捕获,跟 main.cpp 的 [f] 同理
        double v = 0;
        if (!parseValue(payload, v)) {
            fprintf(stderr, "[worker] bad payload on %s: %s\n",
                    topic.c_str(), payload.c_str());
            return;
        }
        long ts = static_cast<long>(time(nullptr));
        // device_id 用 topic 末段(gateway/sensor/temp → temp);最小闭环简化
        std::string dev = topic;
        size_t pos = topic.find_last_of('/');
        if (pos != std::string::npos) dev = topic.substr(pos + 1);

        g_db->insert(dev, v, ts);         // 工作线程写库(内部带锁)
    });
}

int main() {
    gateway::Database   db("pipeline.db");
    gateway::ThreadPool pool(4);
    g_db = &db; g_pool = &pool;

    mosquitto_lib_init();
    struct mosquitto* mosq = mosquitto_new("gateway-pipeline", true, nullptr);
    if (!mosq) { fprintf(stderr, "mosquitto_new failed\n"); return 1; }

    mosquitto_message_callback_set(mosq, on_message);

    int rc = mosquitto_connect(mosq, "localhost", 1883, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "connect failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq); mosquitto_lib_cleanup(); return 1;
    }
    rc = mosquitto_subscribe(mosq, nullptr, "gateway/sensor/#", 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "subscribe failed: %s\n", mosquitto_strerror(rc));
        mosquitto_disconnect(mosq); mosquitto_destroy(mosq);
        mosquitto_lib_cleanup(); return 1;
    }

    printf("[pipeline] listening gateway/sensor/#, writing to pipeline.db\n");
    mosquitto_loop_forever(mosq, -1, 1);

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}