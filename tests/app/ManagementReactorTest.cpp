#include "gateway/app/ManagementReactor.h"
#include "gateway/core/concurrent/TaskExecutor.h"

#include <gtest/gtest.h>

#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using gateway::ManagementReactor;
using gateway::TaskExecutor;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 每个场景先 fork，再在子进程屏蔽信号和创建线程；不会向 gtest 进程或其他用例发信号。
void isolated(const std::function<void()>& scenario) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        alarm(10);
        try {
            scenario();
            _exit(EXIT_SUCCESS);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "management child: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "management child: unknown failure\n");
        }
        _exit(EXIT_FAILURE);
    }
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            ASSERT_TRUE(WIFEXITED(status)) << "child wait status: " << status;
            EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS);
            return;
        }
        if (result < 0 && errno != EINTR) {
            ADD_FAILURE() << "waitpid failed: " << errno;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    // 即使组件卡在 epoll/join，测试也不会永久挂住，且只回收本例创建的子进程。
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    ADD_FAILURE() << "management child exceeded bounded timeout";
}

void sendSignal(int signal) {
    require(kill(getpid(), signal) == 0, "could not signal isolated child");
}

template <typename T>
void await(std::future<T>& result, const char* message) {
    require(result.wait_for(2s) == std::future_status::ready, message);
}

struct ReleaseGate {
    std::promise<void> promise;
    std::shared_future<void> future = promise.get_future().share();
    bool released = false;
    void release() {
        if (!released) {
            released = true;
            promise.set_value();
        }
    }
    ~ReleaseGate() { release(); }
};

TEST(ManagementReactor, IdleStopWakesEpollAndDoesNotRequestApplicationShutdown) {
    isolated([] {
        ManagementReactor::blockManagedSignals();
        std::atomic<unsigned> callbacks{0};
        ManagementReactor reactor([&] { ++callbacks; }, [&] { ++callbacks; }, [&] { ++callbacks; });
        const auto begin = std::chrono::steady_clock::now();
        reactor.stop();
        reactor.stop();
        require(std::chrono::steady_clock::now() - begin < 2s, "idle stop did not wake epoll");
        require(callbacks.load() == 0, "programmatic stop emitted an application event");
    });
}

TEST(ManagementReactor, ReloadPostsToDifferentWorkerAndSlowWorkDoesNotBlockTermination) {
    isolated([] {
        ManagementReactor::blockManagedSignals();
        std::promise<std::thread::id> reload_owner, worker_owner, shutdown_owner;
        auto reload_result = reload_owner.get_future();
        auto worker_result = worker_owner.get_future();
        auto shutdown_result = shutdown_owner.get_future();
        std::atomic<bool> work_completed{false};
        std::atomic<unsigned> failures{0};
        TaskExecutor worker("test-manage");
        ReleaseGate gate; // 失败展开时先放行任务，再析构 worker，避免测试自身死锁。
        ManagementReactor reactor([&] {
            char name[16]{};
            require(pthread_getname_np(pthread_self(), name, sizeof(name)) == 0, "thread name query failed");
            require(std::string(name) == "gateway-admin", "management thread name differs");
            reload_owner.set_value(std::this_thread::get_id());
            require(worker.post([&] {
                worker_owner.set_value(std::this_thread::get_id());
                gate.future.wait();
                work_completed.store(true);
            }), "management worker rejected test task");
        }, [&] { shutdown_owner.set_value(std::this_thread::get_id()); }, [&] { ++failures; });

        sendSignal(SIGHUP);
        await(reload_result, "reload signal was not dispatched");
        await(worker_result, "management work was not submitted");
        const auto management_id = reload_result.get();
        require(management_id != std::this_thread::get_id(), "reload ran on the owner thread");
        require(management_id != worker_result.get(), "blocking work ran on the management reactor");
        sendSignal(SIGTERM);
        await(shutdown_result, "slow work prevented TERM from being dispatched");
        require(shutdown_result.get() == management_id, "signal callbacks have different owners");
        require(!work_completed.load(), "slow work did not overlap TERM handling");
        gate.release();
        worker.stop();
        reactor.stop();
        require(failures.load() == 0, "management reactor unexpectedly failed");
    });
}

TEST(ManagementReactor, TerminationIsDeliveredOnceAndClosesReloadAdmission) {
    isolated([] {
        ManagementReactor::blockManagedSignals();
        std::atomic<unsigned> reloads{0}, shutdowns{0}, failures{0};
        std::promise<void> first_reload, first_shutdown;
        auto reloaded = first_reload.get_future();
        auto stopped = first_shutdown.get_future();
        ManagementReactor reactor([&] {
            if (++reloads == 1) first_reload.set_value();
        }, [&] {
            if (++shutdowns == 1) first_shutdown.set_value();
        }, [&] { ++failures; });
        sendSignal(SIGHUP);
        await(reloaded, "initial reload was not dispatched");
        sendSignal(SIGTERM);
        await(stopped, "initial termination was not dispatched");
        sendSignal(SIGHUP);
        sendSignal(SIGINT);
        sendSignal(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        for (;;) {
            sigset_t pending;
            require(sigpending(&pending) == 0, "could not query pending signals");
            if (sigismember(&pending, SIGHUP) == 0 && sigismember(&pending, SIGINT) == 0 &&
                sigismember(&pending, SIGTERM) == 0) break;
            require(std::chrono::steady_clock::now() < deadline, "later signals were not consumed");
            std::this_thread::sleep_for(1ms);
        }
        reactor.stop();
        require(reloads.load() == 1, "reload remained open after termination");
        require(shutdowns.load() == 1, "termination was delivered more than once");
        require(failures.load() == 0, "repeat signals caused a failure");
    });
}

TEST(ManagementReactor, CallbackFailureNotifiesOwnerAndThreadCanBeJoined) {
    isolated([] {
        ManagementReactor::blockManagedSignals();
        std::promise<std::thread::id> failed_owner;
        auto failure = failed_owner.get_future();
        std::atomic<unsigned> failures{0}, shutdowns{0};
        ManagementReactor reactor([] {
            throw std::runtime_error("injected management callback failure");
        }, [&] { ++shutdowns; }, [&] {
            if (++failures == 1) failed_owner.set_value(std::this_thread::get_id());
        });
        sendSignal(SIGHUP);
        await(failure, "runtime failure was not reported");
        require(failure.get() != std::this_thread::get_id(), "failure callback ran on owner thread");
        reactor.stop();
        reactor.stop();
        require(failures.load() == 1 && shutdowns.load() == 0, "incorrect failure/shutdown notification");
    });
}

TEST(ManagementReactor, MissingSignalMaskFailsSynchronously) {
    isolated([] {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGHUP);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        require(pthread_sigmask(SIG_UNBLOCK, &mask, nullptr) == 0, "could not reset child mask");
        bool rejected = false;
        try { ManagementReactor reactor([] {}, [] {}, [] {}); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected, "unblocked management signals were silently accepted");
    });
}

TEST(ManagementReactor, PartialInitializationFailureClosesAlreadyRegisteredDescriptors) {
    isolated([] {
        ManagementReactor::blockManagedSignals();
        rlimit previous;
        require(getrlimit(RLIMIT_NOFILE, &previous) == 0, "getrlimit failed");
        require(previous.rlim_max >= 64, "child descriptor hard limit is too small for test");
        const rlimit limited{64, previous.rlim_max};
        require(setrlimit(RLIMIT_NOFILE, &limited) == 0, "setrlimit failed");
        std::vector<int> fillers;
        for (;;) {
            const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd < 0) break;
            fillers.push_back(fd);
        }
        require(errno == EMFILE && fillers.size() >= 2, "could not fill descriptor table");
        for (int i = 0; i < 2; ++i) {
            close(fillers.back());
            fillers.pop_back();
        }
        // 两个空位分别用于 epoll 和 signalfd，eventfd 必须在启动线程前失败。
        std::string message;
        try { ManagementReactor reactor([] {}, [] {}, [] {}); }
        catch (const std::system_error& error) { message = error.what(); }
        require(message.find("management eventfd") != std::string::npos,
                "partial fd initialization did not fail synchronously at eventfd");
        const int reclaimed1 = open("/dev/null", O_RDONLY | O_CLOEXEC);
        const int reclaimed2 = open("/dev/null", O_RDONLY | O_CLOEXEC);
        require(reclaimed1 >= 0 && reclaimed2 >= 0, "constructor failure leaked epoll/signalfd");
        close(reclaimed1);
        close(reclaimed2);
        for (const int fd : fillers) close(fd);
        require(setrlimit(RLIMIT_NOFILE, &previous) == 0, "could not restore child descriptor limit");
    });
}
} // namespace
