#include "net/io_thread_pool.h"
#include <iostream>
#include <stdexcept>

namespace net {

IOThreadPool::IOThreadPool(std::size_t pool_size)
    : pool_size_(pool_size == 0 ? 1 : pool_size),
      next_index_(0),
      running_(false) {
    for (std::size_t i = 0; i < pool_size_; ++i) {
        auto io_ctx = std::make_shared<asio::io_context>();
        io_contexts_.push_back(io_ctx);
        work_guards_.push_back(asio::make_work_guard(*io_ctx));
    }
}

IOThreadPool::~IOThreadPool() {
    Stop();
}

void IOThreadPool::Start() {
    if (running_) return;
    running_ = true;

    for (std::size_t i = 0; i < pool_size_; ++i) {
        threads_.emplace_back([this, i]() {
            try {
                io_contexts_[i]->run();
            } catch (const std::exception& e) {
                std::cerr << "[IOThreadPool] Thread " << i << " exception: " << e.what() << std::endl;
            }
        });
    }
}

void IOThreadPool::Stop() {
    if (IsCurrentThread()) {
        throw std::logic_error("IOThreadPool::Stop() cannot be called from within its own thread.");
    }

    if (!running_) return;
    running_ = false;

    // 撤销工作守卫，使得 io_context 可以在执行完积压任务（如 gracefully 投递的 Close）后自然退出
    for (auto& guard : work_guards_) {
        guard.reset();
    }

    // 强制终止剩余阻塞中的上下文，打破由异常连接造成的长期等待死锁
    for (auto& io_ctx : io_contexts_) {
        io_ctx->stop();
    }

    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

bool IOThreadPool::IsCurrentThread() const {
    auto current_id = std::this_thread::get_id();
    for (const auto& t : threads_) {
        if (t.get_id() == current_id) return true;
    }
    return false;
}

asio::io_context& IOThreadPool::GetNextIOContext() {
    std::size_t index = next_index_.fetch_add(1, std::memory_order_relaxed) % pool_size_;
    return *io_contexts_[index];
}

} // namespace net
