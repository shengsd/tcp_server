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

/**
 * @brief One io_context per thread 的固定大小 IO 线程池。
 *
 * Start()/Stop() 以及其他生命周期操作应由同一个外部控制线程调用，不能并发执行。
 * 线程池为一次性对象：Stop() 会停止 io_context 并释放 work guard，之后不能再次 Start()。
 */
class IOThreadPool {
public:
    /** @param pool_size IO 线程数；传 0 时内部按 1 处理。 */
    explicit IOThreadPool(std::size_t pool_size = std::thread::hardware_concurrency());
    ~IOThreadPool();

    IOThreadPool(const IOThreadPool&) = delete;
    IOThreadPool& operator=(const IOThreadPool&) = delete;

    /** @brief 创建工作线程并非阻塞返回，只允许调用一次。 */
    void Start();

    /**
     * @brief 停止所有 io_context 并等待工作线程退出。
     *
     * 尚未执行的 handler 可能不会被调用。禁止从池内工作线程调用，否则抛出
     * std::logic_error；应由外部控制线程调用。
     */
    void Stop();

    /**
     * @brief 判断调用线程是否为本线程池的工作线程。
     * @note 仅用于调用 Stop() 前的防死锁检查，不提供通用同步保证。
     */
    bool IsCurrentThread() const;

    /**
     * @brief 轮询返回下一个 io_context。
     * @return 由线程池持有的引用，只在本对象生命周期内有效。
     * @note 通常在 Start() 前为长期对象分配 context，或运行期间用于投递任务；Stop() 后
     *       context 已停止，不能再依赖新任务得到执行。
     */
    asio::io_context& GetNextIOContext();

    /** @brief 返回实际线程数，始终至少为 1。 */
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
