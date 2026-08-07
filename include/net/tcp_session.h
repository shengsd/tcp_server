#ifndef TCP_SERVER_NET_TCP_SESSION_H_
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#define TCP_SERVER_NET_TCP_SESSION_H_

#include <asio.hpp>
#include <memory>
#include <functional>
#include <deque>
#include <vector>
#include <string>
#include <atomic>
#include <array>

namespace net {

class TcpServer;
class TcpSession;
using TcpSessionPtr = std::shared_ptr<TcpSession>;

// 消息回调在会话所属 IO 线程执行。data 只在本次回调返回前有效，如需异步使用必须复制。
using OnMessageHandler = std::function<void(TcpSessionPtr, const uint8_t*, std::size_t)>;
// 关闭回调在 socket 和心跳定时器关闭后，于会话所属 IO 线程执行，且最多执行一次。
using OnCloseHandler = std::function<void(TcpSessionPtr)>;
// IO 错误回调在会话所属 IO 线程执行，随后框架会请求关闭会话；主动取消不触发该回调。
using OnErrorHandler = std::function<void(TcpSessionPtr, const std::error_code&)>;

/**
 * @brief 单个 TCP 连接的异步会话。
 *
 * 对象必须由 std::shared_ptr 管理，内部异步任务依赖 shared_from_this() 保持生命周期。
 * Send() 和 Close() 可从业务线程调用，真正的 socket、timer 和发送队列操作都会在所属
 * IO executor 中串行执行。回调和超时等配置必须在 Start() 前完成。
 */
class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    /**
     * @brief 接管一个已连接的 socket，但不会自动开始读取。
     * @param socket 已连接的 TCP socket。
     * @param heartbeat_timeout_s 入站数据空闲超时秒数；小于等于 0 表示禁用。
     *
     * 通过 TcpServer 获得的会话会由服务器自动启动，调用者不应再次调用 Start()。
     */
    explicit TcpSession(asio::ip::tcp::socket socket, int heartbeat_timeout_s = 60);
    ~TcpSession();

    TcpSession(const TcpSession&) = delete;
    TcpSession& operator=(const TcpSession&) = delete;

    /**
     * @brief 开始心跳计时和异步读取。
     *
     * 只能调用一次，并且应在 socket 所属 executor 中调用；TcpServer 会自动满足该约束。
     */
    void Start();

    /**
     * @brief 复制 data[0, length) 并异步加入发送队列，可从业务线程调用。
     *
     * 函数返回后调用者可以立即释放原始数据。空数据或已请求关闭的会话会被忽略；超过
     * 发送队列上限时会异步关闭连接。
     */
    void Send(const uint8_t* data, std::size_t length);

    /** @brief 复制字符串内容并异步发送，语义与字节版本一致。 */
    void Send(const std::string& message);

    /**
     * @brief 幂等地请求异步关闭，可从业务线程调用。
     *
     * 返回时只保证关闭请求已提交，不保证底层 socket 已关闭；实际关闭完成后才会调用
     * OnCloseHandler。对象析构时仍会直接释放底层资源，但不会再调用用户回调。
     */
    void Close();

    /**
     * @brief 返回底层 socket，仅供查询或在会话启动前设置 socket 选项。
     *
     * 不要绕过 TcpSession 直接发起异步读写、关闭 socket，或从其他线程并发操作它，
     * 否则会破坏会话的串行化和生命周期保证。
     */
    asio::ip::tcp::socket& GetSocket() { return socket_; }

    // true 表示已经请求关闭；不表示异步关闭任务和 OnCloseHandler 已执行完毕。
    bool IsClosed() const { return is_closed_.load(); }

    // 远端信息在构造时缓存，获取时不访问 socket；端点查询失败时地址为空。
    std::string GetRemoteAddress() const { return remote_address_; }
    unsigned short GetRemotePort() const { return remote_port_; }

    // 以下配置方法不是线程安全的，应在 Start() 前调用。
    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }
    void SetOnError(OnErrorHandler cb) { on_error_ = std::move(cb); }

    // 设置入站数据空闲超时；小于等于 0 表示禁用。发送数据不会重置该计时器。
    void SetHeartbeatTimeout(int seconds) { heartbeat_timeout_s_ = seconds; }

    // 设置最大待发送字节数；超过上限时会请求关闭会话。默认 10 MiB。
    void SetMaxSendQueueSize(std::size_t max_size) { max_send_queue_size_ = max_size; }

    // 仅供 TcpServer 内部使用，用于清理 active_sessions_；普通调用者不要设置。
    void SetInternalCloseHandler(std::function<void(TcpSessionPtr)> cb) { internal_close_handler_ = std::move(cb); }

private:
    friend class TcpServer;

    void Close(std::function<void()> completion);
    void DoRead();
    void DoWrite();
    void ResetHeartbeatTimer();
    void HandleError(const std::error_code& ec);

private:
    asio::ip::tcp::socket socket_;
    std::string remote_address_;
    unsigned short remote_port_;
    asio::steady_timer heartbeat_timer_;
    int heartbeat_timeout_s_;
    std::atomic<bool> is_closed_;
    bool close_completed_;

    std::array<uint8_t, 8192> read_buffer_;
    std::deque<std::vector<uint8_t>> write_queue_;
    std::size_t max_send_queue_size_{10 * 1024 * 1024}; // 默认10MB
    std::atomic<std::size_t> current_send_queue_size_{0};

    OnMessageHandler on_message_;
    OnCloseHandler on_close_;
    OnErrorHandler on_error_;
    std::function<void(TcpSessionPtr)> internal_close_handler_;
};

} // namespace net

#endif // TCP_SERVER_NET_TCP_SESSION_H_
