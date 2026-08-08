#include "net/io_thread_pool.h"
#include <iostream>
#include <stdexcept>

namespace net {

// 构造函数：初始化指定数量的 io_context 和 WorkGuard
IOThreadPool::IOThreadPool(std::size_t pool_size)
    : pool_size_(pool_size == 0 ? 1 : pool_size), // 至少保证有 1 个工作线程
      next_index_(0),
      running_(false) {
    for (std::size_t i = 0; i < pool_size_; ++i) {
        auto io_ctx = std::make_shared<asio::io_context>();
        io_contexts_.push_back(io_ctx);
        // 为每个 io_context 分配一个 work_guard。
        // 原理：在没有活跃异步事件时，默认 io_context::run() 会直接退出。
        // 持有 work_guard 可以维持事件循环处于待命状态，即使没有活跃连接也不退出。
        work_guards_.push_back(asio::make_work_guard(*io_ctx));
    }
}

// 析构函数：确保对象销毁时所有工作线程已回收
IOThreadPool::~IOThreadPool() {
    Stop();
}

// 启动线程池：创建 std::thread 并运行各个 io_context 的事件循环
void IOThreadPool::Start() {
    if (running_) return; // 防止重复启动
    running_ = true;

    for (std::size_t i = 0; i < pool_size_; ++i) {
        // 创建工作线程，每个线程绑定其独立的 io_context 实例
        threads_.emplace_back([this, i]() {
            try {
                // 进入事件循环：阻塞等待并分发在该 io_context 注册的读/写/定时器事件
                io_contexts_[i]->run();
            } catch (const std::exception& e) {
                // 捕获可能从 handler 逃逸的未捕获异常，防止工作线程直接 crash 整个进程
                std::cerr << "[IOThreadPool] Thread " << i << " exception: " << e.what() << std::endl;
            }
        });
    }
}

// 停止线程池：安全退出事件循环并等待所有线程结束
void IOThreadPool::Stop() {
    // 【防死锁保障】：如果某个 IO 线程在处理用户回调时调用了 Stop()，
    // 它接下来如果对自己执行 join()，会导致永远自己等自己，发生永久死锁。
    if (IsCurrentThread()) {
        throw std::logic_error("IOThreadPool::Stop() cannot be called from within its own thread.");
    }

    if (!running_) return;
    running_ = false;

    // 1. 释放工作守卫（reset work guards）：
    // 告诉 io_context 不再有“人工维持”的待办任务。一旦队列中现存的已注册任务（如 Close 回调）执行完毕，run() 就会自然返回。
    for (auto& guard : work_guards_) {
        guard.reset();
    }

    // 2. 显式停止所有 io_context：
    // 强制唤醒正在阻塞等待网络事件（如阻塞在 epoll_wait/kevent）的线程，使其立刻退出 run()。
    for (auto& io_ctx : io_contexts_) {
        io_ctx->stop();
    }

    // 3. 等待所有工作线程执行完毕并回收资源
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

// 检查当前调用线程的 ID 是否在工作线程列表中
bool IOThreadPool::IsCurrentThread() const {
    auto current_id = std::this_thread::get_id();
    for (const auto& t : threads_) {
        if (t.get_id() == current_id) return true;
    }
    return false;
}

// 轮询（Round-Robin）返回下一个 io_context
asio::io_context& IOThreadPool::GetNextIOContext() {
    // 利用 fetch_add 原子操作递增序号，并在 pool_size_ 范围内取模，实现无锁的跨线程均匀分发
    std::size_t index = next_index_.fetch_add(1, std::memory_order_relaxed) % pool_size_;
    return *io_contexts_[index];
}

} // namespace net
