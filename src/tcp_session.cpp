#include "net/tcp_session.h"

#include <iostream>
#include <stdexcept>

namespace net {

TcpSession::TcpSession(asio::ip::tcp::socket socket, int heartbeat_timeout_s)
    : socket_(std::move(socket)),
      executor_(socket_.get_executor()),
      io_context_(&static_cast<asio::io_context&>(executor_.context())),
      heartbeat_timer_(executor_),
      heartbeat_timeout_s_(heartbeat_timeout_s),
      state_(State::Open) {
    std::error_code ec;
    const auto ep = socket_.remote_endpoint(ec);
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
    if (state_.load(std::memory_order_acquire) != State::Open) {
        return;
    }
    ResetHeartbeatTimer();
    DoRead();
}

void TcpSession::Send(const uint8_t* data, std::size_t length) {
    if (!data || length == 0 ||
        state_.load(std::memory_order_acquire) != State::Open) {
        return;
    }

    auto self = shared_from_this();
    bool high_watermark_exceeded = false;
    {
        // Close 也持有此锁完成 Open -> Draining，使“关闭前已接受的发送”有明确线性化边界。
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (state_.load(std::memory_order_acquire) != State::Open) {
            return;
        }

        if (!TryReserveSendQueueBytes(length)) {
            high_watermark_exceeded = true;
        } else {
            bool pending_task_registered = false;
            try {
                // 先预留背压额度再拷贝，避免超大单包在被拒绝前造成无约束分配。
                std::vector<uint8_t> buffer(data, data + length);
                auto buffer_holder =
                    std::make_shared<std::vector<uint8_t>>(std::move(buffer));
                pending_send_tasks_.fetch_add(1, std::memory_order_relaxed);
                pending_task_registered = true;
                asio::post(executor_, [this, self, buffer_holder]() {
                    const std::size_t len = buffer_holder->size();
                    const State state = state_.load(std::memory_order_acquire);

                    if (state == State::Closing || state == State::Closed) {
                        current_send_queue_size_.fetch_sub(len, std::memory_order_relaxed);
                    } else {
                        try {
                            write_queue_.push_back(std::move(*buffer_holder));
                        } catch (const std::exception& e) {
                            current_send_queue_size_.fetch_sub(len, std::memory_order_relaxed);
                            std::cerr << "[TcpSession] Failed to enqueue send buffer: "
                                      << e.what() << std::endl;
                            pending_send_tasks_.fetch_sub(1, std::memory_order_relaxed);
                            ForceCloseOnExecutor(false, true);
                            return;
                        } catch (...) {
                            current_send_queue_size_.fetch_sub(len, std::memory_order_relaxed);
                            std::cerr << "[TcpSession] Failed to enqueue send buffer: unknown exception"
                                      << std::endl;
                            pending_send_tasks_.fetch_sub(1, std::memory_order_relaxed);
                            ForceCloseOnExecutor(false, true);
                            return;
                        }

                        if (!write_in_progress_) {
                            DoWrite();
                        }
                    }

                    pending_send_tasks_.fetch_sub(1, std::memory_order_relaxed);
                    if (state_.load(std::memory_order_acquire) == State::Draining) {
                        MaybeFinishDrain();
                    } else if (state_.load(std::memory_order_acquire) == State::Closing) {
                        MaybeFinalizeForcedClose();
                    }
                });
            } catch (...) {
                if (pending_task_registered) {
                    pending_send_tasks_.fetch_sub(1, std::memory_order_relaxed);
                }
                current_send_queue_size_.fetch_sub(length, std::memory_order_relaxed);
                throw;
            }
        }
    }

    if (high_watermark_exceeded) {
        std::cerr << "[TcpSession] Send queue high watermark exceeded, closing connection."
                  << std::endl;
        RequestAsyncClose();
    }
}

void TcpSession::Send(const std::string& message) {
    Send(reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

bool TcpSession::Close(std::chrono::milliseconds timeout) {
    if (RunningInThisIoThread()) {
        throw std::logic_error(
            "TcpSession::Close() cannot wait from the session's IO thread.");
    }

    BeginActiveClose();
    return WaitForClose(timeout);
}

bool TcpSession::IsClosed() const {
    return state_.load(std::memory_order_acquire) != State::Open;
}

void TcpSession::BeginActiveClose() {
    auto self = shared_from_this();
    // 先发布主动关闭标志阻断新回调；已进入的回调由 FinalizeCloseOnExecutor 的门闩等待。
    // Begin 本身不等待回调，使 TcpServer::Stop 能先让所有 Session 并行进入 Draining。
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    active_close_requested_.store(true, std::memory_order_release);
    if (state_.load(std::memory_order_acquire) != State::Open) {
        return;
    }

    {
        std::lock_guard<std::mutex> close_lock(close_mutex_);
        drain_succeeded_ = true;
    }
    state_.store(State::Draining, std::memory_order_release);

    try {
        asio::post(executor_, [this, self]() { BeginDrainOnExecutor(); });
    } catch (...) {
        state_.store(State::Open, std::memory_order_release);
        active_close_requested_.store(false, std::memory_order_release);
        throw;
    }
}

bool TcpSession::WaitForClose(std::chrono::milliseconds timeout) {
    if (!WaitUntilClosed(timeout)) {
        ForceActiveClose();
    }
    return WaitForCloseCompletion();
}

bool TcpSession::WaitUntilClosed(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        timeout = std::chrono::milliseconds(0);
    }

    std::unique_lock<std::mutex> lock(close_mutex_);
    return close_cv_.wait_for(lock, timeout, [this]() {
            return state_.load(std::memory_order_acquire) == State::Closed;
        });
}

void TcpSession::ForceActiveClose() {
    bool should_force = false;
    {
        std::lock_guard<std::mutex> lock(close_mutex_);
        const State state = state_.load(std::memory_order_acquire);
        if (state == State::Closed ||
            (drain_succeeded_ &&
             pending_send_tasks_.load(std::memory_order_acquire) == 0 &&
             current_send_queue_size_.load(std::memory_order_acquire) == 0)) {
            // 发送已经排空，当前只是在等待 IO handler/用户回调形成最终屏障；
            // 该等待不受发送 timeout 限制。
            return;
        }
        should_force = !force_close_posted_;
        force_close_posted_ = true;
        drain_succeeded_ = false;
    }

    if (should_force) {
        auto self = shared_from_this();
        asio::post(executor_, [this, self]() {
            ForceCloseOnExecutor(false, false);
        });
    }
}

bool TcpSession::WaitForCloseCompletion() {
    std::unique_lock<std::mutex> lock(close_mutex_);
    close_cv_.wait(lock, [this]() {
        return state_.load(std::memory_order_acquire) == State::Closed;
    });
    return drain_succeeded_;
}

void TcpSession::RequestAsyncClose() {
    auto self = shared_from_this();
    std::lock_guard<std::mutex> admission_lock(admission_mutex_);
    if (state_.load(std::memory_order_acquire) != State::Open) {
        return;
    }

    state_.store(State::Closing, std::memory_order_release);
    {
        std::lock_guard<std::mutex> close_lock(close_mutex_);
        drain_succeeded_ = false;
    }

    try {
        asio::post(executor_, [this, self]() {
            ForceCloseOnExecutor(false, true);
        });
    } catch (...) {
        state_.store(State::Open, std::memory_order_release);
        throw;
    }
}

void TcpSession::BeginDrainOnExecutor() {
    if (state_.load(std::memory_order_acquire) != State::Draining) {
        return;
    }

    std::error_code ec;
    heartbeat_timer_.cancel(ec);
    // 停止入站消息和 on_message，同时保留发送方向供 write_queue_ 排空。
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_receive, ec);
    }
    MaybeFinishDrain();
}

void TcpSession::MaybeFinishDrain() {
    if (state_.load(std::memory_order_acquire) != State::Draining) {
        return;
    }
    if (pending_send_tasks_.load(std::memory_order_acquire) != 0 ||
        write_in_progress_ || !write_queue_.empty()) {
        return;
    }

    state_.store(State::Closing, std::memory_order_release);
    std::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
    FinalizeCloseOnExecutor();
}

void TcpSession::ForceCloseOnExecutor(bool drained, bool notify_close) {
    const State current = state_.load(std::memory_order_acquire);
    if (current == State::Closed) {
        return;
    }

    if (current == State::Open || current == State::Draining) {
        std::lock_guard<std::mutex> admission_lock(admission_mutex_);
        if (state_.load(std::memory_order_acquire) == State::Open ||
            state_.load(std::memory_order_acquire) == State::Draining) {
            state_.store(State::Closing, std::memory_order_release);
        }
    }

    notify_close_on_finalize_ = notify_close_on_finalize_ || notify_close;
    if (!drained) {
        std::lock_guard<std::mutex> close_lock(close_mutex_);
        drain_succeeded_ = false;
    }

    std::error_code ec;
    heartbeat_timer_.cancel(ec);
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
    MaybeFinalizeForcedClose();
}

void TcpSession::MaybeFinalizeForcedClose() {
    if (state_.load(std::memory_order_acquire) != State::Closing) {
        return;
    }
    if (pending_send_tasks_.load(std::memory_order_acquire) != 0 || write_in_progress_) {
        return;
    }

    ClearWriteQueue();
    FinalizeCloseOnExecutor();
}

void TcpSession::FinalizeCloseOnExecutor() {
    if (state_.load(std::memory_order_acquire) == State::Closed) {
        return;
    }

    auto self = shared_from_this();
    OnCloseHandler on_close;
    std::function<void(TcpSessionPtr)> internal_close =
        std::move(internal_close_handler_);
    internal_close_handler_ = std::function<void(TcpSessionPtr)>();

    {
        // 主动 Close 与全部用户回调在此门闩上线性化；门闩释放后回调对象已全部清空。
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        on_close = std::move(on_close_);
        on_message_ = OnMessageHandler();
        on_error_ = OnErrorHandler();
        on_close_ = OnCloseHandler();

        if (!active_close_requested_.load(std::memory_order_acquire) &&
            notify_close_on_finalize_ && on_close) {
            try {
                on_close(self);
            } catch (const std::exception& e) {
                std::cerr << "[TcpSession] on_close exception: " << e.what()
                          << std::endl;
            } catch (...) {
                std::cerr << "[TcpSession] on_close unknown exception" << std::endl;
            }
        }
    }

    if (internal_close) {
        try {
            internal_close(self);
        } catch (...) {
        }
    }

    // Closed 必须最后发布：同步 Close 观察到它时，用户回调和内部移除均已返回。
    state_.store(State::Closed, std::memory_order_release);
    close_cv_.notify_all();
}

void TcpSession::DoRead() {
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(read_buffer_),
        [this, self](const std::error_code& ec, std::size_t bytes_transferred) {
            if (state_.load(std::memory_order_acquire) != State::Open) {
                return;
            }

            if (!ec) {
                ResetHeartbeatTimer();
                bool callback_failed = false;
                {
                    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
                    if (state_.load(std::memory_order_acquire) == State::Open &&
                        !active_close_requested_.load(std::memory_order_acquire) &&
                        on_message_) {
                        try {
                            on_message_(self, read_buffer_.data(), bytes_transferred);
                        } catch (const std::exception& e) {
                            std::cerr << "[TcpSession] on_message exception: " << e.what()
                                      << std::endl;
                            callback_failed = true;
                        } catch (...) {
                            std::cerr << "[TcpSession] on_message unknown exception"
                                      << std::endl;
                            callback_failed = true;
                        }
                    }
                }

                if (callback_failed) {
                    ForceCloseOnExecutor(false, true);
                    return;
                }

                if (state_.load(std::memory_order_acquire) == State::Open) {
                    DoRead();
                }
            } else {
                HandleError(ec);
            }
        });
}

void TcpSession::DoWrite() {
    if (write_queue_.empty() || write_in_progress_) {
        return;
    }

    auto self = shared_from_this();
    write_in_progress_ = true;
    try {
        asio::async_write(socket_, asio::buffer(write_queue_.front()),
            [this, self](const std::error_code& ec, std::size_t /*bytes_transferred*/) {
                write_in_progress_ = false;

                if (!ec) {
                    current_send_queue_size_.fetch_sub(
                        write_queue_.front().size(), std::memory_order_relaxed);
                    write_queue_.pop_front();

                    const State state = state_.load(std::memory_order_acquire);
                    if (state == State::Open || state == State::Draining) {
                        if (!write_queue_.empty()) {
                            DoWrite();
                        } else if (state == State::Draining) {
                            MaybeFinishDrain();
                        }
                    } else {
                        MaybeFinalizeForcedClose();
                    }
                } else {
                    const State state = state_.load(std::memory_order_acquire);
                    ClearWriteQueue();
                    if (state == State::Open) {
                        HandleError(ec);
                    } else if (state == State::Draining) {
                        ForceCloseOnExecutor(false, false);
                    } else {
                        MaybeFinalizeForcedClose();
                    }
                }
            });
    } catch (const std::exception& e) {
        write_in_progress_ = false;
        ClearWriteQueue();
        std::cerr << "[TcpSession] Failed to initiate async_write: " << e.what()
                  << std::endl;
        ForceCloseOnExecutor(false, true);
    } catch (...) {
        write_in_progress_ = false;
        ClearWriteQueue();
        std::cerr << "[TcpSession] Failed to initiate async_write: unknown exception"
                  << std::endl;
        ForceCloseOnExecutor(false, true);
    }
}

bool TcpSession::TryReserveSendQueueBytes(std::size_t length) {
    std::size_t current = current_send_queue_size_.load(std::memory_order_relaxed);
    for (;;) {
        if (length > max_send_queue_size_ || current > max_send_queue_size_ - length) {
            return false;
        }
        if (current_send_queue_size_.compare_exchange_weak(
                current, current + length,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void TcpSession::ClearWriteQueue() {
    std::size_t bytes_to_release = 0;
    for (const auto& buffer : write_queue_) {
        bytes_to_release += buffer.size();
    }
    write_queue_.clear();
    if (bytes_to_release != 0) {
        current_send_queue_size_.fetch_sub(bytes_to_release, std::memory_order_relaxed);
    }
}

void TcpSession::ResetHeartbeatTimer() {
    if (heartbeat_timeout_s_ <= 0 ||
        state_.load(std::memory_order_acquire) != State::Open) {
        return;
    }

    heartbeat_timer_.expires_after(std::chrono::seconds(heartbeat_timeout_s_));
    auto self = shared_from_this();
    heartbeat_timer_.async_wait([this, self](const std::error_code& ec) {
        if (!ec && state_.load(std::memory_order_acquire) == State::Open) {
            std::cerr << "[TcpSession] Heartbeat timeout from " << remote_address_
                      << ":" << remote_port_ << ", closing." << std::endl;
            ForceCloseOnExecutor(false, true);
        }
    });
}

void TcpSession::HandleError(const std::error_code& ec) {
    if (state_.load(std::memory_order_acquire) != State::Open ||
        ec == asio::error::operation_aborted) {
        return;
    }

    {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        if (state_.load(std::memory_order_acquire) == State::Open &&
            !active_close_requested_.load(std::memory_order_acquire) && on_error_) {
            try {
                on_error_(shared_from_this(), ec);
            } catch (const std::exception& e) {
                std::cerr << "[TcpSession] on_error exception: " << e.what()
                          << std::endl;
            } catch (...) {
                std::cerr << "[TcpSession] on_error unknown exception" << std::endl;
            }
        }
    }
    ForceCloseOnExecutor(false, true);
}

bool TcpSession::RunningInThisIoThread() const {
    return io_context_->get_executor().running_in_this_thread();
}

} // namespace net
