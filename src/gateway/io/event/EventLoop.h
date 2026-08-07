#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <unistd.h>
#include <string>
#include <ctime>

namespace gateway{

struct channel {
    int fd = -1;          // -1 表示未持有 fd，析构时据此决定是否 close
    uint32_t events = 0;
    std::function<void()> on_read;
    std::function<void()> on_write;
    std::string out_buf; 
    time_t last_active = 0;
    bool timeout_exempt = false;

    channel() = default;

    // fd 是独占资源，禁用拷贝以避免两个 channel 关闭同一个 fd
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // fd 的生命周期与本对象绑定
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