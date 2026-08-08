#include "net/tcp_server.h"
#include <iostream>
#include <future>
#include <thread>
#include <asio.hpp>

// 全局 promise 用于在收到退出信号（Ctrl+C / SIGINT）时通知主线程退出
static std::promise<void> g_exit_promise;

int main() {
    // 1. 信号处理：启动一个专用的 IO context 处理系统信号，保证 async-signal-safety
    asio::io_context signal_ctx;
    asio::signal_set signals(signal_ctx, SIGINT, SIGTERM);
    
    unsigned short port = 8888;
    int heartbeat_timeout_s = 15; // 15秒无数据包自动切断空闲连接

    std::cout << "Starting TCP Server on port " << port << "..." << std::endl;
    std::cout << "Heartbeat timeout set to " << heartbeat_timeout_s << " seconds." << std::endl;

    // 2. 初始化 TcpServer 实例（配置 4 个 IO 工作线程）
    net::TcpServer server(port, 4 /* 4个IO线程 */, heartbeat_timeout_s);

    // 异步等待 Ctrl+C (SIGINT) 或 SIGTERM 信号
    signals.async_wait([&](const std::error_code&, int signal_number) {
        std::cout << "\n[Main] Signal " << signal_number << " received, stopping server..." << std::endl;
        try {
            g_exit_promise.set_value(); // 唤醒主线程的 wait()
        } catch (...) {}
    });
    
    // 在独立线程运行信号事件循环
    std::thread sig_thread([&]() {
        signal_ctx.run();
    });

    // 3. 配置应用层回调函数

    // (1) 新客户端连接建立回调
    server.SetOnConnect([](net::TcpSessionPtr session) {
        std::cout << "[Server] Client connected: " 
                  << session->GetRemoteAddress() << ":" << session->GetRemotePort() << std::endl;
        
        // 主动向客户端发送欢迎消息
        std::string welcome_msg = "Welcome to C++11 TCP Server!\n";
        session->Send(welcome_msg);
    });

    // (2) 消息接收回调 (此处实现 Echo 回显服务)
    server.SetOnMessage([](net::TcpSessionPtr session, const uint8_t* data, std::size_t length) {
        std::string msg(reinterpret_cast<const char*>(data), length);
        std::cout << "[Server] Received " << length << " bytes from "
                  << session->GetRemoteAddress() << ":" << session->GetRemotePort()
                  << " -> " << msg;

        // 回显数据给客户端（Send 为线程安全异步发送）
        std::string reply = "[Echo] " + msg;
        session->Send(reply);
    });

    // (3) 客户端连接断开回调
    server.SetOnClose([](net::TcpSessionPtr session) {
        std::cout << "[Server] Client disconnected: " 
                  << session->GetRemoteAddress() << ":" << session->GetRemotePort() << std::endl;
    });

    // (4) 异常网络错误回调
    server.SetOnError([](net::TcpSessionPtr session, const std::error_code& ec) {
        std::cout << "[Server] Session error (" << session->GetRemoteAddress() 
                  << "): " << ec.message() << std::endl;
    });

    // 4. 启动 TCP 服务器 (非阻塞调用，函数立即返回)
    server.Start();
    std::cout << "[Main] server.Start() called successfully, non-blocking return." << std::endl;

    // 5. 主线程阻塞等待退出信号（等待 Ctrl+C）
    g_exit_promise.get_future().wait();

    // 6. 优雅停止服务器（断开所有客户端、排空队列并等待线程池退出）
    server.Stop();
    std::cout << "[Main] Server stopped gracefully." << std::endl;

    // 7. 清理信号监听线程
    signal_ctx.stop();
    if (sig_thread.joinable()) {
        sig_thread.join();
    }

    return 0;
}
