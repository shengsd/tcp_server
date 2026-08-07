#include "net/tcp_session.h"
#include <iostream>

namespace net {

TcpSession::TcpSession(asio::ip::tcp::socket socket, int heartbeat_timeout_s)
    : socket_(std::move(socket)),
      heartbeat_timer_(socket_.get_executor()),
      heartbeat_timeout_s_(heartbeat_timeout_s),
      is_closed_(false) {
    std::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (!ec) {
        remote_address_ = ep.address().to_string();
        remote_port_ = ep.port();
    }
}

TcpSession::~TcpSession() {
    std::error_code ec;
    heartbeat_timer_.cancel(ec);
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

void TcpSession::Start() {
    ResetHeartbeatTimer();
    DoRead();
}

void TcpSession::Send(const uint8_t* data, std::size_t length) {
    if (is_closed_ || !data || length == 0) return;

    if (current_send_queue_size_.load(std::memory_order_relaxed) + length > max_send_queue_size_) {
        std::cerr << "[TcpSession] Send queue high watermark exceeded, closing connection." << std::endl;
        Close();
        return;
    }
    
    current_send_queue_size_.fetch_add(length, std::memory_order_relaxed);

    auto self(shared_from_this());
    std::vector<uint8_t> buffer(data, data + length);

    struct SendTask {
        TcpSession* session;
        TcpSessionPtr self;
        std::vector<uint8_t> buffer;

        void operator()() {
            if (session->is_closed_) return;
            bool write_in_progress = !session->write_queue_.empty();
            session->write_queue_.push_back(std::move(buffer));
            if (!write_in_progress) {
                session->DoWrite();
            }
        }
    };

    asio::post(socket_.get_executor(), SendTask{this, self, std::move(buffer)});
}

void TcpSession::Send(const std::string& message) {
    Send(reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

void TcpSession::Close() {
    bool expected = false;
    if (!is_closed_.compare_exchange_strong(expected, true)) {
        return; // 已经关闭
    }

    std::error_code ec;
    heartbeat_timer_.cancel(ec);

    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    if (on_close_) {
        try {
            on_close_(shared_from_this());
        } catch (const std::exception& e) {
            std::cerr << "[TcpSession] on_close exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[TcpSession] on_close unknown exception" << std::endl;
        }
    }

    if (internal_close_handler_) {
        try {
            internal_close_handler_(shared_from_this());
        } catch (...) {}
    }
}



void TcpSession::DoRead() {
    auto self(shared_from_this());
    socket_.async_read_some(asio::buffer(read_buffer_),
        [this, self](const std::error_code& ec, std::size_t bytes_transferred) {
            if (is_closed_) return;

            if (!ec) {
                ResetHeartbeatTimer();
                if (on_message_) {
                    try {
                        on_message_(self, read_buffer_.data(), bytes_transferred);
                    } catch (const std::exception& e) {
                        std::cerr << "[TcpSession] on_message exception: " << e.what() << std::endl;
                        HandleError(asio::error::make_error_code(asio::error::operation_aborted));
                        return;
                    } catch (...) {
                        std::cerr << "[TcpSession] on_message unknown exception" << std::endl;
                        HandleError(asio::error::make_error_code(asio::error::operation_aborted));
                        return;
                    }
                }
                DoRead();
            } else {
                HandleError(ec);
            }
        });
}

void TcpSession::DoWrite() {
    auto self(shared_from_this());
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
        [this, self](const std::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (is_closed_) return;

            if (!ec) {
                current_send_queue_size_.fetch_sub(write_queue_.front().size(), std::memory_order_relaxed);
                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    DoWrite();
                }
            } else {
                HandleError(ec);
            }
        });
}

void TcpSession::ResetHeartbeatTimer() {
    if (heartbeat_timeout_s_ <= 0) return;

    heartbeat_timer_.expires_after(std::chrono::seconds(heartbeat_timeout_s_));

    auto self(shared_from_this());
    heartbeat_timer_.async_wait([this, self](const std::error_code& ec) {
        if (!ec) {
            // 超时未收到数据，断开连接
            Close();
        }
    });
}

void TcpSession::HandleError(const std::error_code& ec) {
    if (ec == asio::error::operation_aborted) {
        return; // 取消操作，正常退出
    }

    if (on_error_) {
        try {
            on_error_(shared_from_this(), ec);
        } catch (const std::exception& e) {
            std::cerr << "[TcpSession] on_error exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[TcpSession] on_error unknown exception" << std::endl;
        }
    }
    Close();
}

} // namespace net
