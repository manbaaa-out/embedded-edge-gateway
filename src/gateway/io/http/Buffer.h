#pragma once

// 每条 HTTP 连接的读缓冲,muduo Buffer 的简化版:两个游标把一块内存切成
// 「已读(可回收) | 待解析 | 空闲可写」三段。
//
// 需要它是因为 TCP 和串口一样是字节流:一次 recv 可能读到半个请求、一个半、
// 或者两个粘在一起,得把「读到的」和「已经解析掉的」分开管。
//
// 真正有技术含量的是 makeSpace 的「先腾挪、不够再扩容」—— 那是长连接不会把内存
// 吃穿的原因,详见该函数注释。

#include <vector>
#include <algorithm>
#include <cstddef>

namespace gateway {



class Buffer {

    public:
    Buffer(): buffer_(kInitialSize),readerIndex_(0),writerIndex_(0) {}

    size_t readableBytes() const {
        return writerIndex_ - readerIndex_;
    }

    size_t writableBytes() const {
        return buffer_.size() - writerIndex_;
    }

    const char* peek() const {
        return buffer_.data() + readerIndex_;
    }

    void retrieve(size_t n) {
        if (n < readableBytes()) {
            readerIndex_ += n;        // 尚有剩余数据,只推进读游标
        } else {
            retrieveAll();            // 全部消费,游标归零以复用空间
        }
    }

    void retrieveAll() {
        // 全部消费,两个游标都归零
        readerIndex_ = 0;
        writerIndex_ = 0;
    }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::copy(data, data + len, buffer_.data() + writerIndex_);
        writerIndex_ += len;
    }

    const char* beginWrite() const {
        return buffer_.data() + writerIndex_;
    }


    private:
    static constexpr size_t kInitialSize = 1024;

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    void ensureWritableBytes(size_t len) {
        if (writableBytes() >= len) {
            return;
        }
        // 空间不足:优先腾挪,不足再扩容
        makeSpace(len);
    }

    void makeSpace(size_t len) {
        if (readerIndex_ + writableBytes() >= len) {
            // 已读区 + 尾部空闲足够,把待读数据前移以复用已读区,免去扩容
            size_t readable = readableBytes();
            std::copy(buffer_.data() + readerIndex_,
                    buffer_.data() + writerIndex_,
                    buffer_.data());
            readerIndex_ = 0;
            writerIndex_ = readerIndex_ + readable;
        } else {
            // 腾挪后仍不足,扩容
            buffer_.resize(writerIndex_ + len);
        }
    }
};

}
