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
 * @brief IO 线程池：采用 "One io_context per thread"（每个线程一个独立事件循环）模型。
 *
 * 【架构设计与优势】：
 * 1. 独立事件循环：每个工作线程独占一个 asio::io_context，彼此没有锁竞争，各线程独立进行 epoll/kqueue 事件轮询。
 * 2. 负载均衡：新接入的 TCP 连接通过轮询（Round-Robin）算法均匀分配给不同的 io_context。
 * 3. 任务串行化：分配到同一个 io_context 的同一个 Session，其读写和定时器事件都在该特定线程中串行触发，天然避免多线程竞态。
 *
 * 【生命周期与线程安全规则】：
 * 1. 外部控制约束：Start()、Stop() 以及构造/析构必须由同一个外部控制线程（通常为主线程）调用，禁止并发调用。
 * 2. 一次性生命周期：Stop() 调用后会释放工作守卫并终止所有 io_context，该对象不可再次 Start()。如需重启服务请新建实例。
 * 3. 防死锁保障：禁止从 IO 线程池内部的工作线程中调用 Stop()（内部做了 IsCurrentThread 检查并抛出异常）。
 */
class IOThreadPool {
public:
    /**
     * @brief 构造函数：初始化 IO 上下文与工作守卫。
     * @param pool_size IO 线程数。若传入 0，内部会自动纠正为 1，确保至少有一个可用工作线程。
     */
    explicit IOThreadPool(std::size_t pool_size = std::thread::hardware_concurrency());

    /**
     * @brief 析构函数：确保对象销毁时所有工作线程已安全停止并回收（调用 Stop()）。
     */
    ~IOThreadPool();

    // 禁用拷贝构造和赋值运算符（管理底层线程与上下文资源，禁止复制）
    IOThreadPool(const IOThreadPool&) = delete;
    IOThreadPool& operator=(const IOThreadPool&) = delete;

    /**
     * @brief 创建并启动所有工作线程，使各线程开始运行 io_context::run()。
     * @note 非阻塞调用，线程创建后立即返回。只允许调用一次。
     */
    void Start();

    /**
     * @brief 停止所有 IO 事件循环，并等待（join）所有工作线程退出。
     * 
     * 【关闭流程细节】：
     * 1. 检查是否在工作线程中调用，防止自身 join 导致死锁。
     * 2. 重置 work_guard，允许没有未决事件的 io_context 自然退出。
     * 3. 显式调用 io_context::stop()，强行唤醒阻塞在 epoll_wait 中的线程。
     * 4. 逐一 join 工作线程，确保线程安全回收。
     */
    void Stop();

    /**
     * @brief 判断当前正在执行的线程是否属于本线程池的工作线程。
     * @return true 表示当前线程是工作线程；false 表示外部线程。
     * @note 核心用于 Stop() 前的防死锁检查，避免“自等待”死锁。
     */
    bool IsCurrentThread() const;

    /**
     * @brief 采用轮询（Round-Robin）策略获取下一个 io_context 的引用。
     * @return asio::io_context& 返回给新连接绑定的 IO 上下文。
     * @note 返回的引用在 IOThreadPool 生命周期内长期有效；原子计数保证了跨线程分配时的线程安全。
     */
    asio::io_context& GetNextIOContext();

    /**
     * @brief 获取线程池中实际运行的工作线程数量。
     * @return std::size_t 始终 >= 1。
     */
    std::size_t GetPoolSize() const { return pool_size_; }

private:
    // 类型别名定义
    using IOContextPtr = std::shared_ptr<asio::io_context>;
    // WorkGuard：用于防止 io_context 在没有任务时因 run() 返回而退出
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    std::size_t pool_size_;                  ///< 线程池大小（至少为 1）
    std::atomic<std::size_t> next_index_;    ///< 轮询分配连接时使用的原子索引计数器
    std::vector<IOContextPtr> io_contexts_;  ///< 保存每个线程独占的 io_context 智能指针集合
    std::vector<WorkGuard> work_guards_;     ///< 工作守卫集合，持有它以维持 io_context::run() 持续运行
    std::vector<std::thread> threads_;       ///< 底层工作线程集合
    bool running_;                           ///< 线程池运行状态标志位
};

} // namespace net

#endif // TCP_SERVER_NET_IO_THREAD_POOL_H_
