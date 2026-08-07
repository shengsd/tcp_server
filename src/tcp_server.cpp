#include "net/tcp_server.h"
#include <iostream>
#include <stdexcept>
#include <future>
#include <atomic>

namespace net {

TcpServer::TcpServer(unsigned short port, std::size_t thread_pool_size, int heartbeat_timeout_s)
    : io_thread_pool_(thread_pool_size),
      acceptor_(main_io_context_),
      heartbeat_timeout_s_(heartbeat_timeout_s),
      is_running_(false) {
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

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

TcpServer::~TcpServer() {
    Stop();
}

void TcpServer::Start() {
    if (is_running_) return;
    is_running_ = true;

    io_thread_pool_.Start();
    DoAccept();

    // 在独立子线程运行 Accept 事件循环，使 Start() 成为非阻塞调用
    acceptor_thread_ = std::thread([this]() {
        try {
            main_io_context_.run();
        } catch (const std::exception& e) {
            std::cerr << "[TcpServer] Main io_context exception: " << e.what() << std::endl;
        }
    });
}

void TcpServer::Stop() {
    if (acceptor_thread_.joinable() && acceptor_thread_.get_id() == std::this_thread::get_id()) {
        throw std::logic_error("TcpServer::Stop() cannot be called from the acceptor thread.");
    }
    if (io_thread_pool_.IsCurrentThread()) {
        throw std::logic_error("TcpServer::Stop() cannot be called from an IO worker thread.");
    }

    if (!is_running_) return;
    is_running_ = false;

    // 先主动关闭所有活跃会话，防止析构期内存泄露或回调崩溃
    std::vector<TcpSessionPtr> sessions_to_close;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_to_close.assign(active_sessions_.begin(), active_sessions_.end());
        // 交由内部回调进行 erase，不直接 clear()
    }

    if (!sessions_to_close.empty()) {
        auto count = std::make_shared<std::atomic<size_t>>(sessions_to_close.size());
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();

        for (auto& session : sessions_to_close) {
            // 完成通知由 session 的真实关闭任务末尾触发，不依赖跨线程投递顺序。
            session->Close([count, promise]() {
                if (count->fetch_sub(1, std::memory_order_relaxed) == 1) {
                    promise->set_value();
                }
            });
        }
        // 这是关闭任务的排空宽限期，不是 Stop() 的硬超时：已经开始执行的
        // 用户回调无法被安全抢占，后续 join 仍会等待它们自行返回。
        if (future.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
            std::cerr << "[TcpServer] Timed out while draining session close callbacks; forcing IO contexts to stop."
                      << std::endl;
        }
    }

    std::error_code ec;
    acceptor_.close(ec);
    main_io_context_.stop();
    io_thread_pool_.Stop();

    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }
}

void TcpServer::DoAccept() {
    auto& target_io_ctx = io_thread_pool_.GetNextIOContext();
    auto socket_ptr = std::make_shared<asio::ip::tcp::socket>(target_io_ctx);

    acceptor_.async_accept(*socket_ptr, [this, socket_ptr](const std::error_code& ec) {
        if (!is_running_) return;

        if (!ec) {
            auto session = std::make_shared<TcpSession>(std::move(*socket_ptr), heartbeat_timeout_s_);
            session->SetMaxSendQueueSize(max_send_queue_size_);

            if (on_message_) session->SetOnMessage(on_message_);
            if (on_close_) session->SetOnClose(on_close_);
            if (on_error_) session->SetOnError(on_error_);

            // 设置内部清理回调
            session->SetInternalCloseHandler([this](TcpSessionPtr s) {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                active_sessions_.erase(s);
            });

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                if (!is_running_) {
                    // Stop 正在并发执行，服务器即将销毁，拒绝接管新会话
                    return; 
                }
                active_sessions_.insert(session);
            }

            if (on_connect_) {
                try {
                    on_connect_(session);
                } catch (const std::exception& e) {
                    std::cerr << "[TcpServer] on_connect exception: " << e.what() << std::endl;
                    session->Close();
                } catch (...) {
                    std::cerr << "[TcpServer] on_connect unknown exception" << std::endl;
                    session->Close();
                }
            }

            // 将启动逻辑投递至其归属的 IO 线程，彻底封死极短时间窗内的读写与销毁竞态
            asio::post(session->GetSocket().get_executor(), [session]() {
                if (!session->IsClosed()) {
                    session->Start();
                }
            });
        } else if (ec != asio::error::operation_aborted) {
            std::cerr << "[TcpServer] Accept error: " << ec.message() << std::endl;
        }

        if (is_running_) {
            DoAccept();
        }
    });
}

} // namespace net
