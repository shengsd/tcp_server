#include "net/io_thread_pool.h"
#include <iostream>

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
    if (!running_) return;
    running_ = false;

    for (auto& guard : work_guards_) {
        guard.reset();
    }

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

asio::io_context& IOThreadPool::GetNextIOContext() {
    std::size_t index = next_index_.fetch_add(1, std::memory_order_relaxed) % pool_size_;
    return *io_contexts_[index];
}

} // namespace net
