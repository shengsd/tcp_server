#include "net/tcp_server.h"
#include <iostream>
#include <stdexcept>
#include <future>
#include <atomic>

namespace net {

// 构造函数：基于端口监听所有 IPv4 地址
TcpServer::TcpServer(unsigned short port, std::size_t thread_pool_size, int heartbeat_timeout_s)
    : io_thread_pool_(thread_pool_size),
      acceptor_(main_io_context_),
      heartbeat_timeout_s_(heartbeat_timeout_s),
      is_running_(false) {
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
    // 显式执行 open -> set_option(reuse_address) -> bind -> listen 步骤
    acceptor_.open(endpoint.protocol());
    // SO_REUSEADDR 允许快速重启服务而无需等待 TCP TIME_WAIT 状态结束
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

// 构造函数：基于指定 IP 地址和端口监听
TcpServer::TcpServer(const std::string& address, unsigned short port, std::size_t thread_pool_size, int heartbeat_timeout_s)
    : io_thread_pool_(thread_pool_size),
      acceptor_(main_io_context_),
      heartbeat_timeout_s_(heartbeat_timeout_s),
      is_running_(false) {
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(address), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

// 析构函数：确保对象销毁时自动执行 Stop()
TcpServer::~TcpServer() {
    Stop();
}

// 启动服务端（非阻塞调用）
void TcpServer::Start() {
    if (is_running_) return; // 避免重复启动
    is_running_ = true;

    // 1. 启动从 Reactor 线程池（各 IO 线程进入 run() 循环）
    io_thread_pool_.Start();

    // 2. 在主 io_context 注册第一个异步 accept 监听事件
    DoAccept();

    // 3. 在独立的子线程中运行主 Acceptor 事件循环，使 Start() 成为即刻返回的非阻塞调用
    acceptor_thread_ = std::thread([this]() {
        main_io_context_.run();
    });
}

// 停止服务端并优雅排空现有会话
void TcpServer::Stop() {
    // 【防死锁校验】：禁止从 Acceptor 线程或 IO 工作线程中调用 Stop()，否则自身 join 会导致永久死锁
    if (acceptor_thread_.joinable() && acceptor_thread_.get_id() == std::this_thread::get_id()) {
        throw std::logic_error("TcpServer::Stop() cannot be called from the acceptor thread.");
    }
    if (io_thread_pool_.IsCurrentThread()) {
        throw std::logic_error("TcpServer::Stop() cannot be called from an IO worker thread.");
    }

    if (!is_running_) return;
    is_running_ = false;

    // 1. 复制当前所有活跃会话快照（加锁保护）
    std::vector<TcpSessionPtr> sessions_to_close;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_to_close.assign(active_sessions_.begin(), active_sessions_.end());
        // 注意：不在这里直接 clear() active_sessions_，而是由各会话完成关闭回调后自行 erase
    }

    // 2. 优雅排空机制：向所有会话投递异步 Close，并设置 3 秒宽限期等待未竟数据收尾
    if (!sessions_to_close.empty()) {
        auto count = std::make_shared<std::atomic<size_t>>(sessions_to_close.size());
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();

        for (auto& session : sessions_to_close) {
            // 异步关闭完成通知：当所有 Session 的 Close 任务都在所属 IO 线程执行完成后递减原子计数
            session->Close([count, promise]() {
                if (count->fetch_sub(1, std::memory_order_relaxed) == 1) {
                    promise->set_value(); // 所有连接均已完成关闭
                }
            });
        }

        // 等待所有连接优雅关闭，最多等待 3 秒宽限期
        if (future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
            std::cerr << "[TcpServer] Timed out while draining session close callbacks; forcing IO contexts to stop."
                      << std::endl;
        }
    }

    // 3. 关闭 Acceptor，拒绝新的客户端接入
    std::error_code ec;
    acceptor_.close(ec);

    // 4. 停止主 Acceptor 上下文与工作线程池
    main_io_context_.stop();
    io_thread_pool_.Stop();

    // 5. 等待 Acceptor 独立子线程退出
    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }
}

// 异步接收新连接状态机
void TcpServer::DoAccept() {
    // 1. 负载均衡：从线程池中轮询获取下一个目标 IO 线程的 io_context
    auto& target_io_ctx = io_thread_pool_.GetNextIOContext();
    // 2. 创建 socket 并直接绑定到目标 IO 线程的 executor 上
    auto socket_ptr = std::make_shared<asio::ip::tcp::socket>(target_io_ctx);

    // 3. 在主 Acceptor 上注册异步接收
    acceptor_.async_accept(*socket_ptr, [this, socket_ptr](const std::error_code& ec) {
        if (!is_running_) return;

        if (!ec) {
            // 客户端连接成功建立：将底层 socket 移动所有权给新的 TcpSession 实例
            auto session = std::make_shared<TcpSession>(std::move(*socket_ptr), heartbeat_timeout_s_);
            session->SetMaxSendQueueSize(max_send_queue_size_);

            // 将全局配置的回调函数传递给新会话
            if (on_message_) session->SetOnMessage(on_message_);
            if (on_close_) session->SetOnClose(on_close_);
            if (on_error_) session->SetOnError(on_error_);

            // 设置内部清理钩子：连接断开时自动从 active_sessions_ 集合中剔除
            session->SetInternalCloseHandler([this](TcpSessionPtr s) {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                active_sessions_.erase(s);
            });

            // 注册到全局活跃会话表
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                if (!is_running_) {
                    // 如果在 accept 期间服务器被并发 Stop()，拒绝接管新会话
                    return; 
                }
                active_sessions_.insert(session);
            }

            // 触发用户的连接建立通知回调
            if (on_connect_) {
                on_connect_(session);
            }

            // 【关键跨线程投递】：将 session->Start() 投递至其归属的 IO 线程执行，
            // 确保后续所有的读写和定时器操作都在同一个专属线程中串行展开
            asio::post(session->GetExecutor(), [session]() {
                if (!session->IsClosed()) {
                    session->Start();
                }
            });
        } else if (ec != asio::error::operation_aborted) {
            std::cerr << "[TcpServer] Accept error: " << ec.message() << std::endl;
        }

        // 递归（挂起）下一次异步 Accept 监听
        if (is_running_) {
            DoAccept();
        }
    });
}

} // namespace net
