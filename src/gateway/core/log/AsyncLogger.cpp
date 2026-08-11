// 双缓冲的两侧:append 跑在任意业务线程,flushThread 独占后端。
//
// 全文围绕一句话:持锁的时间只够交换指针,真正的 write 发生在锁外。
// 由此派生出三条不变量,改动时先确认它们还成立 ——
//   一、一次 append = 一整行,行不跨缓冲、不跨 write,故多线程不交错;
//   二、失败时不留半毁状态,队列到顶宁可丢日志也不丢进程;
//   三、只有 flushThread 写 fd,全进程唯一写者。

#include "gateway/core/log/AsyncLogger.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <chrono>

namespace gateway {

    static ssize_t safe_write(int fd, const void* buf, size_t n) {
        size_t total = 0;                       // 与写入长度同为无符号,免去来回强转
        const char* p = static_cast<const char*>(buf);
        while(total < n) {
            ssize_t n_write = write(fd, p + total, n - total);
            if (n_write < 0) {
                if( errno == EINTR) continue;
                return -1;
            }
            total += static_cast<size_t>(n_write);   // n_write >= 0
        }

        return static_cast<ssize_t>(total);
    }

    // 把"丢了多少块"以一行日志的形式写进日志流本身。
    // 刻意不走 fprintf(stderr):既为了和其余日志同序,也因为调用它的两处都
    // 已经确认 fd 可写。格式对齐 Logger::log 的前缀,读起来和普通日志一致。
    static void report_dropped(int fd, size_t dropped) {
        if (dropped == 0) return;

        char note[128];
        int n = snprintf(note, sizeof(note),
                         "[WARN ] AsyncLogger 待写队列溢出,丢弃 %zu 块缓冲\n", dropped);
        if (n <= 0) return;

        size_t len = static_cast<size_t>(n);
        if (len >= sizeof(note)) len = sizeof(note) - 1;   // snprintf 返回「本应写入」
        safe_write(fd, note, len);
    }

    // 进程单例写 stderr,不写文件。两个理由:
    // 一是全进程只剩一个日志出口 —— 直接 fprintf(stderr) 的那几处已改走 LOG_*,
    //   同一次故障的线索不会再分散在文件与 journal 两个地方;
    // 二是原先的 /tmp/gateway.log 在多数发行版上是 tmpfs,重启即失,
    //   而"排查一次开机后的故障"恰恰是最需要日志的场景。
    // 交给 journald 之后,轮转、持久化(Storage=persistent)、按时间/单元过滤
    // 都是它的既有能力,进程侧不必自己实现。
    AsyncLogger& AsyncLogger::instance() {
        static AsyncLogger inst("", 3);
        return inst;
    }

    AsyncLogger::AsyncLogger(const std::string& filepath, int flush_interval_sec)
    : currentBuffer_(std::make_unique<Buffer>()),nextBuffer_(std::make_unique<Buffer>()),filepath_(filepath),
    flush_interval_sec_(flush_interval_sec),running_(true) {
        currentBuffer_->reserve(kBufferSize);
        nextBuffer_->reserve(kBufferSize);

        // 按硬上限一次到位,免得溢出前夕还在锁内做 vector 扩容
        bufferToWrite_.reserve(kMaxQueuedBuffers);

        thread_ = std::thread([this](){flushThread();});

    }

    AsyncLogger::~AsyncLogger() {
        running_.store(false);
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    // 本文件内的报错一概不走 LOG_*:LOG_* 的终点就是本类,日志系统自身的故障
    // 若再交给它上报,轻则递归,重则在持锁路径上自我阻塞。这是唯一容许绕开
    // 统一日志通道的地方。绕开之后有两个出口,按"此刻 fd 还能不能写"来选:
    //   fprintf(stderr) —— 用于 fd 尚未打开或正可疑的时候(截断、open/write 失败);
    //   safe_write(fd)  —— 见 report_dropped,用于已确认可写、且希望与其余日志同序的时候。
    void AsyncLogger::append(const char* msg, size_t len) {
        if (len > kMaxLogLine) {
            fprintf(stderr, "AsyncLogger: 日志信息将被截断\n");
            len = kMaxLogLine;
        }

        std::lock_guard<std::mutex> lock(mtx_);

        if (len + currentBuffer_->size() < kBufferSize) {
            currentBuffer_->append(msg, len);
            return;
        }

        // 走到这里是要换页了。换页前先看队列有没有到顶 —— 到顶说明后端根本
        // 沉不下去,再往里塞只是把 OOM 提前。此时只能在"丢日志"和"丢进程"
        // 之间选一个,网关上选前者:丢掉的量有 dropped_ 可查,而被 OOM killer
        // 收掉的进程什么都不会留下。
        //
        // 计数交给 flushThread 补记,这里不就地 fprintf(stderr):此刻 stderr
        // 十有八九正是堵住的那个 fd,持着 mtx_ 去写它会把所有前端线程连同
        // flushThread 一起钉死 —— 那就从丢日志变成丢进程了。
        if (bufferToWrite_.size() >= kMaxQueuedBuffers) {
            dropped_ += bufferToWrite_.size() - kKeepOnOverflow;

            // 顺手从将弃的块里补上 nextBuffer_,免得紧接着的换页还要 malloc。
            // 被移走的那块留在尾部,下面一并 erase 掉。
            if (!nextBuffer_) {
                nextBuffer_ = std::move(bufferToWrite_.back());
                nextBuffer_->clear();
            }

            bufferToWrite_.erase(
                bufferToWrite_.begin() + static_cast<std::ptrdiff_t>(kKeepOnOverflow),
                bufferToWrite_.end());
        }

        bufferToWrite_.push_back(std::move(currentBuffer_));

        if (nextBuffer_) {
            currentBuffer_ = std::move(nextBuffer_);
        }
        else {
            currentBuffer_ = std::make_unique<Buffer>();
            currentBuffer_->reserve(kBufferSize);
        }

        currentBuffer_->append(msg,len);
        cv_.notify_one();
    }

    void AsyncLogger::flushThread() {
        // 空路径 = 写 stderr。此时 fd 是借来的,不能 close:关掉 fd 2 之后
        // 后面所有诊断输出(含本函数自己的报错)都会静默消失。
        const bool to_stderr = filepath_.empty();

        int fd = to_stderr ? STDERR_FILENO
                           : open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "AsyncLogger: open '%s' failed: %s\n",
                   filepath_.c_str(), strerror(errno));

            return;
        }

        // 预分配备用 buffer,用于在交换出当前 buffer 后立即补位
        BufferPtr newCurrent = std::make_unique<Buffer>();
        BufferPtr newNext    = std::make_unique<Buffer>();
        newCurrent->reserve(kBufferSize);
        newNext->reserve(kBufferSize);

        std::vector<BufferPtr> localBuffers;
        localBuffers.reserve(kMaxQueuedBuffers);

        while(running_.load()) {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(flush_interval_sec_), [this](){
                return !bufferToWrite_.empty() || !running_.load();
            });

            if (currentBuffer_ && !currentBuffer_->empty()) {
                bufferToWrite_.push_back(std::move(currentBuffer_));
                currentBuffer_ = std::move(newCurrent);
            }

            if (!nextBuffer_) {
                nextBuffer_ = std::move(newNext);
            }

            if (bufferToWrite_.empty()) {
                continue;
            }

            // 只在确定要写的这一轮才取走计数。放在上面那个 continue 之前的话,
            // 遇到空转的一轮就会把计数清掉却没人补记。
            const size_t dropped = dropped_;
            dropped_ = 0;

            localBuffers.swap(bufferToWrite_);
            lock.unlock();

            // append 溢出丢弃时只记了个数,报告推迟到这里:那一刻 fd 多半正堵着,
            // 而此刻我们即将往它写东西,写得进去本身就说明后端已经缓过来了。
            // 走 fd 而不是 fprintf(stderr),是为了让这行落在日志流里断口所在的
            // 位置 —— 读日志的人需要知道那段空白是丢了,不是没发生。
            report_dropped(fd, dropped);

            for (const auto& buf:localBuffers) {
                ssize_t r = safe_write(fd, buf->data(), buf->size());
                if (r < 0) {
                    fprintf(stderr, "AsyncLogger: write failed: %s\n", strerror(errno));
                }

            }

            if (!newCurrent) {
                newCurrent = std::move(localBuffers.back());
                localBuffers.pop_back();
                newCurrent->clear();
            }
            if (!newNext && !(localBuffers.empty()))
            {
                newNext = std::move(localBuffers.back());
                localBuffers.pop_back();
                newNext->clear();
            }

            localBuffers.clear();


        }

        size_t dropped_at_exit = 0;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (currentBuffer_ && !currentBuffer_->empty()) {
                bufferToWrite_.push_back(std::move(currentBuffer_));
            }
            localBuffers.swap(bufferToWrite_);
            dropped_at_exit = dropped_;
            dropped_ = 0;
        }

        // 收尾同样要补记:最后一轮循环之后发生的丢弃,不补在这里就永远没人报了
        report_dropped(fd, dropped_at_exit);

        for (const auto& buf: localBuffers) {
            ssize_t r = safe_write(fd, buf->data(), buf->size());
            if (r < 0) {
                fprintf(stderr, "AsyncLogger: write failed: %s\n", strerror(errno));
            }

        }

        if (!to_stderr) close(fd);

    }
}
