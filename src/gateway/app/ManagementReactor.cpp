#include "gateway/app/ManagementReactor.h"

#include "gateway/core/log/Logger.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace gateway {
namespace {
sigset_t managedSignals() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    return mask;
}

void checkSignalMask() {
    sigset_t current;
    const int error = pthread_sigmask(SIG_SETMASK, nullptr, &current);
    if (error != 0) throw std::system_error(error, std::generic_category(), "pthread_sigmask query");
    for (const int signal : {SIGHUP, SIGTERM, SIGINT}) {
        if (sigismember(&current, signal) != 1)
            throw std::logic_error("ManagementReactor requires blockManagedSignals before starting threads");
    }
}
} // namespace

void ManagementReactor::blockManagedSignals() {
    const auto mask = managedSignals();
    const int error = pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (error != 0) throw std::system_error(error, std::generic_category(), "pthread_sigmask block");
}

ManagementReactor::ManagementReactor(Callback reload, Callback shutdown, Callback failed)
    : reload_(std::move(reload)), shutdown_(std::move(shutdown)), failed_(std::move(failed)) {
    checkSignalMask();
    const auto mask = managedSignals();
    auto signals = std::make_shared<channel>();
    signals->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signals->fd == -1) throw std::system_error(errno, std::generic_category(), "management signalfd");
    signals->events = EPOLLIN;
    signals->on_read = [this] { onSignals(); };
    loop_.addChannel(signals);
    signal_fd_ = signals->fd;

    auto wake = std::make_shared<channel>();
    wake->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake->fd == -1) throw std::system_error(errno, std::generic_category(), "management eventfd");
    wake->events = EPOLLIN;
    wake->on_read = [this] { onStop(); };
    loop_.addChannel(wake);
    stop_fd_ = wake->fd;

    // 最后一步才启动线程；前面的任何失败都由 channel/EventLoop 的 RAII 回收 fd。
    thread_ = std::thread([this] { run(); });
}

ManagementReactor::~ManagementReactor() { stop(); }

void ManagementReactor::stop() {
    if (!thread_.joinable()) return;
    if (thread_.get_id() == std::this_thread::get_id())
        throw std::logic_error("ManagementReactor::stop must be called by its owner");
    stopping_.store(true);
    const uint64_t one = 1;
    ssize_t written;
    do {
        written = ::write(stop_fd_, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);
    // EAGAIN 表示已有唤醒待消费；fd 私有且直到 join 后才关闭。
    if (written != static_cast<ssize_t>(sizeof(one)) && !(written < 0 && errno == EAGAIN))
        throw std::system_error(written < 0 ? errno : EIO, std::generic_category(), "management stop write");
    thread_.join();
}

void ManagementReactor::onStop() {
    uint64_t count;
    ssize_t received;
    do {
        received = ::read(stop_fd_, &count, sizeof(count));
    } while (received < 0 && errno == EINTR);
    if (received != static_cast<ssize_t>(sizeof(count)) && !(received < 0 && errno == EAGAIN))
        throw std::system_error(received < 0 ? errno : EIO, std::generic_category(), "management stop read");
    loop_.quit();
}

void ManagementReactor::onSignals() {
    // 标准信号会合并；一轮有界读取，同时就绪的退出信号优先于重载，避免重载持续生产。
    signalfd_siginfo signals[8];
    ssize_t received;
    do {
        received = ::read(signal_fd_, signals, sizeof(signals));
    } while (received < 0 && errno == EINTR);
    if (received < 0 && errno == EAGAIN) return;
    if (received <= 0 || received % static_cast<ssize_t>(sizeof(signals[0])) != 0)
        throw std::system_error(received < 0 ? errno : EIO, std::generic_category(), "management signal read");
    if (stopping_.load()) return;

    const auto count = static_cast<std::size_t>(received) / sizeof(signals[0]);
    bool terminate = false;
    bool reload = false;
    for (std::size_t i = 0; i < count; ++i) {
        terminate = terminate || signals[i].ssi_signo == SIGTERM || signals[i].ssi_signo == SIGINT;
        reload = reload || signals[i].ssi_signo == SIGHUP;
    }
    if (terminate && !shutdown_sent_) {
        shutdown_sent_ = true;
        if (shutdown_) shutdown_();
    } else if (reload && !shutdown_sent_ && reload_) {
        reload_();
    }
}

void ManagementReactor::run() noexcept {
    pthread_setname_np(pthread_self(), "gateway-admin");
    try {
        loop_.loop();
        return;
    } catch (const std::exception& error) {
        try { LOG_ERROR("management reactor failed: %s", error.what()); } catch (...) {}
    } catch (...) {
        try { LOG_ERROR("%s", "management reactor failed with unknown exception"); } catch (...) {}
    }
    stopping_.store(true);
    loop_.quit();
    try {
        if (failed_) failed_();
    } catch (...) {
        try { LOG_ERROR("%s", "management failure notification threw"); } catch (...) {}
    }
}

} // namespace gateway
