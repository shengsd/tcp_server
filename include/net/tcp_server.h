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

// 新连接回调在 acceptor 线程执行。回调返回后，会话才会在所属 IO 线程启动读取。
using OnConnectHandler = std::function<void(TcpSessionPtr)>;

/**
 * @brief 非阻塞 TCP 服务端。
 *
 * 线程模型：一个独立 acceptor 线程负责接收连接，IOThreadPool 中的线程负责各会话读写。
 * 配置方法和 Start()/Stop() 应由同一个外部控制线程调用，不能与彼此并发执行。
 * TcpServer 为一次性对象：Stop() 后不能再次 Start()，如需重启请创建新实例。
 */
class TcpServer {
public:
    /**
     * @brief 监听所有 IPv4 地址。
     * @param port 监听端口；传 0 时由操作系统分配端口。
     * @param thread_pool_size IO 线程数；传 0 时内部按 1 处理。
     * @param heartbeat_timeout_s 入站数据空闲超时秒数；小于等于 0 表示禁用。
     * @throws std::system_error 打开、绑定或监听 socket 失败时抛出。
     */
    TcpServer(unsigned short port, 
              std::size_t thread_pool_size = std::thread::hardware_concurrency(),
              int heartbeat_timeout_s = 60);

    /**
     * @brief 监听指定的数字 IP 地址。
     * @param address IPv4 或 IPv6 数字地址，例如 "127.0.0.1" 或 "::1"；不解析域名。
     * @param port 监听端口；传 0 时由操作系统分配端口。
     * @param thread_pool_size IO 线程数；传 0 时内部按 1 处理。
     * @param heartbeat_timeout_s 入站数据空闲超时秒数；小于等于 0 表示禁用。
     * @throws std::system_error 地址无效，或打开、绑定、监听失败时抛出。
     */
    TcpServer(const std::string& address,
              unsigned short port,
              std::size_t thread_pool_size = std::thread::hardware_concurrency(),
              int heartbeat_timeout_s = 60);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /**
     * @brief 启动 IO 线程和 acceptor 线程，非阻塞返回。
     *
     * 只允许调用一次。所有回调和发送队列上限应在调用前配置。
     */
    void Start();

    /**
     * @brief 停止接收连接、请求关闭所有会话并等待线程退出。
     *
     * 必须从外部控制线程调用；禁止在 on_connect/on_message/on_close/on_error 回调中调用，
     * 否则抛出 std::logic_error。关闭任务有 3 秒排空宽限期，但这不是硬超时：已经开始
     * 执行的用户回调不能被安全中断，Stop() 会等待这些回调自行返回。
     *
     * Stop() 成功后可重复调用，但服务端不能再次 Start()。
     */
    void Stop();

    // 以下配置方法不是线程安全的，必须在 Start() 前调用。
    // 用户回调抛出的异常会被框架捕获；连接或消息回调异常会关闭对应会话。
    void SetOnConnect(OnConnectHandler cb) { on_connect_ = std::move(cb); }
    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }
    void SetOnError(OnErrorHandler cb) { on_error_ = std::move(cb); }

    /**
     * @brief 设置每个会话允许排队的最大待发送字节数，默认 10 MiB。
     *
     * 超过上限时会拒绝本次发送并异步关闭该会话。必须在 Start() 前调用。
     */
    void SetMaxSendQueueSize(std::size_t max_size) { max_send_queue_size_ = max_size; }

private:
    void DoAccept();

private:
    asio::io_context main_io_context_;
    IOThreadPool io_thread_pool_;
    asio::ip::tcp::acceptor acceptor_;
    int heartbeat_timeout_s_;
    std::atomic<bool> is_running_;
    std::thread acceptor_thread_;
    std::size_t max_send_queue_size_{10 * 1024 * 1024};
    
    std::mutex sessions_mutex_;
    std::unordered_set<TcpSessionPtr> active_sessions_;

    OnConnectHandler on_connect_;
    OnMessageHandler on_message_;
    OnCloseHandler on_close_;
    OnErrorHandler on_error_;
};

} // namespace net

#endif // TCP_SERVER_NET_TCP_SERVER_H_
