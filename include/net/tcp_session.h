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

class TcpSession;
using TcpSessionPtr = std::shared_ptr<TcpSession>;

using OnMessageHandler = std::function<void(TcpSessionPtr, const uint8_t*, std::size_t)>;
using OnCloseHandler = std::function<void(TcpSessionPtr)>;
using OnErrorHandler = std::function<void(TcpSessionPtr, const std::error_code&)>;

class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    explicit TcpSession(asio::ip::tcp::socket socket, int heartbeat_timeout_s = 60);
    ~TcpSession();

    TcpSession(const TcpSession&) = delete;
    TcpSession& operator=(const TcpSession&) = delete;

    void Start();
    void Send(const uint8_t* data, std::size_t length);
    void Send(const std::string& message);
    void Close();

    asio::ip::tcp::socket& GetSocket() { return socket_; }
    bool IsClosed() const { return is_closed_.load(); }
    std::string GetRemoteAddress() const { return remote_address_; }
    unsigned short GetRemotePort() const { return remote_port_; }

    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }
    void SetOnError(OnErrorHandler cb) { on_error_ = std::move(cb); }
    void SetHeartbeatTimeout(int seconds) { heartbeat_timeout_s_ = seconds; }
    void SetMaxSendQueueSize(std::size_t max_size) { max_send_queue_size_ = max_size; }
    
    // 供 TcpServer 内部使用，用于清理 active_sessions_
    void SetInternalCloseHandler(std::function<void(TcpSessionPtr)> cb) { internal_close_handler_ = std::move(cb); }

private:
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
