# C++11 TCP Server 架构与设计文档

本文档详细介绍了基于 C++11 和 **Standalone Asio** 构建的高性能 TCP 服务端的设计理念、线程模型、核心状态机机制、并发控制以及优雅停机流程。

---

## 1. 架构概览与线程模型

本项目采用业界标准的高性能网络并发模型 —— **主从 Reactor 模式（One Loop Per Thread）**。

```mermaid
graph TD
    subgraph AcceptorThread["主 Acceptor 线程 (main_io_context_)"]
        Acceptor["asio::ip::tcp::acceptor (DoAccept)"]
    end

    subgraph IOThreadPool["从 Reactor 线程池 (IOThreadPool)"]
        IO1["Worker Thread 0 (io_context 0)"]
        IO2["Worker Thread 1 (io_context 1)"]
        IO3["Worker Thread N (io_context N)"]
    end

    Acceptor -->|Round-Robin 轮询分配| IO1
    Acceptor -->|Round-Robin 轮询分配| IO2
    Acceptor -->|Round-Robin 轮询分配| IO3

    subgraph TcpSessionCycle["单个 TcpSession 的生命周期 (严格绑定在单一 IO 线程)"]
        ReadOp["DoRead() 异步读循环"] -->|收包| ResetTimer["ResetHeartbeatTimer()"]
        ResetTimer --> OnMsg["触发 OnMessageHandler"]

        SendCall["业务线程 Send()"] --> Backpressure["原子高水位背压检测"]
        Backpressure -->|asio::post| WriteTask["IO 线程串行执行 SendTask"]
        WriteTask --> DoWrite["DoWrite() 异步写队列"]

        Timer["heartbeat_timer_ (心跳超时)"] --> Close["Close() 优雅关闭"]
    end

    IO1 -.->|托管运行| TcpSessionCycle
```

### 1.1 线程角色划分
1. **主 Reactor 线程（Acceptor Thread）**：
   - 拥有独立的 `main_io_context_`。
   - 仅负责监听套接字（`acceptor_`）的 `async_accept` 事件，确保新连接接入不被任何繁重的读写业务阻塞。
2. **从 Reactor 线程池（IO Worker Threads）**：
   - 线程池内每个线程拥有独立且唯一的 `io_context` 实例。
   - 彼此**无锁竞争**，独立进行 `epoll/kqueue` 事件轮询。
   - 新建连接通过轮询（Round-Robin）分配到某个 `io_context` 上，后续该连接的所有读、写、心跳定时器事件都在该特定线程内串行执行。

---

## 2. 核心组件与职责

```mermaid
classDiagram
    class TcpServer {
        -asio::io_context main_io_context_
        -IOThreadPool io_thread_pool_
        -asio::ip::tcp::acceptor acceptor_
        -unordered_set~TcpSessionPtr~ active_sessions_
        +Start() void
        +Stop() void
        +SetOnConnect(cb) void
        +SetOnMessage(cb) void
        +SetOnClose(cb) void
        +SetOnError(cb) void
    }

    class IOThreadPool {
        -vector~shared_ptr~io_context~~ io_contexts_
        -vector~work_guard~ work_guards_
        -vector~thread~ threads_
        +Start() void
        +Stop() void
        +GetNextIOContext() io_context&
        +IsCurrentThread() bool
    }

    class TcpSession {
        -asio::ip::tcp::socket socket_
        -asio::steady_timer heartbeat_timer_
        -deque~vector~uint8_t~~ write_queue_
        -atomic~size_t~ current_send_queue_size_
        -atomic~bool~ is_closed_
        +Start() void
        +Send(data, len) void
        +Close() void
        +GetSocket() socket&
    }

    TcpServer *-- IOThreadPool
    TcpServer o-- TcpSession : 管理活跃连接生命周期
```

### 2.1 `IOThreadPool`（IO 线程池）
* **WorkGuard 机制**：每个 `io_context` 初始化时绑定一个 `executor_work_guard`，保证在没有网络连接时工作线程也不会退出 `run()`。
* **优雅停止**：`Stop()` 阶段首先 `reset()` 释放 WorkGuard，然后显式调用 `stop()` 强行唤醒阻塞中的线程，最后逐个 `join()` 回收。

### 2.2 `TcpSession`（TCP 会话状态机）
* 继承自 `std::enable_shared_from_this<TcpSession>`，封装单个客户端连接的完整生命周期。
* 内置异步读循环（`DoRead`）、链式发送队列（`DoWrite`）、原子背压流量控制和空闲心跳检测（`heartbeat_timer_`）。

### 2.3 `TcpServer`（服务端管理器）
* 管理主监听套接字与线程池生命周期。
* 持有 `active_sessions_` 全局活跃会话集合，提供停机时的会话快照与排空等待机制。

---

## 3. 关键机制与并发安全设计

### 3.1 异步生命周期管理（彻底杜绝 Use-After-Free）
* **痛点**：在异步网络编程中，如果发起 `async_read` 或 `async_write` 后对象被上层析构，回调执行时访问成员变量会引发崩溃。
* **解决**：在发起每个 Asio 异步调用时，通过 `auto self = shared_from_this();` 将自身的引用计数传递给 Lambda 闭包。只要内核还有挂起的 I/O 操作，`TcpSession` 对象就绝对不会被释放。

```cpp
// 典型的生命周期绑定范式
void TcpSession::DoRead() {
    auto self(shared_from_this()); // 引用计数 +1，确保回调返回前 Session 绝不析构
    socket_.async_read_some(asio::buffer(read_buffer_),
        [this, self](const std::error_code& ec, std::size_t bytes_transferred) {
            // 回调执行期间对象安全存活
        });
}
```

---

### 3.2 并发发送与任务串行化（解决 Asio 并发写 UB）
* **痛点**：Asio 底层规定在同一个 socket 上并发发起多个 `async_write` 是未定义行为（UB）。
* **解决**：
  1. `Send()` 接口开放给任意外部业务线程；
  2. 构造 Session 时缓存 socket 的 executor，内部通过 `asio::post(executor_, ...)` 将发送任务投递到绑定的单一 IO 线程中，避免业务线程访问共享 socket；
  3. 在 IO 线程内维护 `write_queue_`，严格保证上一个 `async_write` 完成触发回调后，才发起下一次 `async_write`。
  4. 多个业务线程并发调用 `Send()` 时只保证写操作不会重叠，不承诺这些调用之间的全局发送顺序；需要严格顺序的协议应在业务层编号或统一投递。

```mermaid
sequenceDiagram
    participant Biz as 业务线程
    participant IO as Session 绑定的 IO 线程
    participant Socket as 底层 Socket

    Biz->>Biz: 1. 深拷贝数据到 buffer
    Biz->>IO: 2. asio::post(SendTask)
    Note over IO: 进入 IO 线程串行执行
    IO->>IO: 3. write_queue_.push_back(buffer)
    alt 当前无正在进行的写操作
        IO->>Socket: 4. async_write(queue.front())
        Socket-->>IO: 5. 写入完成回调 (pop_front)
        IO->>Socket: 6. 若队列非空，链式触发下一次 async_write
    else 当前已有写操作在进行中
        Note over IO: 仅入队等待，由上一任务的回调自动触发
    end
```

---

### 3.3 发送背压与防 OOM 水位控制（Backpressure）
* **痛点**：当客户端网络极慢（或恶意不读取数据），而服务端高频调用 `Send()` 时，发送队列会无限膨胀，最终导致服务端内存溢出（OOM）。
* **解决**：
  - 引入 `current_send_queue_size_`（原子计数）和 `max_send_queue_size_`（默认 10MB）。
  - 每次 `Send()` 时通过 CAS 原子预留待发送字节数，使用减法比较避免 `size_t` 溢出，并保证并发调用不会突破阈值。
  - 数据拷贝、任务投递、入队或异步写失败，以及关闭期间丢弃待发送数据时，都会回收对应计数。

---

### 3.4 优雅停机与排空机制（Graceful Shutdown）
服务端关闭（`TcpServer::Stop()`）遵循严格的时序保证：

```mermaid
sequenceDiagram
    participant Main as 控制主线程
    participant Server as TcpServer
    participant Session as 活跃 TcpSession
    participant IO as IO 线程池

    Main->>Server: Stop()
    Server->>Server: 防死锁检查 (禁止在 IO 线程内调用)
    Server->>Server: 抓取 active_sessions_ 快照
    loop 遍历所有活跃会话
        Server->>Session: session->Close(completion_promise)
    end
    Server->>Server: 等待所有会话关闭完成 (最多 3 秒宽限期)
    Server->>Server: acceptor_.close() 停止接收新连接
    Server->>IO: io_thread_pool_.Stop() (释放 work_guard 并 stop)
    Server->>Main: 所有资源安全回收，Stop() 返回
```

---

## 4. API 契约与回调规范

| 回调函数 | 触发线程 | 时机与注意事项 |
| :--- | :--- | :--- |
| `OnConnectHandler` | **主 Acceptor 线程** | 新客户端握手成功后触发。回调完成后，会话才会在 IO 线程启动读取。 |
| `OnMessageHandler` | **Session 归属 IO 线程** | 收到新数据时触发。`data` 仅在回调函数返回前有效，如需异步跨线程使用**必须自行深拷贝**。 |
| `OnCloseHandler` | **Session 归属 IO 线程** | 当底层 socket 和定时器完全关闭后触发，**保证每个连接只触发一次**。 |
| `OnErrorHandler` | **Session 归属 IO 线程** | 发生非主动取消的网络异常时触发，随后框架会自动进入关闭流程。 |

---

## 5. 快速上手示例

```cpp
#include "net/tcp_server.h"
#include <iostream>

int main() {
    unsigned short port = 8888;
    int heartbeat_timeout = 30; // 30秒无数据自动断开空闲连接

    // 初始化服务端：4 个工作线程
    net::TcpServer server(port, 4, heartbeat_timeout);

    // 1. 收到数据时 Echo 回显
    server.SetOnMessage([](net::TcpSessionPtr session, const uint8_t* data, std::size_t len) {
        std::string msg(reinterpret_cast<const char*>(data), len);
        session->Send("[Echo] " + msg); // 线程安全发送
    });

    // 2. 客户端连接/断开事件监听
    server.SetOnConnect([](net::TcpSessionPtr s) {
        std::cout << "Client connected: " << s->GetRemoteAddress() << ":" << s->GetRemotePort() << std::endl;
    });
    server.SetOnClose([](net::TcpSessionPtr s) {
        std::cout << "Client closed: " << s->GetRemoteAddress() << std::endl;
    });

    // 3. 启动服务端（非阻塞调用）
    server.Start();

    // 运行...
    // server.Stop(); // 优雅停止
    return 0;
}
```

---

## 6. 后续演进建议

1. **应用层协议编解码（Codec）**：建议在 `OnMessageHandler` 上层封装基于固定长度包头（如 4 字节 Payload Length）的 `LengthFieldCodec`，以便处理 TCP 流式通信中的粘包和拆包问题。
2. **高频小包内存池优化**：若面对数百万 QPS 的极端小包场景，可引入环形缓冲区（RingBuffer）或 Slice 内存池减少 `std::vector` 的内存分配开销。
3. **超大规模连接心跳优化**：当连接规模达到 10 万+ 时，可引入**时间轮算法（Timing Wheel）**替代单 Session 独立定时器以降低调度开销。
