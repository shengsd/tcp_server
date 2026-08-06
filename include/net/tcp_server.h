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

namespace net {

using OnConnectHandler = std::function<void(TcpSessionPtr)>;

class TcpServer {
public:
    TcpServer(unsigned short port, 
              std::size_t thread_pool_size = std::thread::hardware_concurrency(),
              int heartbeat_timeout_s = 60);
    TcpServer(const std::string& address,
              unsigned short port,
              std::size_t thread_pool_size = std::thread::hardware_concurrency(),
              int heartbeat_timeout_s = 60);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void Start();
    void Stop();

    void SetOnConnect(OnConnectHandler cb) { on_connect_ = std::move(cb); }
    void SetOnMessage(OnMessageHandler cb) { on_message_ = std::move(cb); }
    void SetOnClose(OnCloseHandler cb) { on_close_ = std::move(cb); }
    void SetOnError(OnErrorHandler cb) { on_error_ = std::move(cb); }

private:
    void DoAccept();

private:
    asio::io_context main_io_context_;
    IOThreadPool io_thread_pool_;
    asio::ip::tcp::acceptor acceptor_;
    int heartbeat_timeout_s_;
    std::atomic<bool> is_running_;
    std::thread acceptor_thread_;

    OnConnectHandler on_connect_;
    OnMessageHandler on_message_;
    OnCloseHandler on_close_;
    OnErrorHandler on_error_;
};

} // namespace net

#endif // TCP_SERVER_NET_TCP_SERVER_H_
