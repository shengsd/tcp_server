#include "net/tcp_server.h"
#include <iostream>
#include <future>
#include <thread>
#include <asio.hpp>

static std::promise<void> g_exit_promise;

int main() {
    // 启动一个专用的 IO context 处理信号，保证 async-signal-safety
    asio::io_context signal_ctx;
    asio::signal_set signals(signal_ctx, SIGINT, SIGTERM);
    
    unsigned short port = 8888;
    int heartbeat_timeout_s = 15; // 15秒无数据包自动切断空闲连接

    std::cout << "Starting TCP Server on port " << port << "..." << std::endl;
    std::cout << "Heartbeat timeout set to " << heartbeat_timeout_s << " seconds." << std::endl;

    net::TcpServer server(port, 4 /* 4个IO线程 */, heartbeat_timeout_s);

    signals.async_wait([&](const std::error_code&, int signal_number) {
        std::cout << "\n[Main] Signal " << signal_number << " received, stopping server..." << std::endl;
        try {
            g_exit_promise.set_value();
        } catch (...) {}
    });
    
    std::thread sig_thread([&]() {
        signal_ctx.run();
    });

    // 1. 新连接建立回调
    server.SetOnConnect([](net::TcpSessionPtr session) {
        std::cout << "[Server] Client connected: " 
                  << session->GetRemoteAddress() << ":" << session->GetRemotePort() << std::endl;
        
        std::string welcome_msg = "Welcome to C++11 TCP Server!\n";
        session->Send(welcome_msg);
    });

    // 2. 消息接收回调 (Echo 服务演示)
    server.SetOnMessage([](net::TcpSessionPtr session, const uint8_t* data, std::size_t length) {
        std::string msg(reinterpret_cast<const char*>(data), length);
        std::cout << "[Server] Received " << length << " bytes from "
                  << session->GetRemoteAddress() << ":" << session->GetRemotePort()
                  << " -> " << msg;

        // 回显数据给客户端
        std::string reply = "[Echo] " + msg;
        session->Send(reply);
    });

    // 3. 连接断开回调
    server.SetOnClose([](net::TcpSessionPtr session) {
        std::cout << "[Server] Client disconnected: " 
                  << session->GetRemoteAddress() << ":" << session->GetRemotePort() << std::endl;
    });

    // 4. 异常错误回调
    server.SetOnError([](net::TcpSessionPtr session, const std::error_code& ec) {
        std::cout << "[Server] Session error (" << session->GetRemoteAddress() 
                  << "): " << ec.message() << std::endl;
    });

    // 启动 TCP 服务器 (非阻塞调用，函数立即返回)
    server.Start();
    std::cout << "[Main] server.Start() called successfully, non-blocking return." << std::endl;

    // 主线程等待退出信号
    g_exit_promise.get_future().wait();

    // 优雅停止服务器
    server.Stop();
    std::cout << "[Main] Server stopped gracefully." << std::endl;

    signal_ctx.stop();
    if (sig_thread.joinable()) {
        sig_thread.join();
    }

    return 0;
}
