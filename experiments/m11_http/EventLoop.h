#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <unistd.h>
#include <string>
#include <ctime>

namespace m7{

struct channel {
    int fd = -1;          // 默认 -1，析构时据此判断要不要关
    uint32_t events = 0;
    std::function<void()> on_read;
    std::function<void()> on_write;
    std::string out_buf; 
    time_t last_active = 0;
    bool timeout_exempt = false;

    channel() = default;

    // 持有 fd 这种独占资源，禁掉拷贝，避免两个 channel 关同一个 fd（double close）
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // RAII：fd 跟对象同生共死
    ~channel() { if (fd != -1) ::close(fd); }

    void handleRead() { if (on_read) on_read(); }
    void handleWrite() { if (on_write) on_write(); }
};

class EventLoop {
    public:
    explicit EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    EventLoop(EventLoop&&) noexcept;
    EventLoop& operator=(EventLoop&&) noexcept;

    void loop();

    void addChannel(std::shared_ptr<channel>);
    void removeChannel(int);
    void modifyChannel(channel*);

    void forEachChannel(std::function<void(channel*)>);
    
    ~EventLoop() noexcept;

    private:
    int epoll_fd_ = -1;
    std::map<int, std::shared_ptr<channel>> channels_;
    std::vector<std::shared_ptr<channel>> dying_;

};

}