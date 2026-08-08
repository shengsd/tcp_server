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

/**
 * @brief 接收数据回调定义
 * @param session 触发事件的 TcpSession 智能指针（保证回调期间会话对象存活）
 * @param data 接收到的原始字节数组首地址（只在回调函数返回前有效，如需异步使用必须自行深拷贝）
 * @param length 接收到的字节长度
 * @note 回调在会话归属的 IO 工作线程中同步执行。
 */
using OnMessageHandler = std::function<void(TcpSessionPtr session, const uint8_t* data, std::size_t length)>;

/**
 * @brief 会话断开回调定义
 * @param session 触发事件的 TcpSession 智能指针
 * @note 当底层 socket 和心跳定时器完全关闭后触发，在 IO 工作线程中执行，且保证每个会话只触发一次。
 */
using OnCloseHandler = std::function<void(TcpSessionPtr session)>;

/**
 * @brief 网络错误回调定义
 * @param session 触发事件的 TcpSession 智能指针
 * @param ec 具体的错误码（如连接重置 asio::error::connection_reset 等）
 * @note 仅在发生非主动取消的异常网络错误时触发；触发后框架会自动启动会话关闭流程。
 */
using OnErrorHandler = std::function<void(TcpSessionPtr session, const std::error_code& ec)>;

/**
 * @brief TcpSession：代表单个 TCP 客户端连接的异步状态机与读写通道。
 *
 * 【核心设计与生命周期管理】：
 * 1. 引用计数保证：继承自 std::enable_shared_from_this<TcpSession>。在发起任何底层 Asio 异步操作
 *    （async_read_some、async_write、async_wait）时，都会通过 shared_from_this() 生成一个副本捕获到
 *    Lambda 闭包中。只要底层内核还有未决的 I/O 操作，该 Session 对象就绝对不会被提前析构，彻底消除悬空指针。
 * 2. 线程模型与串行化：每个 Session 在创建时绑定到一个特定的 IO 线程（io_context）。虽然 Send() 和 Close()
 *    允许从任意外部业务线程调用，但底层真正的 socket 读写、发送队列操作与定时器重置全部通过 asio::post
 *    排队在绑定的 IO 线程中串行执行，因此无需对 socket 加重锁。
 * 3. 发送背压机制（Backpressure）：内置发送队列与原子高水位限制，防止客户端因“慢网络/只读不读”导致服务端内存无底线暴涨。
 * 4. 心跳保活机制：内置入站数据空闲检测定时器，规定时间内无数据到达将主动切断僵尸连接。
 */
class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    /**
     * @brief 构造函数：接管已连接的 socket。
     * @param socket 已完成 TCP 三次握手的客户端 socket（通过 std::move 转移所有权）。
     * @param heartbeat_timeout_s 入站数据空闲超时时间（秒）。<=0 表示禁用心跳检测。
     * @note 构造时会缓存对端的 IP 和端口，因此后续断开后仍可查询地址信息。
     */
    explicit TcpSession(asio::ip::tcp::socket socket, int heartbeat_timeout_s = 60);

    /**
     * @brief 析构函数：释放 socket 与定时器资源。
     */
    ~TcpSession();

    // 禁用拷贝语义（Socket 具有唯一所有权，禁止拷贝）
    TcpSession(const TcpSession&) = delete;
    TcpSession& operator=(const TcpSession&) = delete;

    /**
     * @brief 启动会话：开启心跳定时器并挂起第一次异步读操作。
     * @note 只能调用一次，且必须在 socket 归属的 executor 中调用（由 TcpServer 内部自动调度）。
     */
    void Start();

    /**
     * @brief 异步发送二进制字节数据。
     * @param data 要发送的字节指针
     * @param length 字节长度
     * 
     * 【特性与线程安全性】：
     * - 线程安全：可从任意业务线程并发调用。
     * - 内存语义：函数内部会立即深拷贝 data[0, length) 到缓冲区中，函数返回后调用方可立即释放原数据。
     * - 发送队列与背压：如果当前有未完成的写操作，数据将存入 write_queue_；如果当前排队总字节数超过
     *   max_send_queue_size_，将拒绝发送并自动触发 Close() 切断连接防 OOM。
     */
    void Send(const uint8_t* data, std::size_t length);

    /**
     * @brief 异步发送字符串（重载便捷方法）。
     * @param message 待发送的字符串
     */
    void Send(const std::string& message);

    /**
     * @brief 幂等地请求异步关闭连接。
     * @note 线程安全。函数返回时仅代表关闭任务已排入 IO 线程，不代表底层连接已立刻断开。
     *       底层完全关闭后会触发 on_close 回调。
     */
    void Close();

    /**
     * @brief 获取底层 socket 对象的引用。
     * @note 仅用于在会话启动前配置底层 socket 选项（如 TCP_NODELAY、SO_KEEPALIVE 等）。
     *       严禁直接绕过 TcpSession 发起裸 async 读写，否则会破坏串行化约束导致未定义行为。
     */
    asio::ip::tcp::socket& GetSocket() { return socket_; }

    /**
     * @brief 检查当前是否已经发起了关闭请求。
     * @return true 表示已调用过 Close() 或连接已进入断开流程。
     */
    bool IsClosed() const { return is_closed_.load(std::memory_order_acquire); }

    /** @brief 获取对端客户端的 IP 地址字符串（如 "127.0.0.1"） */
    std::string GetRemoteAddress() const { return remote_address_; }

    /** @brief 获取对端客户端的连接端口号 */
    unsigned short GetRemotePort() const { return remote_port_; }

    // ================= 回调与参数配置（必须在 Start() 前完成） ================= //

    /** @brief 设置接收到应用层数据时的回调函数 */
    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }

    /** @brief 设置连接彻底关闭时的回调函数 */
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }

    /** @brief 设置发生网络 I/O 错误时的回调函数 */
    void SetOnError(OnErrorHandler cb) { on_error_ = std::move(cb); }

    /** @brief 动态设置心跳空闲超时时间（秒）。<=0 表示不超时 */
    void SetHeartbeatTimeout(int seconds) { heartbeat_timeout_s_ = seconds; }

    /** @brief 设置发送队列允许积压的最大字节阈值（默认 10 MiB） */
    void SetMaxSendQueueSize(std::size_t max_size) { max_send_queue_size_ = max_size; }

    /**
     * @brief 框架内部使用的关闭清理回调（供 TcpServer 内部从 active_sessions_ 集合中剔除自身）。
     * @note 普通上层业务代码无需调用。
     */
    void SetInternalCloseHandler(std::function<void(TcpSessionPtr)> cb) { internal_close_handler_ = std::move(cb); }

private:
    friend class TcpServer;

    /**
     * @brief 内部带完成通知的关闭逻辑，供 TcpServer::Stop() 实现优雅排空与跨线程等待。
     * @param completion 关闭任务在 IO 线程完成后的通知函数
     */
    void Close(std::function<void()> completion);

    /** @brief 发起底层异步读操作（async_read_some） */
    void DoRead();

    /** @brief 发起底层异步写操作（async_write 队列头部的包） */
    void DoWrite();

    /** @brief 重新设置/刷新心跳超时定时器 */
    void ResetHeartbeatTimer();

    /** @brief 统一处理网络错误并执行后续清理 */
    void HandleError(const std::error_code& ec);

private:
    asio::ip::tcp::socket socket_;             ///< 底层 TCP socket 对象
    std::string remote_address_;               ///< 缓存的客户端 IP
    unsigned short remote_port_;               ///< 缓存的客户端 Port
    asio::steady_timer heartbeat_timer_;       ///< 心跳检测定时器
    int heartbeat_timeout_s_;                  ///< 心跳超时秒数
    std::atomic<bool> is_closed_;              ///< 标志位：是否已进入关闭流程（供跨线程快速判断）
    bool close_completed_;                     ///< 标志位：在 IO 线程内标记关闭回调是否已执行，防止重复触发

    std::array<uint8_t, 8192> read_buffer_;    ///< 读缓冲区（每个 Session 独立 8KB）
    std::deque<std::vector<uint8_t>> write_queue_; ///< 待发送的数据包队列（严格在 IO 线程内操作）
    std::size_t max_send_queue_size_{10 * 1024 * 1024}; ///< 最大允许排队的待发送字节数（默认 10MB）
    std::atomic<std::size_t> current_send_queue_size_{0}; ///< 当前发送队列中累积的字节数（原子计数）

    OnMessageHandler on_message_;              ///< 业务数据回调
    OnCloseHandler on_close_;                  ///< 业务关闭回调
    OnErrorHandler on_error_;                  ///< 业务错误回调
    std::function<void(TcpSessionPtr)> internal_close_handler_; ///< 内部容器清理回调
};

} // namespace net

#endif // TCP_SERVER_NET_TCP_SESSION_H_
