#ifndef TCP_SERVER_NET_TCP_SERVER_H_
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#define TCP_SERVER_NET_TCP_SERVER_H_

#include "net/tcp_session.h"
#include "net/io_thread_pool.h"

#include <asio.hpp>
#include <memory>
#include <functional>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>
#include <mutex>

namespace net {

/**
 * @brief 新客户端连接建立成功时的回调函数定义。
 * @param session 封装新连接的 TcpSession 智能指针
 * @note 该回调在主 Acceptor 线程中同步执行。回调返回后，服务端才会真正将该 Session 调度至其所属 IO 线程并启动读取。
 */
using OnConnectHandler = std::function<void(TcpSessionPtr session)>;

/**
 * @brief TcpServer：高并发多线程非阻塞 TCP 服务端核心管理器。
 *
 * 【架构设计与并发模型】：
 * 1. 主从 Reactor 线程模型：
 *    - 主 Reactor（Acceptor 独立线程）：专门负责 listen 与 async_accept 接入新连接。
 *    - 从 Reactor（IO 线程池 IOThreadPool）：每个工作线程拥有独立的 io_context，负责已连接客户端的读、写和超时检测。
 * 2. 一次性生命周期：
 *    - TcpServer 实例是一次性的：调用 Stop() 后底层的 acceptor 和线程池均已销毁，不能再次调用 Start()。如需重启服务请新建 TcpServer 实例。
 * 3. 优雅停机（Graceful Shutdown）：
 *    - Stop() 采用“排空宽限期”设计，主动向所有活跃 Session 投递关闭请求，并等待所有正在进行的用户回调和异步写入安全收尾。
 */
class TcpServer {
public:
    /**
     * @brief 构造函数：监听所有 IPv4 地址（0.0.0.0）。
     * @param port 监听的端口号；如果传 0 则由操作系统动态分配可用端口。
     * @param thread_pool_size IO 工作线程数；默认等于 CPU 核心数，传 0 内部自动按 1 处理。
     * @param heartbeat_timeout_s 每个客户端会话的入站心跳超时秒数；<= 0 表示禁用心跳。
     * @throws std::system_error 当端口已被占用（bind 失败）或底层 socket 初始化失败时抛出。
     */
    TcpServer(unsigned short port, 
              std::size_t thread_pool_size = std::thread::hardware_concurrency(),
              int heartbeat_timeout_s = 60);

    /**
     * @brief 构造函数：监听指定的 IP 地址（支持 IPv4 / IPv6 字符串）。
     * @param address 绑定的 IP 地址字符串（如 "127.0.0.1" 或 "::1"）。
     * @param port 监听的端口号。
     * @param thread_pool_size IO 工作线程数。
     * @param heartbeat_timeout_s 入站心跳超时秒数。
     * @throws std::system_error 当 IP 地址格式非法或网络绑定失败时抛出。
     */
    TcpServer(const std::string& address,
              unsigned short port,
              std::size_t thread_pool_size = std::thread::hardware_concurrency(),
              int heartbeat_timeout_s = 60);

    /**
     * @brief 析构函数：保证对象析构时自动触发 Stop()，安全回收底层所有线程和连接。
     */
    ~TcpServer();

    // 禁用拷贝语义（服务器持有唯一网络监听与线程资源，禁止复制）
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /**
     * @brief 启动 TCP 服务端。
     * 
     * 【行为特性】：
     * - 非阻塞调用：创建底层 IO 线程池并启动 Acceptor 监听子线程后立即返回，不阻塞当前调用者线程。
     * - 幂等性：若已处于运行状态，重复调用将直接忽略。
     * - 配置前提：所有事件回调（SetOnConnect 等）和参数配置必须在 Start() 调用前完成。
     */
    void Start();

    /**
     * @brief 优雅停止服务端并回收所有资源。
     *
     * 【关闭时序与安全约束】：
     * 1. 外部线程约束：必须从外部控制线程（如 main 函数的主线程）调用；严禁在 IO 回调（如 on_message）中调用，否则抛出 std::logic_error 异常。
     * 2. 会话优雅排空：所有 active_sessions_ 并行进入 Draining，共享 3 秒截止时间，随后完成回调屏障。
     * 3. 线程安全 join：关闭 main_io_context 并依次 join acceptor 线程与 IO 工作线程池。
     */
    void Stop();

    // ================= 全局回调与参数配置（必须在 Start() 前完成） ================= //

    /** @brief 设置新连接建立时的通知回调 */
    void SetOnConnect(OnConnectHandler cb) { on_connect_ = std::move(cb); }

    /** @brief 设置接收到客户端数据时的通知回调（会被每个新创建的 Session 继承） */
    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }

    /** @brief 设置客户端断开连接时的通知回调 */
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }

    /** @brief 设置发生网络 I/O 错误时的通知回调 */
    void SetOnError(OnErrorHandler cb) { on_error_ = std::move(cb); }

    /**
     * @brief 设置每个客户端会话允许排队的最大发送字节阈值（默认 10 MiB）。
     * @param max_size 字节数。超过此阈值将切断恶意/慢速客户端。
     */
    void SetMaxSendQueueSize(std::size_t max_size) { max_send_queue_size_ = max_size; }

private:
    /**
     * @brief 异步监听接入循环（Accept 状态机）。
     * 
     * 从 IOThreadPool 轮询获取下一个目标 io_context，并将新连接的 socket 绑定至该 io_context。
     */
    void DoAccept();

private:
    asio::io_context main_io_context_;         ///< Acceptor 独立事件循环
    IOThreadPool io_thread_pool_;              ///< 从 Reactor 工作线程池
    asio::ip::tcp::acceptor acceptor_;         ///< 底层 TCP 监听套接字
    int heartbeat_timeout_s_;                  ///< 心跳检测超时秒数
    std::atomic<bool> is_running_;             ///< 服务端运行状态原子标志位
    std::thread acceptor_thread_;              ///< 专门跑 main_io_context_.run() 的 Acceptor 线程
    std::size_t max_send_queue_size_{10 * 1024 * 1024}; ///< 默认发送队列高水位（10MB）
    
    std::mutex sessions_mutex_;                ///< 保护 active_sessions_ 集合的互斥锁
    std::unordered_set<TcpSessionPtr> active_sessions_; ///< 当前所有存活的会话集合（持有 shared_ptr 保证生命周期）

    OnConnectHandler on_connect_;              ///< 接入回调
    OnMessageHandler on_message_;              ///< 消息回调
    OnCloseHandler on_close_;                  ///< 关闭回调
    OnErrorHandler on_error_;                  ///< 错误回调
};

} // namespace net

#endif // TCP_SERVER_NET_TCP_SERVER_H_
