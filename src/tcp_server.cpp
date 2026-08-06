#include "net/tcp_server.h"
#include <iostream>

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

    // 运行主事件循环 (负责接收连接)
    try {
        main_io_context_.run();
    } catch (const std::exception& e) {
        std::cerr << "[TcpServer] Main io_context exception: " << e.what() << std::endl;
    }
}

void TcpServer::Stop() {
    if (!is_running_) return;
    is_running_ = false;

    std::error_code ec;
    acceptor_.close(ec);
    main_io_context_.stop();
    io_thread_pool_.Stop();
}

void TcpServer::DoAccept() {
    auto& target_io_ctx = io_thread_pool_.GetNextIOContext();
    auto socket_ptr = std::make_shared<asio::ip::tcp::socket>(target_io_ctx);

    acceptor_.async_accept(*socket_ptr, [this, socket_ptr](const std::error_code& ec) {
        if (!is_running_) return;

        if (!ec) {
            auto session = std::make_shared<TcpSession>(std::move(*socket_ptr), heartbeat_timeout_s_);

            if (on_message_) session->SetOnMessage(on_message_);
            if (on_close_) session->SetOnClose(on_close_);
            if (on_error_) session->SetOnError(on_error_);

            if (on_connect_) {
                on_connect_(session);
            }

            session->Start();
        } else if (ec != asio::error::operation_aborted) {
            std::cerr << "[TcpServer] Accept error: " << ec.message() << std::endl;
        }

        if (is_running_) {
            DoAccept();
        }
    });
}

} // namespace net
