#pragma once
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
            readerIndex_ += n;        // 还有剩余数据,只推进读游标
        } else {
            retrieveAll();            // n == readableBytes(),全消费完,游标归零复用
        }
    }

    void retrieveAll() {
        // 全部消费,两个游标都归零
        readerIndex_ = 0;
        writerIndex_ = 0;
    }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);    // ← 先保证有 len 的可写空间
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
            return;                   // 空间够,直接返回
        }
        // 空间不够,腾挪 or 扩容
        makeSpace(len);
    }

    void makeSpace(size_t len) {
        if (readerIndex_ + writableBytes() >= len) {
            // 情况 A:已读区 + 尾部空闲 够用,不扩容,把数据往前挪
            size_t readable = readableBytes();
            std::copy(buffer_.data() + readerIndex_,      // 待读数据起点
                    buffer_.data() + writerIndex_,      // 待读数据终点
                    buffer_.data());                    // 挪到最前面
            readerIndex_ = 0;
            writerIndex_ = readerIndex_ + readable;
        } else {
            // 情况 B:真不够,扩容
            buffer_.resize(writerIndex_ + len);
        }
    }
};

}
