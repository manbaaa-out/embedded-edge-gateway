#pragma once
#include <mosquitto.h>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gateway/core/log/Logger.h"

namespace gateway {

// libmosquitto 的 C++ 薄封装。
//
// ---- 线程契约 ----
// 本类分两个阶段,阶段边界是 loopStart():
//
//   配置期(构造 → loopStart 之前):只允许构造线程访问。setMessageHandler /
//   subscribe 在此期间写 handler_ 与 subs_,此时尚无网络线程,无并发。
//
//   运行期(loopStart 之后):handler_ 与 subs_ 转为只读,由 mosquitto 网络线程
//   在收到消息 / 每次 CONNACK 时读取。std::thread 的创建对新线程构成 happens-before,
//   因此读侧不需要任何同步 —— 前提是运行期不再有人写。这个前提由 started_ 检查兜住,
//   越界调用直接抛,把契约从注释变成强制。
//
//   publish() 是唯一允许跨线程并发调用的成员,且本类不为它加锁,理由见函数上方。
//
// 于是本类只剩一把锁:静态的 lib_mtx_,保护 libmosquitto 的进程级 init/cleanup。
class MqttClient {
public:
    using MessageHandler = std::function<void(const std::string& topic,
                                              const std::string& payload)>;

    MqttClient(const std::string& id,
               const std::string& host, int port, int keepalive = 60) {
        libInit();
        mosq_ = mosquitto_new(id.c_str(), true, this);
        if (!mosq_) {
            libCleanup();
            throw std::runtime_error("mosquitto_new failed");
        }
        mosquitto_message_callback_set(mosq_, &MqttClient::onMessageTrampoline);
        // 每次连接建立(含 loop_start 线程的自动重连)都要重挂订阅,见 onConnectTrampoline
        mosquitto_connect_callback_set(mosq_, &MqttClient::onConnectTrampoline);

        int rc = mosquitto_connect(mosq_, host.c_str(), port, keepalive);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::string msg = mosquitto_strerror(rc);
            mosquitto_destroy(mosq_);
            libCleanup();
            throw std::runtime_error("mosquitto_connect failed: " + msg);
        }
    }

    // 停网络线程的顺序不能反:先 disconnect 把状态置为断开,网络线程据此自行退出,
    // 再 loop_stop(false) 等它 join 完。mosquitto_loop_stop 的文档写明,force=false
    // 要求先调过 disconnect。
    //
    // 不用 force=true:那是 pthread_cancel。网络线程随时可能停在 libmosquitto 的
    // 内部锁里,被砍掉后那把锁再也不会解开,紧接着的 mosquitto_destroy 会去
    // pthread_mutex_destroy 一把仍处于锁定状态的锁。
    //
    // join 完成后,onMessageTrampoline / onConnectTrampoline 确定不再执行 —— 下行
    // handler 因此可以安全地捕获裸指针,见 GatewayApp::startMqtt。
    ~MqttClient() noexcept {
        if (mosq_) {
            mosquitto_disconnect(mosq_);
            mosquitto_loop_stop(mosq_, false);   // 未起过网络线程时返回 INVAL,无害
            mosquitto_destroy(mosq_);
        }
        libCleanup();
    }

    MqttClient(const MqttClient&)            = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    // 配置期接口,见类头「线程契约」。
    void setMessageHandler(MessageHandler h) {
        requireNotStarted("setMessageHandler");
        handler_ = std::move(h);
    }

    // 订阅并【记住】它。配置期接口。
    //
    // 记住这一步不是多余的:客户端以 clean_session = true 连接,broker 不保存会话,
    // 所以每次连接都必须重新提交订阅。loop_start 起的网络线程在断线后会自动重连 ——
    // 连接确实恢复了,发布也照常成功,唯独订阅没了,下行命令从此静默收不到,
    // 直到进程重启。这个故障没有任何报错,只表现为「命令发出去没反应」。
    // 因此订阅列表由本类持有,并在每次 CONNACK 后由 onConnectTrampoline 重放。
    void subscribe(const std::string& topic, int qos = 1) {
        requireNotStarted("subscribe");
        subs_.emplace_back(topic, qos);
        int rc = mosquitto_subscribe(mosq_, nullptr, topic.c_str(), qos);
        if (rc != MOSQ_ERR_SUCCESS)
            throw std::runtime_error(std::string("subscribe failed: ")
                                     + mosquitto_strerror(rc));
    }

    // 唯一允许跨线程并发调用的成员:主线程(命令应答与超时判死)、线程池 worker
    // (遥测上行)、mosquitto 网络线程(下行命令的拒收原因)都会调它。
    //
    // 这里不加本类的锁,是两条理由叠起来的结论:
    //
    //   一、加了也是白加。libmosquitto 2.0 自身线程安全(见 mosquitto.h 的
    //       "Topic: Threads",唯一例外是 lib_init),而 loopStart() 用的
    //       mosquitto_loop_start 会自动置上 threaded 标志 —— 该标志正是库切换到
    //       多线程行为的开关(见 mosquitto_threaded_set 文档)。句柄的内部状态
    //       人家已经护住了。
    //
    //   二、加了反而危险。onConnectTrampoline 是 libmosquitto 在自己的内部锁下
    //       回调进来的,若它再去抢本类的锁,而 publish 这边持本类的锁去调库,
    //       两条路径的加锁顺序就相反,构成 ABBA 死锁的形状。是否真的成环取决于
    //       库内部用了哪几把锁 —— 那是本项目看不见也控制不了的实现细节,
    //       不该让正确性依赖它。
    //
    // 附带的好处:失败分支的日志不再落在临界区里。broker 断开时每条 publish
    // 都会失败,若带锁,六条线程会一起排队记这条日志,恰好在最需要吞吐的时刻
    // 自我放大。改走 LOG_WARN 后单次代价已从"无缓冲 stderr 写"降到"取一次
    // AsyncLogger 的锁 + 一次 memcpy",但把它留在锁外仍然更好。
    void publish(const std::string& topic, const std::string& payload,
                 int qos = 0, bool retain = false) {
        int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(),
                                   static_cast<int>(payload.size()),
                                   payload.data(), qos, retain);
        if (rc != MOSQ_ERR_SUCCESS)
            LOG_WARN("mqtt publish failed: %s", mosquitto_strerror(rc));
    }

    // 起网络线程,并把本对象推进运行期。此后配置期接口一律拒绝。
    void loopStart() {
        started_ = true;
        mosquitto_loop_start(mosq_);
    }

private:
    void requireNotStarted(const char* who) const {
        // 运行期再改 handler_ / subs_ 就是与网络线程的读并发,而读侧按契约是不加锁的。
        // 这不是运行时故障而是用法错误,故抛 logic_error 而非 runtime_error。
        if (started_)
            throw std::logic_error(std::string(who) + "() called after loopStart()");
    }

    // 每次连接建立后由 mosquitto 网络线程回调:首连一次,之后每次自动重连各一次。
    // rc != 0 表示 broker 拒绝了连接(认证失败等),此时不必重放订阅。
    //
    // 直接遍历 subs_ 而不加锁:它在 loopStart() 之前写完,此后只读(见类头「线程契约」)。
    static void onConnectTrampoline(struct mosquitto* mosq, void* obj, int rc) {
        auto* self = static_cast<MqttClient*>(obj);
        if (rc != 0) {
            LOG_ERROR("mqtt connect refused: %s", mosquitto_connack_string(rc));
            return;
        }
        for (const auto& s : self->subs_) {
            int r = mosquitto_subscribe(mosq, nullptr, s.first.c_str(), s.second);
            if (r != MOSQ_ERR_SUCCESS)
                LOG_ERROR("mqtt re-subscribe '%s' failed: %s",
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

    // ---- libmosquitto 的进程级 init/cleanup ----
    //
    // 本类唯一的锁在这儿。mosquitto_lib_init 的文档明说它不是线程安全的,而本类的
    // 析构未必发生在主线程:在飞的线程池 task 按值持有 client 的 shared_ptr 快照,
    // 热加载换 broker 时最后一个释放者可能就是某个 worker。于是「主线程构造新 client
    // (init)」与「worker 析构旧 client(cleanup)」会同时进出库的全局初始化。
    //
    // 计数器让 cleanup 只在最后一个实例消失时真正执行 —— 否则旧 client 的析构会
    // 把新 client 脚下的库全局状态拆掉。
    static void libInit() {
        std::lock_guard<std::mutex> lock(lib_mtx_);
        if (lib_refcount_++ == 0) mosquitto_lib_init();
    }
    static void libCleanup() noexcept {
        std::lock_guard<std::mutex> lock(lib_mtx_);
        if (--lib_refcount_ == 0) mosquitto_lib_cleanup();
    }

    inline static std::mutex lib_mtx_;
    inline static int        lib_refcount_ = 0;

    struct mosquitto* mosq_    = nullptr;
    bool              started_ = false;   // 仅构造线程读写,见 requireNotStarted

    // 以下两项:配置期由构造线程写,运行期由 mosquitto 网络线程只读。
    // 建线程提供 happens-before,故无锁。
    MessageHandler                           handler_;
    std::vector<std::pair<std::string, int>> subs_;   // 已订阅的 topic + qos,供重连重放
};

}  // namespace gateway
