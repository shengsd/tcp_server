#include "net/tcp_session.h"
#include <iostream>

namespace net {

// 构造函数：接管已建立的 TCP socket，并缓存对端地址/端口信息
TcpSession::TcpSession(asio::ip::tcp::socket socket, int heartbeat_timeout_s)
    : socket_(std::move(socket)),
      executor_(socket_.get_executor()),
      heartbeat_timer_(executor_), // 定时器与 socket 绑定在相同的 executor（同一线程）
      heartbeat_timeout_s_(heartbeat_timeout_s),
      is_closed_(false),
      close_completed_(false) {
    std::error_code ec;
    // 在构造时主动获取并缓存对端 endpoint，即使后续 socket 关闭，地址信息也不会丢失
    auto ep = socket_.remote_endpoint(ec);
    if (!ec) {
        remote_address_ = ep.address().to_string();
        remote_port_ = ep.port();
    }
}

// 析构函数：释放底层系统资源
TcpSession::~TcpSession() {
    std::error_code ec;
    heartbeat_timer_.cancel(ec); // 取消定时器
    if (socket_.is_open()) {
        // shutdown 发送 FIN 包并关闭连接通道，避免连接处于 TIME_WAIT/半开悬垂状态
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}

// 启动会话：开启心跳检测并挂起第一次异步读
void TcpSession::Start() {
    ResetHeartbeatTimer();
    DoRead();
}

// 异步发送数据（支持多线程并发调用）
void TcpSession::Send(const uint8_t* data, std::size_t length) {
    // 快速前置判断：如果已关闭或数据为空则直接返回
    if (is_closed_ || !data || length == 0) return;

    // 在修改背压计数前确认对象确实由 shared_ptr 管理，避免 shared_from_this()
    // 因误用抛出 bad_weak_ptr 时留下无法回收的预留额度。
    auto self(shared_from_this());

    // 1. 发送背压（Backpressure）与防 OOM 控制：
    // 使用 fetch_add 原子的增加待发送队列字节总数，即使多线程并发调用 Send 也绝对准确
    size_t old_size = current_send_queue_size_.fetch_add(length, std::memory_order_relaxed);
    if (old_size + length > max_send_queue_size_) {
        // 超过高水位阈值（如 10MB）：说明客户端处理过慢/恶意不读取，产生积压。
        // 回滚计数并主动切断连接，保全服务端内存不被耗尽
        current_send_queue_size_.fetch_sub(length, std::memory_order_relaxed);
        std::cerr << "[TcpSession] Send queue high watermark exceeded, closing connection." << std::endl;
        Close();
        return;
    }

    // 深拷贝数据，使得调用方函数返回后可立即释放原数据缓冲区
    std::vector<uint8_t> buffer(data, data + length);

    // 3. 将发送任务投递至该 Session 独占的 IO 线程 executor 中串行执行
    struct SendTask {
        TcpSession* session;
        TcpSessionPtr self;
        std::vector<uint8_t> buffer;

        void operator()() {
            // 如果连接在排队期间已被关闭，直接丢弃并回滚计数
            if (session->is_closed_) {
                session->current_send_queue_size_.fetch_sub(buffer.size(), std::memory_order_relaxed);
                return;
            }

            // 检查当前是否已经有 async_write 正在进行中：
            // 如果 write_queue_ 为空，说明当前没有写操作正在进行，塞入队列后需要立即触发 DoWrite()；
            // 如果 write_queue_ 非空，说明已有底层 async_write 在飞，底层写完后会自动触发链式写入，这里仅入队即可。
            bool write_in_progress = !session->write_queue_.empty();
            session->write_queue_.push_back(std::move(buffer));

            if (!write_in_progress) {
                session->DoWrite();
            }
        }
    };

    asio::post(executor_, SendTask{this, self, std::move(buffer)});
}

// 重载便捷方法：发送 string
void TcpSession::Send(const std::string& message) {
    Send(reinterpret_cast<const uint8_t*>(message.data()), message.size());
}

// 公开的 Close 方法
void TcpSession::Close() {
    Close(std::function<void()>());
}

// 内部核心关闭实现：支持外部传入完成回调（用于 TcpServer::Stop 优雅排空等待）
void TcpSession::Close(std::function<void()> completion) {
    // 第一步：原子标记快速拦截后续的所有业务 Send / Read 请求
    is_closed_.store(true, std::memory_order_release);

    auto self(shared_from_this());
    // 第二步：将真正的 socket/timer 关闭操作投递至绑定的 IO 线程，彻底消除与读写 handler 的并发竞态
    asio::post(executor_, [this, self, completion]() {
        // 使用 close_completed_ 标志位保证关闭逻辑在 IO 线程内【严格只执行一次】
        if (!close_completed_) {
            close_completed_ = true;

            std::error_code ec;
            // 取消心跳定时器（会立即触发 timer 的 async_wait 回调，错误码为 operation_aborted）
            heartbeat_timer_.cancel(ec);

            // 关闭 TCP 套接字
            if (socket_.is_open()) {
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                socket_.close(ec);
            }

            // 触发用户的 on_close 回调
            if (on_close_) {
                on_close_(self);
            }

            // 触发服务器内部的容器剔除回调（从 active_sessions_ 中 erase 自己）
            if (internal_close_handler_) {
                internal_close_handler_(self);
            }
        }

        // 如果调用方传递了完成通知（如 TcpServer::Stop 中的 promise），在此处唤醒等待线程
        if (completion) {
            completion();
        }
    });
}

// 异步读循环状态机
void TcpSession::DoRead() {
    // 捕获 self 保持当前异步调用期间 Session 对象存活
    auto self(shared_from_this());
    socket_.async_read_some(asio::buffer(read_buffer_),
        [this, self](const std::error_code& ec, std::size_t bytes_transferred) {
            // 如果连接已关闭，直接中断循环
            if (is_closed_) return;

            if (!ec) {
                // 收到新数据，重置心跳超时计时器
                ResetHeartbeatTimer();

                // 将数据分发给业务层的 on_message 回调
                if (on_message_) {
                    on_message_(self, read_buffer_.data(), bytes_transferred);
                }
                // 递归（挂起）下一次异步读
                DoRead();
            } else {
                // 发生错误（如对端正常断开 EOF 或网络中断），进入统一错误处理
                HandleError(ec);
            }
        });
}

// 异步写循环状态机（严格串行化发送队列中的数据）
void TcpSession::DoWrite() {
    auto self(shared_from_this());
    // 发送队列头部的第一块数据
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
        [this, self](const std::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (is_closed_) {
                // socket 关闭后，当前 async_write 的 buffer 生命周期到此结束，
                // 现在可以安全释放整个队列并回收其背压计数。
                ClearWriteQueue();
                return;
            }

            if (!ec) {
                // 发送成功：从原子计数器中扣减已发送字节数
                current_send_queue_size_.fetch_sub(write_queue_.front().size(), std::memory_order_relaxed);
                // 弹出已发送完成的数据包
                write_queue_.pop_front();

                // 如果队列中还有后续排队的数据包，继续触发下一次 async_write
                if (!write_queue_.empty()) {
                    DoWrite();
                }
            } else {
                // 当前写操作已经完成（失败），所有队列 buffer 都不再被底层 I/O 使用。
                ClearWriteQueue();
                // 发送失败，进入错误处理
                HandleError(ec);
            }
        });
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

// 刷新并重置心跳定时器
void TcpSession::ResetHeartbeatTimer() {
    if (heartbeat_timeout_s_ <= 0) return; // 禁用心跳

    // 设置过期时间点为当前时间 + 超时秒数
    heartbeat_timer_.expires_after(std::chrono::seconds(heartbeat_timeout_s_));

    auto self(shared_from_this());
    heartbeat_timer_.async_wait([this, self](const std::error_code& ec) {
        if (!ec) {
            // 定时器正常触发（未被中途 cancel 且未发生错误），说明超时时间内没有收到任何数据包
            std::cerr << "[TcpSession] Heartbeat timeout from " << remote_address_ << ":" << remote_port_ << ", closing." << std::endl;
            Close();
        }
        // 如果 ec == asio::error::operation_aborted 说明被 ResetHeartbeatTimer() 重新刷新或连接已关闭，属正常现象
    });
}

// 统一网络 I/O 错误处理
void TcpSession::HandleError(const std::error_code& ec) {
    // operation_aborted 是主动 cancel 引起的操作取消，不需要触发用户错误回调
    if (ec == asio::error::operation_aborted) {
        return;
    }

    // 触发用户的错误回调
    if (on_error_) {
        on_error_(shared_from_this(), ec);
    }
    // 发生网络故障后，自动触发关闭收尾流程
    Close();
}

} // namespace net
