#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace engine {
namespace infra {

template <typename T>
class BufferPool : public std::enable_shared_from_this<BufferPool<T>> {
public:
    using BufferType = std::vector<T>;

    class PoolDeleter {
    public:
        explicit PoolDeleter(std::shared_ptr<BufferPool> pool) : pool_(std::move(pool)) {}
        void operator()(BufferType* ptr) const {
            if (pool_ && pool_->is_alive_.load(std::memory_order_acquire)) {
                pool_->Release(ptr);
            } else {
                delete ptr;
            }
        }
    private:
        std::shared_ptr<BufferPool> pool_;
    };

    using BufferPtr = std::unique_ptr<BufferType, PoolDeleter>;

    static std::shared_ptr<BufferPool> Create(size_t pool_size, size_t buffer_capacity) {
        struct EnableMakeShared : public BufferPool {
            EnableMakeShared(size_t ps, size_t bc) : BufferPool(ps, bc) {}
        };
        return std::make_shared<EnableMakeShared>(pool_size, buffer_capacity);
    }

    ~BufferPool() {
        is_alive_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto buf : free_buffers_) {
            delete buf;
        }
        free_buffers_.clear();
    }

    BufferPtr Acquire() {
        auto self = this->shared_from_this();
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (free_buffers_.empty()) {
            spdlog::warn(" [BufferPool] 缓冲池已耗尽！触发临时动态分配，请注意排查并发压力！");
            auto temp_buf = new BufferType();
            temp_buf->resize(buffer_capacity_, T());
            return BufferPtr(temp_buf, PoolDeleter{self});
        }

        BufferType* ptr = free_buffers_.back();
        free_buffers_.pop_back();
        
        return BufferPtr(ptr, PoolDeleter{self});
    }

private:
    BufferPool(size_t pool_size, size_t buffer_capacity) 
        : buffer_capacity_(buffer_capacity), is_alive_(true) {
        
        spdlog::info(" [BufferPool] 正在预分配物理内存... 容量: {} 块, 每块大小: {}", 
                     pool_size, buffer_capacity);

        for (size_t i = 0; i < pool_size; ++i) {
            auto buf = new BufferType();
            buf->resize(buffer_capacity_, T());
            free_buffers_.push_back(buf);
        }
    }

    void Release(BufferType* ptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_buffers_.push_back(ptr);
    }

    std::vector<BufferType*> free_buffers_;
    size_t buffer_capacity_;
    std::mutex mutex_;
    std::atomic<bool> is_alive_;
};

} // namespace infra
} // namespace engine
