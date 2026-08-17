#pragma once

/**
 * @file
 * HTTP 连接的连续增量读缓冲。
 *
 * readerIndex_ 与 writerIndex_ 将存储区划分为已消费、待读和尾部空闲三段。尾部不足
 * 时先把待读数据前移复用头部空间，仍不够才扩容，适合多次 recv 和请求粘包。
 */

#include <vector>
#include <algorithm>
#include <cstddef>

namespace gateway {
/** 维护一段可追加、可按前缀消费的字节序列。 */
class Buffer {

    public:
    /** 创建具有 kInitialSize 字节容量的空缓冲。 */
    Buffer(): buffer_(kInitialSize),readerIndex_(0),writerIndex_(0) {}

    /** @return 当前尚未消费、可供解析器读取的字节数。 */
    size_t readableBytes() const {
        return writerIndex_ - readerIndex_;
    }

    /** @return writerIndex_ 之后无需扩容即可写入的字节数。 */
    size_t writableBytes() const {
        return buffer_.size() - writerIndex_;
    }

    /** @return 待读区域首字节的借用指针，下一次修改缓冲后可能失效。 */
    const char* peek() const {
        return buffer_.data() + readerIndex_;
    }

    /**
     * 消费待读区域的前缀。
     * @param n 期望消费的字节数；达到或超过 readableBytes() 时清空缓冲。
     */
    void retrieve(size_t n) {
        if (n < readableBytes()) {
            readerIndex_ += n;
        } else {
            retrieveAll();
        }
    }

    /** 将缓冲恢复为空，并保留已经分配的容量。 */
    void retrieveAll() {
        readerIndex_ = 0;
        writerIndex_ = 0;
    }

    /**
     * 在待读区域尾部追加字节。
     * @param data 输入缓冲起点，至少包含 len 字节。
     * @param len 追加的字节数。
     */
    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::copy(data, data + len, buffer_.data() + writerIndex_);
        writerIndex_ += len;
    }

    /** @return 待读区域末尾的一过尾借用指针。 */
    const char* beginWrite() const {
        return buffer_.data() + writerIndex_;
    }


    private:
    static constexpr size_t kInitialSize = 1024;  ///< 初始容量，单位为字节。

    std::vector<char> buffer_;  ///< 拥有实际字节存储的连续数组。
    size_t readerIndex_;        ///< 待读区域起点索引。
    size_t writerIndex_;        ///< 待读区域末尾的一过尾索引。

    /** @param len 调用方即将追加、必须保证可写的字节数。 */
    void ensureWritableBytes(size_t len) {
        if (writableBytes() >= len) {
            return;
        }
        makeSpace(len);
    }

    /**
     * 通过前移待读数据或扩容提供空间。
     * @param len 调用方需要的连续尾部空间，单位为字节。
     */
    void makeSpace(size_t len) {
        if (readerIndex_ + writableBytes() >= len) {
            // 回收头部已消费空间，避免长连接只因游标后移而反复扩容。
            size_t readable = readableBytes();  // 前移前需要保留的有效字节数。
            std::copy(buffer_.data() + readerIndex_,
                    buffer_.data() + writerIndex_,
                    buffer_.data());
            readerIndex_ = 0;
            writerIndex_ = readerIndex_ + readable;
        } else {
            buffer_.resize(writerIndex_ + len);
        }
    }
};

}
