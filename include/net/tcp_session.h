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
#include <chrono>
#include <condition_variable>
#include <mutex>

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
 * 2. 线程模型与串行化：每个 Session 在创建时绑定到一个特定的 IO 线程（io_context）。Send() 可从任意
 *    业务线程调用；同步 Close() 只能从 Session IO 线程以外调用。底层 socket 读写、发送队列操作与
 *    定时器重置全部通过 asio::post 排队在绑定的 executor 中串行执行。
 *    注意：当前正确性依赖“One io_context 只有一个 run 线程”。若后续改为多线程 run 同一个 io_context，
 *    则必须引入 explicit asio::strand 来保证串行。
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
     * - 无全局顺序保证：由于多线程调用 asio::post 进入队列的顺序不可控，并发调用时无法保证先调用的必然先发送。
     *   若业务强依赖顺序，上层需在业务层增加序号或在单一业务线程中投递。
     * - 内存语义：函数内部会立即深拷贝 data[0, length) 到缓冲区中，函数返回后调用方可立即释放原数据。
     * - 发送队列与背压：如果当前有未完成的写操作，数据将存入 write_queue_；如果当前排队总字节数超过
     *   max_send_queue_size_，将拒绝发送并异步切断连接防 OOM。
     */
    void Send(const uint8_t* data, std::size_t length);

    /**
     * @brief 异步发送字符串（重载便捷方法）。
     * @param message 待发送的字符串
     */
    void Send(const std::string& message);

    /**
     * @brief 同步排空并关闭连接，为上层业务对象销毁建立回调屏障。
     * @param timeout 等待已接受发送完成的最长时间；超时后强制关闭并丢弃余量。
     * @return true 表示已接受发送全部完成后关闭；false 表示超时或网络错误导致强制关闭。
     * @note 线程安全，但严禁从本 Session 所属 IO 线程调用，否则抛出 std::logic_error。
     *       返回前会等待当前用户回调退出，清空所有用户回调；返回后本 Session 不再触发用户回调。
     *       timeout 仅限制发送排空，不能中断一个已经执行且尚未返回的用户回调。
     */
    bool Close(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

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
    bool IsClosed() const;

    /** @brief 获取对端客户端的 IP 地址字符串（如 "127.0.0.1"） */
    std::string GetRemoteAddress() const { return remote_address_; }

    /** @brief 获取对端客户端的连接端口号 */
    unsigned short GetRemotePort() const { return remote_port_; }

    // ================= 回调与参数配置（必须在 Start() 前完成） ================= //

    /** @brief 设置接收到应用层数据时的回调函数 */
    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }

    /** @brief 设置连接彻底关闭时的回调函数；主动同步 Close 时会被抑制 */
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }

    /** @brief 设置发生网络 I/O 错误时的回调函数；主动同步 Close 时会被抑制 */
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

    using Executor = asio::ip::tcp::socket::executor_type;

    enum class State {
        Open,
        Draining,
        Closing,
        Closed
    };

    /** @brief 发起主动排空关闭但不等待，供公开 Close 与 TcpServer::Stop 使用 */
    void BeginActiveClose();

    /** @brief 等待关闭；排空超时后投递强制关闭，最终等待完整回调屏障 */
    bool WaitForClose(std::chrono::milliseconds timeout);

    /** @brief 仅限时等待 Closed，不触发强关（供 TcpServer 共享截止时间） */
    bool WaitUntilClosed(std::chrono::milliseconds timeout);

    /** @brief 投递一次主动强关；幂等 */
    void ForceActiveClose();

    /** @brief 无超时等待完整回调屏障，并返回统一排空结果 */
    bool WaitForCloseCompletion();

    /** @brief 框架内部异步关闭入口，不阻塞调用线程；正常保留 on_close 语义 */
    void RequestAsyncClose();

    /** @brief 在 Session executor 中开始主动排空 */
    void BeginDrainOnExecutor();

    /** @brief 在 Session executor 中检查主动排空是否完成 */
    void MaybeFinishDrain();

    /** @brief 在 Session executor 中强制关闭；false 表示排空失败 */
    void ForceCloseOnExecutor(bool drained, bool notify_close);

    /** @brief 强制关闭后，等待发送 handler/任务释放 buffer，再完成最终收尾 */
    void MaybeFinalizeForcedClose();

    /** @brief 执行最终业务回调清理、内部移除及同步等待者通知 */
    void FinalizeCloseOnExecutor();

    /** @brief 发起底层异步读操作（async_read_some） */
    void DoRead();

    /** @brief 发起底层异步写操作（async_write 队列头部的包） */
    void DoWrite();

    /** @brief 重新设置/刷新心跳超时定时器 */
    void ResetHeartbeatTimer();

    /** @brief 统一处理网络错误并执行后续清理 */
    void HandleError(const std::error_code& ec);

    /** @brief 原子预留发送队列额度；返回 false 表示会超过高水位 */
    bool TryReserveSendQueueBytes(std::size_t length);

    /** @brief 在 IO 线程内清空发送队列，并同步回收对应的背压计数 */
    void ClearWriteQueue();

    /** @brief 返回构造时缓存的 executor，避免外部线程访问 socket 获取 executor */
    const Executor& GetExecutor() const { return executor_; }

    /** @brief 判断当前线程是否正在运行本 Session 的 io_context */
    bool RunningInThisIoThread() const;

private:
    asio::ip::tcp::socket socket_;             ///< 底层 TCP socket 对象
    Executor executor_;                        ///< 构造时缓存，供任意线程安全地投递 Session 任务
    asio::io_context* io_context_;              ///< executor 所属上下文，用于同步 Close 的死锁检测
    std::string remote_address_;               ///< 缓存的客户端 IP
    unsigned short remote_port_{0};            ///< 缓存的客户端 Port
    asio::steady_timer heartbeat_timer_;       ///< 心跳检测定时器
    int heartbeat_timeout_s_;                  ///< 心跳超时秒数
    std::atomic<State> state_;                  ///< Open/Draining/Closing/Closed 生命周期状态
    std::atomic<bool> active_close_requested_{false}; ///< 主动关闭时抑制全部业务回调

    std::array<uint8_t, 8192> read_buffer_;    ///< 读缓冲区（每个 Session 独立 8KB）
    std::deque<std::vector<uint8_t>> write_queue_; ///< 待发送的数据包队列（严格在 IO 线程内操作）
    std::size_t max_send_queue_size_{10 * 1024 * 1024}; ///< 最大允许排队的待发送字节数（默认 10MB）
    std::atomic<std::size_t> current_send_queue_size_{0}; ///< 当前发送队列中累积的字节数（原子计数）
    std::atomic<std::size_t> pending_send_tasks_{0}; ///< 已接受但尚未在 IO 线程完成入队/回滚的任务数
    bool write_in_progress_{false};             ///< 仅由 Session IO 线程访问
    bool notify_close_on_finalize_{false};      ///< 内部断开是否需要触发业务 on_close

    std::mutex admission_mutex_;                ///< 原子化 Send 接受与 Open->Draining/Closing 转换
    std::mutex callback_mutex_;                 ///< 串行化业务回调与主动关闭线性化边界
    std::mutex close_mutex_;                    ///< 保护同步关闭结果与条件变量
    std::condition_variable close_cv_;
    bool force_close_posted_{false};
    bool drain_succeeded_{false};

    OnMessageHandler on_message_;              ///< 业务数据回调
    OnCloseHandler on_close_;                  ///< 业务关闭回调
    OnErrorHandler on_error_;                  ///< 业务错误回调
    std::function<void(TcpSessionPtr)> internal_close_handler_; ///< 内部容器清理回调
};

} // namespace net

#endif // TCP_SERVER_NET_TCP_SESSION_H_
