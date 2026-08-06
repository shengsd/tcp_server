#ifndef TCP_SERVER_NET_IO_THREAD_POOL_H_
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#define TCP_SERVER_NET_IO_THREAD_POOL_H_

#include <asio.hpp>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <cstddef>

namespace net {

class IOThreadPool {
public:
    explicit IOThreadPool(std::size_t pool_size = std::thread::hardware_concurrency());
    ~IOThreadPool();

    IOThreadPool(const IOThreadPool&) = delete;
    IOThreadPool& operator=(const IOThreadPool&) = delete;

    void Start();
    void Stop();

    // 轮询获取下一个 io_context
    asio::io_context& GetNextIOContext();

    std::size_t GetPoolSize() const { return pool_size_; }

private:
    using IOContextPtr = std::shared_ptr<asio::io_context>;
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    std::size_t pool_size_;
    std::atomic<std::size_t> next_index_;
    std::vector<IOContextPtr> io_contexts_;
    std::vector<WorkGuard> work_guards_;
    std::vector<std::thread> threads_;
    bool running_;
};

} // namespace net

#endif // TCP_SERVER_NET_IO_THREAD_POOL_H_
